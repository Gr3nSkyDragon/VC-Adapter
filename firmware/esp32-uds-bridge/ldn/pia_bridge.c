#include "pia_bridge.h"
#include "pia_link.h"
#include "pico_link.h"
#include "trade_shim.h"
#include "ldn_control.h"
#include "ldn_session.h"
#include "ldn_udp.h"
#include "ldn_wire.h"
#include "ldn_host.h"
#include "pia_host.h"
#include "transport.h"

#include <stdio.h>
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_wifi.h"

/* Bare text would corrupt binary-mode frames. */
#define printf ldn_wire_printf

#define RFU1_HOST_SEND 5u
#define RFU1_CLIENT_SEND 6u
#define RFU1_CONNECT_REQ 1u
#define RFU1_CONNECT_ACK 2u
#define RFU1_CONNECT_NACK 3u
#define RFU1_DISCONNECT 4u
#define RFU1_CLIENT_ACK 7u
#define RFU1_BROADCAST 0u

/* BR_HOST: the GBA leads a group and the board hosts the Switch's room. */
enum { BR_IDLE, BR_SCAN, BR_AUTH, BR_MEMBER, BR_NET, BR_RUN, BR_HOST };

#define FIRERED_ID 0x01006fa0233f8000ULL
/* No GBA beacon for this long closes the group (only before a member joins: EndHost stops
   beacons). */
#define BEACON_TIMEOUT_MS 3000
/* The adapter drops a client silent for 4 s; keepalive interval. */
#define CLIENT_ACK_MS 500
/* Room kept up after the GBA closes the link, for the Switch's own close; then the client
   is dropped and the teardown given time. */
#define CLOSE_GRACE_MS 15000
#define TEARDOWN_MS 6000

/* Console activity counts as a live adapter link for this long. */
#define CONSOLE_LIVE_MS 10000

/* pokefirered constants/union_room.h: trade, single battle, double battle. */
#define GROUP_COUNT 3
static const uint8_t kGroupActivity[GROUP_COUNT] = {4, 1, 2};

static struct
{
    int state;
    bool started;
    ldn_network_t net;
    ldn_auth_t auth;
    /* Joiner and host state share memory; one runs at a time. */
    union
    {
        pia_link_t link;
        struct { pia_host_t session; ldn_host_scratch_t radio; ldn_host_config_t cfg; } hosting;
    } s;
    uint8_t our_mac[6];
    char our_ip[16], host_ip[16];
    int64_t next_scan, next_auth, next_beacon, deadline;
    int64_t next_tick_us, tick_fraction;   /* 59.727 Hz tick: microseconds plus parts per 59727 */
    uint16_t stall_low;
    int64_t stall_since;
    bool stall_reported;
    uint8_t net_type;
    uint32_t net_seq;
    bool net_unacked_noted;
    int channel_index, auth_attempts;
    bool authenticated, have_room, child_connected, session_up;
    int64_t last_host_frame_ms;
    uint32_t last_host_time;
    uint16_t host_skips, host_long_skips;
    int unzip_seen;
    int64_t last_unzip_ms;
    bool host_silent;
    uint16_t child_devid;
    /* One advertised group per activity, each with its own devid. The GBA only lists the
       one matching its player's choice and its connect request names it, so the bridge
       follows the GBA. */
    uint16_t group_devid[GROUP_COUNT];
    uint8_t beacon[GROUP_COUNT][24];
    uint8_t room_activity;              /* the Switch's group, from its room record; 0 unknown */
    int group;                          /* the one the GBA connected to, -1 none */
    int frames_to_gba, frames_from_gba, to_gba_lost;
    int64_t session_ms;                 /* when the Switch session began, for the summary */
    int switch_skipped;                 /* joiner role: frames the Switch's counter skipped */

    /* Hosting: the GBA's group as advertised, and the Switch as its client. */
    pia_host_t *host;                   /* &g.s.hosting.session while hosting */
    uint16_t gba_devid;
    uint8_t gba_beacon[24];
    int64_t gba_beacon_ms;
    uint16_t client_devid;
    uint8_t client_slot;
    bool client_requested, client_connected;
    int member_index;                   /* the LDN member the session serves, -1 none */
    bool link_noted;                    /* the adapter link's state as last printed */
    int64_t next_client_ack;
    uint8_t host_channel;
    int64_t closing_ms;                 /* when the GBA closed the link, 0 while it is open */
} g;

/* Back-off for a room whose handshake failed, doubling per failure. Each attempt joins as
   a new station, so retrying a silent Switch only piles up dead stations. Keyed by SSID,
   so a new room is never held back. Survives the restart. */
static RTC_NOINIT_ATTR struct { uint32_t magic; uint8_t ssid[16]; uint8_t failures; } s_retry;
#define RETRY_MAGIC 0x52455452u

static int64_t retry_hold_ms(void)
{
    if (s_retry.magic != RETRY_MAGIC || s_retry.failures == 0) return 0;
    const int step = s_retry.failures > 4 ? 3 : s_retry.failures - 1;
    return 15000LL << step;   /* 15, 30, 60, 120 s */
}

static const unsigned kChannels[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10};
#define CHANNEL_COUNT (sizeof(kChannels) / sizeof(kChannels[0]))

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- RFU1 framing ---------------------------------------------------------- */

static const uint8_t kRfu1[4] = {'R', 'F', 'U', '1'};

static int rfu1_size(uint32_t type)
{
    if (type == RFU1_BROADCAST) return 36;
    if (type == RFU1_HOST_SEND || type == RFU1_CLIENT_SEND) return 104;
    return 16;
}

static void to_pico(const uint8_t *frame, size_t length)
{
    /* Sent in 64-byte chunks, the adapter's receive buffer size; it reassembles. */
    for (size_t o = 0; o < length; o += 64)
    {
        size_t n = length - o < 64 ? length - o : 64;
        if (!pico_link_send(PICO_LINK_CHANNEL_DATA, frame + o, n)) ++g.to_gba_lost;
    }
    ++g.frames_to_gba;
}

static void send_command(uint32_t type, uint32_t header)
{
    uint8_t f[16] = {0};
    memcpy(f, kRfu1, 4);
    bin_wb32(f + 4, type);
    bin_wb32(f + 8, header);
    to_pico(f, sizeof(f));
}

static void send_host_frame(const uint8_t *payload, size_t length)
{
    uint8_t f[104] = {0};
    memcpy(f, kRfu1, 4);
    bin_wb32(f + 4, RFU1_HOST_SEND);
    bin_wb32(f + 8, (uint32_t)(length & 0x7F));
    memcpy(f + 12, payload, length > 92 ? 92 : length);
    to_pico(f, sizeof(f));
}

static void send_beacon(void)
{
    for (int k = 0; k < GROUP_COUNT; ++k)
    {
        if (g.room_activity && kGroupActivity[k] != g.room_activity) continue;
        uint8_t f[36] = {0};
        memcpy(f, kRfu1, 4);
        bin_wb32(f + 4, RFU1_BROADCAST);
        const bool occupied = g.child_connected && g.group == k;
        bin_wb32(f + 8, (uint32_t)(g.group_devid[k] | (occupied ? 1u : 0u) << 16));
        for (int i = 0; i < 6; ++i) bin_wb32(f + 12 + i * 4, bin_u32(g.beacon[k] + i * 4));
        to_pico(f, sizeof(f));
    }
}

/* Name to the game's charset: letters and digits only. */
static void game_name(const char *name, uint8_t *out, int size)
{
    int n = 0;
    for (const char *c = name; *c && n < size - 1; ++c)
    {
        if (*c >= 'A' && *c <= 'Z') out[n++] = (uint8_t)(*c - 'A' + 0xbb);
        else if (*c >= 'a' && *c <= 'z') out[n++] = (uint8_t)(*c - 'a' + 0xd5);
        else if (*c >= '0' && *c <= '9') out[n++] = (uint8_t)(*c - '0' + 0xa1);
        else out[n++] = 0;
    }
    out[n] = 255;
}

/* Used when the Switch's room record cannot be read: English FireRed, can link nationally,
   national dex and game clear. */
#define BEACON_COMPATIBILITY (2 | (1 << 7) | (1 << 8) | (1 << 9) | (4 << 10))

static void beacon_of_record(uint8_t *beacon, const uint8_t *rec);

/* The Switch's group as the GBA sees it: the room record if readable, else the LDN name
   and fixed values. */
static void build_beacon(uint8_t *beacon, const char *name, const uint8_t *record, uint8_t activity)
{
    memset(beacon, 0, 24);
    if (record) beacon_of_record(beacon, record);
    else
    {
        bin_w16(beacon + 2, BEACON_COMPATIBILITY);
        bin_w16(beacon + 4, 0x1234);
        game_name(name, beacon + 16, 8);
    }
    bin_w16(beacon, 0x0002);                 /* RFU_SERIAL_GAME */
    beacon[12] = activity;                   /* not started */
    int sum = 0;
    for (int i = 0; i < 8; ++i) sum += beacon[16 + i] + beacon[2 + i];
    beacon[15] = (uint8_t)~sum;
}

/* ---- the room's application data ---------------------------------------------- */

/* Room application data: Pia's 92-byte system header, then the room record as base-85
   text. */
#define APP_HEADER 92
#define APP_RECORD 24
#define PIA_SYSTEM_VERSION 22
#define GAME_VERSION 88
#define SCENE_ID 22287

static uint8_t base85_digit(uint32_t v)
{
    uint8_t c = (uint8_t)(0x23 + v % 85);
    return c >= 0x5c ? (uint8_t)(c + 1) : c;   /* skips the backslash */
}

/* Decode the room record from application data. */
static bool read_room_record(const uint8_t *app, int length, uint8_t rec[APP_RECORD])
{
    if (length < APP_HEADER + APP_RECORD / 4 * 5 || bin_b16(app) != APP_HEADER) return false;
    const uint8_t *text = app + APP_HEADER;
    for (int g = 0; g < APP_RECORD / 4; ++g)
    {
        uint32_t v = 0;
        for (int d = 4; d >= 0; --d)
        {
            const uint8_t c = text[g * 5 + d];
            if (c < 0x23 || c == 0x5c || c > 0x78) return false;
            v = v * 85 + (uint32_t)(c < 0x5c ? c - 0x23 : c - 0x24);
        }
        bin_w32(rec + g * 4, v);
    }
    return true;
}

/* Four-byte little-endian groups, five digits each, least significant digit first. */
static int base85_encode(const uint8_t *in, int length, uint8_t *out)
{
    int n = 0;
    for (int i = 0; i + 4 <= length; i += 4)
    {
        uint32_t v = bin_u32(in + i);
        for (int d = 0; d < 5; ++d, v /= 85) out[n++] = base85_digit(v);
    }
    return n;
}

/* The trainer name in a beacon, as text. */
static void text_of_uname(const uint8_t *uname, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < 8 && n + 1 < cap; ++i)
    {
        uint8_t c = uname[i];
        if (c == 0xff) break;
        if (c >= 0xbb && c <= 0xd4) out[n++] = (char)('A' + c - 0xbb);
        else if (c >= 0xd5 && c <= 0xee) out[n++] = (char)('a' + c - 0xd5);
        else if (c >= 0xa1 && c <= 0xaa) out[n++] = (char)('0' + c - 0xa1);
        else if (c == 0) out[n++] = ' ';
        else out[n++] = '?';
    }
    out[n] = 0;
    if (n == 0) snprintf(out, cap, "GBA");
}

/* RFU beacon (the GBA's): serial at 0, then RfuGameData: compatibility word at 2 (language
   0-3, has news 4, has card 5, can link nationally 7, national dex 8, game clear 9,
   version 10-13), trainer id 4, partner info 6-9, trade species 10 (10 bits) and type
   (6 bits), activity 12 (bit 7 started), gender and trade level 13; name at 16.
   Room record (the Switch's): trainer id 0, name 2, adapter id 10, partner info 12-15,
   word 16 (activity 0-6, bit 7, version 8-10, language 11-13, has card 14, started 15),
   18 (progress 0-1, trade type 2-7), gender and trade level 19, trade species 22.
   The progress bits are both set in a finished game's room and both clear in the rooms
   of games without the National Dex, so both set stands for "can link nationally". Bit 7
   of word 16 is not that flag: it is set in rooms whose progress bits are clear. */
static void record_of_beacon(uint8_t *rec, const uint8_t *beacon, uint16_t adapter_id, bool started)
{
    const uint16_t compat = bin_u16(beacon + 2);
    const uint8_t nationally = (compat & 0x80) ? 3 : 0;
    memset(rec, 0, APP_RECORD);
    memcpy(rec, beacon + 4, 2);
    memcpy(rec + 2, beacon + 16, 8);
    bin_w16(rec + 10, adapter_id);
    memcpy(rec + 12, beacon + 6, 4);
    bin_w16(rec + 16, (uint16_t)((beacon[12] & 0x7f) | (compat & 0x80) | ((compat >> 10) & 7) << 8 |
                                 (compat & 7) << 11 | (compat & 0x20) << 9 | (started ? 0x8000 : 0)));
    rec[18] = (uint8_t)((beacon[11] & 0xfc) | ((compat >> 8) & 3) | nationally);
    rec[19] = beacon[13];
    bin_w16(rec + 22, bin_u16(beacon + 10) & 0x3ff);
}

static void beacon_of_record(uint8_t *beacon, const uint8_t *rec)
{
    const uint16_t word = bin_u16(rec + 16);
    const uint16_t nationally = (rec[18] & 3) == 3 ? 0x80 : 0;
    bin_w16(beacon + 2, (uint16_t)(((word >> 11) & 7) | ((word >> 9) & 0x20) | nationally |
                                   (rec[18] & 3) << 8 | ((word >> 8) & 7) << 10));
    memcpy(beacon + 4, rec, 2);
    memcpy(beacon + 6, rec + 12, 4);
    bin_w16(beacon + 10, (uint16_t)((bin_u16(rec + 22) & 0x3ff) | (rec[18] & 0xfc) << 8));
    beacon[13] = rec[19];
    memcpy(beacon + 16, rec + 2, 8);
}

static int build_app_data(uint8_t *app, const uint8_t *beacon, uint16_t adapter_id, int players, bool started)
{
    memset(app, 0, APP_HEADER);
    bin_wb16(app, APP_HEADER);
    app[2] = PIA_SYSTEM_VERSION;
    bin_wb16(app + 3, GAME_VERSION);
    app[0x15] = 1;                              /* player limit on */
    app[0x16] = (uint8_t)players;
    char name[33];
    text_of_uname(beacon + 16, name, sizeof(name));
    size_t n = strlen(name);
    bin_wb32(app + 0x17, (uint32_t)n);
    app[0x1b] = 1;                              /* UTF-8 */
    memcpy(app + 0x1c, name, n);
    uint8_t rec[APP_RECORD];
    record_of_beacon(rec, beacon, adapter_id, started);
    return APP_HEADER + base85_encode(rec, APP_RECORD, app + APP_HEADER);
}

static void enter_host_mode(void);
static void bridge_restart(bool session_ended);

/* A live adapter link: the adapter in wireless mode on the UART or a GBA clocking it, or
   recent console traffic while the host port is in use. Rooms are joined and hosted only
   with one. */
static bool adapter_link_live(void)
{
    if (pico_link_port() == PICO_PORT_HOST) return ldn_control_console_age_ms() < CONSOLE_LIVE_MS;
    return pico_link_await_mode() || pico_link_gba_active();
}

static void note_link(void)
{
    const bool live = adapter_link_live();
    if (live == g.link_noted) return;
    g.link_noted = live;
    printf(live ? "LDN_BRIDGE looking for a FireRed room\n" : "LDN_BRIDGE waiting for an adapter link before joining a room\n");
}

/* ---- callbacks ------------------------------------------------------------- */

static void on_send(const uint8_t *datagram, size_t length, const char *destination, void *user)
{
    (void)user;
    ldn_udp_send(destination, datagram, length);
}

static void on_deliver(const uint8_t *frame, size_t length, void *user)
{
    (void)user;
    /* Byte 8 is the send length, payload from byte 12. */
    if (length < 12) return;
    size_t n = frame[8] & 0x7F;
    if (12 + n > length) n = length - 12;
    if (n > 92) n = 92;
    static uint8_t payload[92];
    memcpy(payload, frame + 12, n);
    int64_t now = now_ms();
    if (g.last_host_frame_ms && now - g.last_host_frame_ms > 250)
        trade_shim_note(now, TRADE_SHIM_NOTE_HOST_SILENCE, (uint16_t)(now - g.last_host_frame_ms), 1);
    g.host_silent = false;
    /* The Switch's frame counter; a jump is a stall, during which it does not drain its
       20-slot receive queue. */
    uint32_t stamp = bin_u32(frame + 4);
    uint32_t jump = stamp - g.last_host_time;
    if (g.last_host_time && jump >= 2 && jump < 1000)
    {
        g.switch_skipped += (int)jump - 1;
        if (jump == 2) ++g.host_skips;
        else
        {
            ++g.host_long_skips;
            trade_shim_note(now, TRADE_SHIM_NOTE_SWITCH_CLOCK_SKIP, (uint16_t)jump, (uint16_t)(now - g.last_host_frame_ms));
        }
    }
    g.last_host_time = stamp;
    g.last_host_frame_ms = now;
    static uint8_t pre[8 * 73];
    size_t r = trade_shim_host(payload, n, now, pre, sizeof(pre));
    for (size_t o = 0; o + 73 <= r; o += 73) send_host_frame(pre + o, 73);
    send_host_frame(payload, n);
}

static void on_log(const char *message, void *user) { (void)user; printf("LDN_BRIDGE %s\n", message); }

static void on_datagram(const char *ip, const uint8_t *data, size_t length)
{
    if (g.state == BR_RUN) pia_link_receive(&g.s.link, data, length, ip);
    else if (g.state == BR_HOST) pia_host_receive(g.host, data, length, ip);
}

/* ---- hosting: the Switch as the GBA group's client ------------------------------ */

/* Shim-built parent frames to the Switch. */
static void to_child(const uint8_t *frames, size_t length)
{
    for (size_t o = 0; o + 73 <= length; o += 73) pia_host_parent_frame(g.host, frames + o, 73);
}

/* A Switch child frame to the GBA's adapter, through the shim. */
static void on_child_frame(const uint8_t *payload, size_t length, void *user)
{
    (void)user;
    if (!g.client_connected || length > 16) return;
    uint8_t f[104] = {0};
    memcpy(f, kRfu1, 4);
    bin_wb32(f + 4, RFU1_CLIENT_SEND);
    bin_wb32(f + 8, (uint32_t)g.client_devid | ((uint32_t)g.client_slot << 16) | ((uint32_t)length << 24));
    memcpy(f + 12, payload, length);
    static uint8_t reply[146];
    bool forward = true;
    size_t r = trade_shim_lead_child(f + 12, length, now_ms(), reply, sizeof(reply), &forward);
    if (forward) to_pico(f, sizeof(f));
    to_child(reply, r);
}

/* Rebuild the application data: the GBA's beacon, the member count, and started once a
   client is in (the adapter stops beaconing then). */
static void update_app_data(void)
{
    if (!g.host) return;
    const bool started = (g.gba_beacon[12] & 0x80) || g.client_connected;
    ldn_host_config_t *cfg = &g.s.hosting.cfg;
    cfg->app_data_len = build_app_data(cfg->app_data, g.gba_beacon, g.gba_devid, 1 + ldn_host_member_count(), started);
    ldn_host_set_app_data(cfg->app_data, cfg->app_data_len);
    pia_host_set_app_data(g.host, cfg->app_data, cfg->app_data_len);
}

static void on_member_joined(const ldn_host_member_t *m)
{
    if (g.host->state != 0)
    {
        printf("LDN_BRIDGE Switch %s joined the room too; only the first one is served\n", m->name);
        return;
    }
    printf("LDN_BRIDGE Switch %s joined the room as %s (%d)\n", m->name, m->ip, m->index);
    g.member_index = m->index;
    update_app_data();
    pia_host_station_joined(g.host, m->mac, m->ip, m->index);
}

static void rate(char *out, size_t cap, int count, int64_t ms)
{
    const int64_t tenths = ms > 0 ? (int64_t)count * 10000 / ms : 0;
    snprintf(out, cap, "%d.%d", (int)(tenths / 10), (int)(tenths % 10));
}

/* One line at the end of a session, the same for both roles. */
static void print_session_summary(void)
{
    const bool host = g.state == BR_HOST;
    const int64_t ms = now_ms() - g.session_ms;
    int dg_in, dg_out, skipped, shed, dropped, resends;
    if (host)
    {
        const pia_host_t *h = g.host;
        dg_in = h->received; dg_out = h->sent; skipped = h->child_skipped;
        shed = h->shed; dropped = h->parent_dropped; resends = h->resends;
    }
    else
    {
        const pia_link_t *l = &g.s.link;
        dg_in = l->received; dg_out = l->sent; skipped = g.switch_skipped;
        shed = l->shed; dropped = l->overflow; resends = -1;
    }
    uint8_t queue_high = 0, sheds = 0;
    uint16_t queue_drops = 0, overflows = 0;
    trade_shim_adapter_counts(&queue_high, &queue_drops, &sheds, &overflows);
    uint32_t resync = 0, inbound_lost = 0;
    pico_link_stats(NULL, NULL, NULL, &resync, &inbound_lost);
    char in[16], out[16], to_gba[16], from_gba[16], skip[16];
    rate(in, sizeof(in), dg_in, ms);
    rate(out, sizeof(out), dg_out, ms);
    rate(to_gba, sizeof(to_gba), g.frames_to_gba, ms);
    rate(from_gba, sizeof(from_gba), g.frames_from_gba, ms);
    rate(skip, sizeof(skip), skipped, ms);
    printf("LDN_BRIDGE session (%s leading) %d s: Switch datagrams in %s/s out %s/s, frames to GBA %s/s from GBA %s/s, "
           "Switch skipped %s/s, board shed %d dropped %d resends %d, adapter queue_high %u sheds %u drops %u uart_lost %u, "
           "board resync %u lost %u+%d+%u, damaged from the page %u, lowest heap %u\n",
           host ? "GBA" : "Switch", (int)(ms / 1000), in, out, to_gba, from_gba, skip, shed, dropped, resends,
           queue_high, sheds, queue_drops, overflows, (unsigned)resync, (unsigned)inbound_lost, g.to_gba_lost,
           (unsigned)bridge_transport_dropped(), (unsigned)ldn_wire_bad_frames(),
           (unsigned)esp_get_minimum_free_heap_size());
}

static void on_member_left(const ldn_host_member_t *m)
{
    if (g.host->state == 0 || m->index != g.member_index) return;
    printf("LDN_BRIDGE Switch %s left the room\n", m->name);
    print_session_summary();
    g.member_index = -1;
    pia_host_station_left(g.host);
    if (g.client_connected)
        send_command(RFU1_DISCONNECT, (uint32_t)g.client_devid | ((uint32_t)g.client_slot << 16));
    g.client_connected = g.client_requested = false;
    g.closing_ms = 0;
    update_app_data();
}

static void enter_host_mode(void)
{
    g.host = &g.s.hosting.session;
    ldn_session_stop();
    ldn_host_config_t *cfg = &g.s.hosting.cfg;
    memset(cfg, 0, sizeof(*cfg));
    cfg->communication_id = FIRERED_ID;
    cfg->scene_id = SCENE_ID;
    cfg->app_version = GAME_VERSION;
    cfg->maximum = 6;
    static const uint8_t kHostChannels[3] = {1, 6, 11};
    cfg->channel = kHostChannels[esp_random() % 3];
    text_of_uname(g.gba_beacon + 16, cfg->host_name, sizeof(cfg->host_name));
    cfg->app_data_len = build_app_data(cfg->app_data, g.gba_beacon, g.gba_devid, 1, (g.gba_beacon[12] & 0x80) != 0);
    ldn_host_set_handlers(on_member_joined, on_member_left);
    if (!ldn_host_start(cfg, &g.s.hosting.radio))
    {
        printf("LDN_BRIDGE could not host a room\n");
        bridge_restart(false);
        return;
    }
    uint8_t mac[6];
    ldn_host_mac(mac);
    pia_host_init(g.host, ldn_host_ssid(), mac, ldn_host_ip(), on_send, on_child_frame, on_log, NULL);
    /* The WA carries the adapter id the record names. */
    g.host->adapter_id = g.gba_devid;
    g.host->sheddable = trade_shim_parent_sheddable;
    pia_host_set_app_data(g.host, cfg->app_data, cfg->app_data_len);
    g.client_requested = g.client_connected = false;
    g.member_index = -1;
    g.state = BR_HOST;
    g.next_tick_us = esp_timer_get_time();
    g.tick_fraction = 0;
    printf("LDN_BRIDGE GBA %s is leading activity %u: hosting a room on channel %d\n",
           cfg->host_name, (unsigned)(g.gba_beacon[12] & 0x7f), cfg->channel);
}

static void on_action(const uint8_t source[6], const uint8_t *body, size_t length)
{
    if (g.state != BR_AUTH || memcmp(source, g.net.host, 6) != 0) return;
    if (ldn_auth_accept(&g.auth, body, length))
    {
        g.authenticated = true;
        printf("LDN_BRIDGE auth verified\n");
    }
}

/* ---- inbound from the adapter ---------------------------------------------- */

static void connect_child(void)
{
    g.child_devid = (uint16_t)(esp_random() | 1);
    g.child_connected = true;
    /* No shim reset: after a link-loss recovery the games keep their round counters and
       the Switch its sequence. pia_bridge_start resets for a new session. */
    trade_shim_note(now_ms(), TRADE_SHIM_NOTE_CHILD_CONNECT, g.child_devid, 0);
    send_command(RFU1_CONNECT_ACK, g.child_devid);
    printf("LDN_BRIDGE child connected devid=%04x\n", g.child_devid);
}

static void feed_rfu1(const uint8_t *frame, size_t length)
{
    if (length < 12 || memcmp(frame, kRfu1, 4) != 0) return;
    uint32_t type = bin_b32(frame + 4), header = bin_b32(frame + 8);
    if ((size_t)rfu1_size(type) != length) return;
    ++g.frames_from_gba;

    if (type == RFU1_BROADCAST)
    {
        /* The GBA leads a group; beacon words arrive big-endian. */
        uint8_t beacon[24];
        for (int i = 0; i < 6; ++i) bin_w32(beacon + i * 4, bin_b32(frame + 12 + i * 4));
        const bool changed = memcmp(beacon, g.gba_beacon, sizeof(beacon)) != 0 || g.gba_devid != (header & 0xFFFF);
        g.gba_devid = (uint16_t)(header & 0xFFFF);
        memcpy(g.gba_beacon, beacon, sizeof(beacon));
        g.gba_beacon_ms = now_ms();
        if (g.state == BR_SCAN && adapter_link_live()) enter_host_mode();
        else if (g.state == BR_HOST && changed) update_app_data();
        return;
    }
    if (g.state == BR_HOST)
    {
        if (type == RFU1_CONNECT_ACK && g.client_requested && !g.client_connected)
        {
            g.client_devid = (uint16_t)(header & 0xFFFF);
            g.client_slot = (uint8_t)((header >> 16) & 3);
            g.client_connected = true;
            g.session_ms = now_ms();
            g.next_client_ack = now_ms() + CLIENT_ACK_MS;
            pia_host_accept(g.host, true);
            update_app_data();
            printf("LDN_BRIDGE GBA accepted the Switch as client %u devid=%04x\n", g.client_slot, g.client_devid);
        }
        else if (type == RFU1_CONNECT_NACK && g.client_requested && !g.client_connected)
        {
            g.client_requested = false;
            pia_host_accept(g.host, false);
            printf("LDN_BRIDGE GBA refused the Switch\n");
        }
        else if (type == RFU1_HOST_SEND)
        {
            size_t n = header & 0x7F;
            if (n > 92) n = 92;
            static uint8_t payload[92];
            memcpy(payload, frame + 12, n);
            trade_shim_lead_parent(payload, n, now_ms());
            pia_host_parent_frame(g.host, payload, n);
        }
        else if (type == RFU1_DISCONNECT && g.client_connected && (header & 0xFFFF) == g.client_devid)
        {
            g.client_connected = g.client_requested = false;
            g.closing_ms = now_ms();
            update_app_data();
            printf("LDN_BRIDGE GBA closed the link; keeping the room while the Switch finishes\n");
        }
        return;
    }
    if (g.state != BR_RUN) return;

    if (type == RFU1_CONNECT_REQ)
    {
        /* The request names the devid of the group the GBA's player picked. */
        for (int k = 0; k < GROUP_COUNT; ++k)
            if (g.group_devid[k] == (header & 0xFFFF) && g.group != k)
            {
                g.group = k;
                printf("LDN_BRIDGE GBA chose the activity %u group\n", kGroupActivity[k]);
            }
        pia_link_set_connect(&g.s.link, true);
        if (!g.child_connected && g.s.link.accepted) connect_child();
    }
    else if (type == RFU1_CLIENT_SEND)
    {
        size_t n = header >> 24;
        if (n > 92) n = 92;
        static uint8_t payload[92], reply[146];
        memcpy(payload, frame + 12, n);
        bool forward = true;
        size_t r = trade_shim_child(payload, n, now_ms(), reply, sizeof(reply), &forward);
        if (forward) pia_link_enqueue(&g.s.link, payload, n);
        for (size_t o = 0; o + 73 <= r; o += 73) send_host_frame(reply + o, 73);
    }
    else if (type == RFU1_DISCONNECT)
    {
        if (g.child_connected) trade_shim_note(now_ms(), TRADE_SHIM_NOTE_CHILD_DISCONNECT, 0, 0);
        g.child_connected = false;
    }
}

static void drain_adapter(void)
{
    static uint8_t gb[5 + PICO_LINK_MAX_PAYLOAD];
    static uint8_t stream[256];
    static size_t used;
    size_t length = 0;
    for (int i = 0; i < 32 && pico_link_poll_inbound(gb, sizeof(gb), &length); ++i)
    {
        if (length < 5 || gb[2] != PICO_LINK_CHANNEL_DATA) continue;
        size_t n = length - 5;
        /* RFU1 chunks are 64 bytes, zero padded; shorter ones are adapter telemetry. */
        if (n < 64) { trade_shim_adapter(now_ms(), gb + 5, n); continue; }
        if (used + n > sizeof(stream)) used = 0;
        memcpy(stream + used, gb + 5, n);
        used += n;
        /* Fixed size per type: resync on the magic. */
        size_t at = 0;
        while (used - at >= 12)
        {
            if (memcmp(stream + at, kRfu1, 4) != 0) { ++at; continue; }
            int size = rfu1_size(bin_b32(stream + at + 4));
            if (used - at < (size_t)size) break;
            feed_rfu1(stream + at, (size_t)size);
            at += (size_t)size;
        }
        if (at > 0) { memmove(stream, stream + at, used - at); used -= at; }
    }
}

/* ---- state machine --------------------------------------------------------- */

void pia_bridge_start(void)
{
    if (g.started) return;
    memset(&g, 0, sizeof(g));
    g.started = true;
    g.state = BR_SCAN;
    g.group = -1;
    for (int k = 0; k < GROUP_COUNT; ++k)
    {
        bool fresh;
        do
        {
            g.group_devid[k] = (uint16_t)(esp_random() | 1);
            fresh = true;
            for (int j = 0; j < k; ++j) fresh = fresh && g.group_devid[j] != g.group_devid[k];
        } while (!fresh);
    }
    trade_shim_reset();
    trade_shim_note(now_ms(), TRADE_SHIM_NOTE_BRIDGE_START, 0, 0);
    ldn_udp_set_handler(on_datagram);
    ldn_control_set_action_handler(on_action);
    /* Mode entry resets the adapter, so skip it if a GBA is already linked. */
    if (!pico_link_gba_active()) pico_link_set_mode();
    g.link_noted = !adapter_link_live();
    note_link();
}

void pia_bridge_stop(void)
{
    if (!g.started) return;
    trade_shim_note(now_ms(), TRADE_SHIM_NOTE_BRIDGE_STOP, (uint16_t)g.state, 0);
    ldn_udp_set_handler(NULL);
    ldn_control_set_action_handler(NULL);
    ldn_session_stop();
    ldn_host_stop();
    g.host = NULL;
    g.started = false;
    g.state = BR_IDLE;
    printf("LDN_BRIDGE stopped\n");
}

/* A finished session or failed join restarts the board, which resumes the room search.
   After a session the adapter re-enters its mode first, which clears the old room from
   the GBA's list. After the restart this may be skipped: once a GBA has clocked the
   adapter, dbgAnyRx stays set and pico_link leaves it alone. */
static void bridge_restart(bool session_ended)
{
    /* A hosted session can end on the GBA's side, with the Switch still in the room. */
    if (g.state == BR_HOST && g.member_index >= 0 && g.session_ms) print_session_summary();
    if (session_ended)
    {
        pico_link_set_mode();
        pico_link_flush();
        ldn_control_flush_adapter();
    }
    pia_bridge_stop();
    esp_restart();
}

bool pia_bridge_running(void) { return g.started; }
bool pia_bridge_in_session(void) { return g.started && (g.state == BR_RUN || g.state == BR_HOST); }

/* Logged once per stall of the host stream. */
static void check_host_stall(int64_t now)
{
    const int pending = pia_reliable_pending(&g.host->reliable);
    if (pending == 0 || g.host->reliable.low != g.stall_low)
    {
        if (g.stall_reported) printf("LDN_BRIDGE Switch is acknowledging our frames again\n");
        g.stall_low = g.host->reliable.low;
        g.stall_since = now;
        g.stall_reported = false;
        return;
    }
    if (g.stall_reported || now - g.stall_since < 1500) return;
    g.stall_reported = true;
    /* Signal and memory tell a Switch out of range from a board out of buffers. */
    wifi_sta_list_t stations;
    int rssi = 0;
    if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0) rssi = stations.sta[0].rssi;
    printf("LDN_BRIDGE Switch has not acknowledged our frames for %d ms (%d waiting, %d resends, retry after %d ms; "
           "signal %d dBm, heap %u, lowest %u)\n",
           (int)(now - g.stall_since), pending, g.host->resends, (int)pia_reliable_rto(&g.host->reliable), rssi,
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

/* Records both sides' view of a stalled stream to the Switch, once per stall. */
static void check_stall(int64_t now)
{
    const pia_link_t *l = &g.s.link;
    int pending = pia_reliable_pending(&l->reliable);
    if (pending == 0 || l->reliable.low != g.stall_low)
    {
        if (g.stall_reported) trade_shim_pia_recovered(now);
        g.stall_low = l->reliable.low;
        g.stall_since = now;
        g.stall_reported = false;
        return;
    }
    if (g.stall_reported || now - g.stall_since < 1500) return;
    g.stall_reported = true;
    trade_shim_pia_t p = {
        .our_low = l->reliable.low, .our_next = l->reliable.next, .our_pending = (uint16_t)pending,
        .peer_low = l->peer_low, .peer_ack_next = l->peer_ack_next, .receive_next = l->reliable.receive_next,
        .stalled_ms = now - g.stall_since,
        .peer_low_age_ms = l->peer_low_valid ? now - l->peer_low_moved_ms : -1,
        .peer_ack_age_ms = l->peer_ack_valid ? now - l->peer_ack_moved_ms : -1,
        .peer_ack_seen_age_ms = l->peer_ack_valid ? now - l->peer_ack_seen_ms : -1,
        .out_of_order = (uint16_t)l->reliable.ooo_count, .credits = (uint16_t)l->credits,
        .k_queued = (uint16_t)l->k_count, .k_inflight = (uint16_t)l->k_inflight_count,
        .out_queued = (uint16_t)l->out_count, .overflow = l->overflow, .idle_evicted = l->idle_evicted,
    };
    trade_shim_pia_stall(now, &p);
}

void pia_bridge_room(const ldn_network_t *net)
{
    if (!g.started) return;
    if (g.state == BR_SCAN)
    {
        if (net->communication_id != FIRERED_ID || !adapter_link_live()) return;
        if (net->policy == 1 || net->member_count >= net->maximum) return;
        if (s_retry.magic == RETRY_MAGIC && memcmp(net->ssid, s_retry.ssid, 16) == 0 &&
            now_ms() < retry_hold_ms()) return;
        g.net = *net;
        g.have_room = true;
        return;
    }
    if (g.state == BR_MEMBER && memcmp(net->host, g.net.host, 6) == 0)
    {
        for (int i = 0; i < net->member_count; ++i)
            if (memcmp(net->members[i].mac, g.our_mac, 6) == 0)
            {
                snprintf(g.our_ip, sizeof(g.our_ip), "%s", net->members[i].ip);
                for (int j = 0; j < net->member_count; ++j)
                    if (net->members[j].index == 0)
                        snprintf(g.host_ip, sizeof(g.host_ip), "%s", net->members[j].ip);
                g.net = *net;
                g.state = BR_NET;
                return;
            }
    }
}

static void hex_of(const uint8_t *data, size_t length, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) { out[2 * i] = digits[data[i] >> 4]; out[2 * i + 1] = digits[data[i] & 15]; }
    out[2 * length] = 0;
}

void pia_bridge_poll(void)
{
    if (!g.started) return;
    int64_t now = now_ms();

    switch (g.state)
    {
    case BR_SCAN:
        drain_adapter();
        note_link();
        if (g.have_room)
        {
            char ssid[33], key_hex[33], bssid[18];
            uint8_t key[16];
            if (!ldn_keys_network(&g.net, key)) { printf("LDN_BRIDGE no keys\n"); pia_bridge_stop(); return; }
            hex_of(g.net.ssid, 16, ssid);
            hex_of(key, 16, key_hex);
            memset(key, 0, sizeof(key));
            snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                     g.net.host[0], g.net.host[1], g.net.host[2], g.net.host[3], g.net.host[4], g.net.host[5]);
            esp_err_t r = ldn_session_configure(ssid, bssid, key_hex, (unsigned)g.net.channel);
            memset(key_hex, 0, sizeof(key_hex));
            printf("LDN_BRIDGE joining channel=%d result=%d\n", g.net.channel, r);
            if (r != ESP_OK) { g.have_room = false; return; }
            g.state = BR_AUTH;
            g.deadline = now + 40000;
            g.next_auth = 0;
            g.auth_attempts = 0;
            g.authenticated = false;
            return;
        }
        if (now >= g.next_scan)
        {
            ldn_session_scan(kChannels[g.channel_index]);
            g.channel_index = (g.channel_index + 1) % CHANNEL_COUNT;
            g.next_scan = now + 500;
        }
        return;

    case BR_AUTH:
        if (now > g.deadline) { printf("LDN_BRIDGE join timed out\n"); bridge_restart(false); return; }
        if (!ldn_control_connected()) return;
        ldn_control_mac(g.our_mac);
        if (g.authenticated) { g.state = BR_MEMBER; return; }
        if (now >= g.next_auth && g.auth_attempts < 4)
        {
            if (g.auth_attempts == 0 && !ldn_auth_begin(&g.auth, &g.net))
            { printf("LDN_BRIDGE auth build failed\n"); pia_bridge_stop(); return; }
            ldn_control_tx_action(g.auth.request, (size_t)g.auth.request_len);
            ++g.auth_attempts;
            g.next_auth = now + 700;
            printf("LDN_BRIDGE auth request %d\n", g.auth_attempts);
        }
        return;

    case BR_MEMBER:
        if (now > g.deadline) { printf("LDN_BRIDGE membership timed out\n"); bridge_restart(false); return; }
        return;

    case BR_NET:
    {
        char line[128];
        snprintf(line, sizeof(line), "LDN_NET %s %s", g.our_ip, g.host_ip);
        ldn_udp_command(line, true);
        for (int i = 0; i < g.net.member_count; ++i)
        {
            if (memcmp(g.net.members[i].mac, g.our_mac, 6) == 0) continue;
            char mac[13];
            hex_of(g.net.members[i].mac, 6, mac);
            snprintf(line, sizeof(line), "LDN_NEIGH %s %s", g.net.members[i].ip, mac);
            ldn_udp_command(line, true);
        }
        const char *host_name = "SWITCH";
        for (int i = 0; i < g.net.member_count; ++i)
            if (g.net.members[i].index == 0 && g.net.members[i].name[0]) host_name = g.net.members[i].name;
        uint8_t record[APP_RECORD];
        const bool have_record = read_room_record(g.net.app_data, g.net.app_data_len, record);
        for (int k = 0; k < GROUP_COUNT; ++k)
            build_beacon(g.beacon[k], host_name, have_record ? record : NULL, kGroupActivity[k]);
        g.room_activity = 0;
        if (have_record)
            for (int k = 0; k < GROUP_COUNT; ++k)
                if ((bin_u16(record + 16) & 0x7f) == kGroupActivity[k]) g.room_activity = kGroupActivity[k];
        uint8_t host_mac[6];
        memcpy(host_mac, g.net.host, 6);
        pia_link_init(&g.s.link, g.net.ssid, g.our_mac, host_mac, g.our_ip, g.host_ip,
                      on_send, on_deliver, on_log, NULL);
        pia_link_set_connect(&g.s.link, false);
        g.state = BR_RUN;
        g.deadline = now + 10000;
        g.next_tick_us = esp_timer_get_time();
        g.tick_fraction = 0;
        g.s.link.stamp = trade_shim_stamp;
        g.s.link.sheddable = trade_shim_sheddable;
        g.stall_since = now;
        g.stall_reported = false;
        g.next_beacon = now;
        printf("LDN_BRIDGE joined room host=%s ours=%s host_ip=%s\n", host_name, g.our_ip, g.host_ip);
        return;
    }

    case BR_RUN:
        ldn_udp_heartbeat();
        drain_adapter();
        if (now >= g.next_beacon) { send_beacon(); g.next_beacon = now + 500; }
        int64_t now_us = esp_timer_get_time();
        if (now_us >= g.next_tick_us)
        {
            pia_link_tick(&g.s.link);
            /* 59.727 Hz, the GBA frame rate: at most one child frame per tick, so a
               slower tick falls behind. */
            g.tick_fraction += 1000000000LL;
            g.next_tick_us += g.tick_fraction / 59727;
            g.tick_fraction %= 59727;
            if (g.next_tick_us < now_us) g.next_tick_us = now_us + 16742;
            if (g.s.link.accepted && !g.child_connected && g.s.link.connect_wanted) connect_child();
        }
        trade_shim_poll(now);
        check_stall(now);
        {
            /* Net requests from the Switch; repeats past a second mean it ignored our
               ack. */
            const pia_conn_t *c = &g.s.link.conn;
            if (c->net_requests && (c->net_last_type != g.net_type || c->net_last_seq != g.net_seq))
            {
                g.net_type = c->net_last_type;
                g.net_seq = c->net_last_seq;
                g.net_unacked_noted = false;
                trade_shim_note(now, TRADE_SHIM_NOTE_NET_REQUEST, g.net_type, (uint16_t)g.net_seq);
                printf("LDN_BRIDGE Switch net request %02x seq %u\n", (unsigned)g.net_type, (unsigned)g.net_seq);
            }
            if (!g.net_unacked_noted && c->net_repeats >= 4)
            {
                g.net_unacked_noted = true;
                trade_shim_note(now, TRADE_SHIM_NOTE_NET_UNACKNOWLEDGED, g.net_type, (uint16_t)c->net_repeats);
                printf("LDN_BRIDGE Switch keeps repeating net request %02x: acknowledgement not taken\n", (unsigned)g.net_type);
            }
        }
        if (g.child_connected)
        {
            uint8_t extra[16], repeat[73];
            size_t n = trade_shim_inject(now, extra, sizeof(extra));
            if (n) pia_link_enqueue(&g.s.link, extra, n);
            n = trade_shim_host_inject(now, repeat, sizeof(repeat));
            if (n) send_host_frame(repeat, n);
        }
        if (g.s.link.rx_unzip_fail != g.unzip_seen)
        {
            /* First failure after a quiet spell marks a burst. */
            if (now - g.last_unzip_ms > 10000)
            {
                trade_shim_note(now, TRADE_SHIM_NOTE_UNZIP_FAILURE, (uint16_t)g.s.link.rx_unzip_last_error,
                                (uint16_t)g.s.link.rx_unzip_last_len);
                printf("LDN_BRIDGE Switch datagram could not be decompressed (error %d)\n", g.s.link.rx_unzip_last_error);
            }
            g.last_unzip_ms = now;
            g.unzip_seen = g.s.link.rx_unzip_fail;
        }
        const int body_max = g.s.link.rx_body_max;
        trade_shim_bridge_counters(now, (uint16_t)g.s.link.reordered, (uint16_t)g.s.link.hold_dropped,
                                   (uint16_t)g.s.link.overflow, (uint16_t)g.s.link.out_count,
                                   (uint16_t)g.s.link.rx_bad_frame, (uint16_t)g.s.link.decrypt_failures, (uint16_t)g.s.link.rx_unzip_fail,
                                   g.host_skips, g.host_long_skips,
                                   (uint16_t)(body_max > 65535 ? 65535 : body_max),
                                   (uint16_t)g.s.link.rx_unzip_last_error);
        if (g.last_host_frame_ms && !g.host_silent && now - g.last_host_frame_ms > 250)
        {
            g.host_silent = true;
            trade_shim_note(now, TRADE_SHIM_NOTE_HOST_SILENCE, 250, 0);
            printf("LDN_BRIDGE Switch stream has gone quiet\n");
        }
        if (g.s.link.host_disconnected)
        {
            printf("LDN_BRIDGE host disconnected\n");
            if (g.session_up) print_session_summary();
            trade_shim_note(now, TRADE_SHIM_NOTE_HOST_DISCONNECT, 0, 0);
            bridge_restart(true);
        }
        if (!g.session_up && pia_conn_connected(&g.s.link.conn))
        {
            g.session_up = true;
            g.session_ms = now;
            s_retry.failures = 0;
            printf("LDN_BRIDGE session established\n");
        }
        /* The Switch closing its room drops the network. With a GBA connected WD comes
           first. */
        if (!ldn_control_connected())
        {
            printf("LDN_BRIDGE network lost\n");
            if (g.session_up) print_session_summary();
            bridge_restart(false);
        }
        /* Handshake timeout: the Switch goes quiet instead of refusing. */
        if (!pia_conn_connected(&g.s.link.conn) && now > g.deadline)
        {
            if (s_retry.magic != RETRY_MAGIC || memcmp(s_retry.ssid, g.net.ssid, 16) != 0)
            {
                s_retry.magic = RETRY_MAGIC;
                memcpy(s_retry.ssid, g.net.ssid, 16);
                s_retry.failures = 0;
            }
            if (s_retry.failures < 255) ++s_retry.failures;
            printf("LDN_BRIDGE session handshake timed out, leaving this room alone for %d s\n",
                   (int)(retry_hold_ms() / 1000));
            bridge_restart(false);
        }
        return;

    case BR_HOST:
    {
        drain_adapter();
        ldn_host_poll();
        int64_t now_us = esp_timer_get_time();
        if (now_us >= g.next_tick_us)
        {
            pia_host_tick(g.host, now);
            g.tick_fraction += 1000000000LL;
            g.next_tick_us += g.tick_fraction / 59727;
            g.tick_fraction %= 59727;
            if (g.next_tick_us < now_us) g.next_tick_us = now_us + 16742;
            /* Shim frames are added only while the queue is nearly empty, so the replayed
               block is not crowded out. */
            if (g.client_connected && g.host->parent_count < 2)
            {
                static uint8_t repeat[73];
                to_child(repeat, trade_shim_lead_inject(now, repeat, sizeof(repeat)));
            }
        }
        if (g.host->state == 3) check_host_stall(now);
        if (pia_host_connect_pending(g.host) && !g.client_requested)
        {
            g.client_requested = true;
            send_command(RFU1_CONNECT_REQ, g.gba_devid);
            printf("LDN_BRIDGE Switch asks to join the group: connecting it to the GBA\n");
        }
        if (g.client_connected && g.host->child_disconnected)
        {
            send_command(RFU1_DISCONNECT, (uint32_t)g.client_devid | ((uint32_t)g.client_slot << 16));
            g.client_connected = g.client_requested = false;
            printf("LDN_BRIDGE Switch left the group\n");
        }
        if (g.client_connected && now >= g.next_client_ack)
        {
            send_command(RFU1_CLIENT_ACK, (uint32_t)g.client_devid | ((uint32_t)g.client_slot << 16));
            g.next_client_ack = now + CLIENT_ACK_MS;
        }
        if (g.closing_ms)
        {
            if (now - g.closing_ms > CLOSE_GRACE_MS + TEARDOWN_MS) bridge_restart(false);
            else if (now - g.closing_ms > CLOSE_GRACE_MS && !g.host->child_disconnected) pia_host_disconnect_child(g.host);
        }
        else if (!g.client_connected && now - g.gba_beacon_ms > BEACON_TIMEOUT_MS)
        {
            printf("LDN_BRIDGE GBA closed its group\n");
            bridge_restart(false);
        }
        return;
    }

    default:
        return;
    }
}

void pia_bridge_status(char *out, size_t cap)
{
    static const char *const names[] = {"idle", "scan", "auth", "member", "net", "run", "host"};
    if (!g.started) { snprintf(out, cap, "state=stopped"); return; }
    if (g.state == BR_HOST)
    {
        snprintf(out, cap, "state=host members=%d child=%d session=%d pia_rx=%d pia_tx=%d decrypt_failed=%d "
                 "to_gba=%d from_gba=%d parent_frames=%d child_frames=%d resends=%d gba_devid=%04x",
                 ldn_host_member_count(), g.client_connected, g.host->state, g.host->received, g.host->sent,
                 g.host->decrypt_failures, g.frames_to_gba, g.frames_from_gba, g.host->parent_frames,
                 g.host->child_frames, g.host->resends, g.gba_devid);
        return;
    }
    /* national: the Switch's save can link with Ruby, Sapphire and Emerald; Emerald joins a
       FireRed trade group only then. */
    snprintf(out, cap,
             "state=%s child=%d accepted=%d pia_rx=%d pia_tx=%d decrypt_failed=%d reordered=%d "
             "to_gba=%d from_gba=%d queued=%d repeated=%d overflow=%d hold_dropped=%d "
             "rx_seen=%d wrong_src=%d short=%d bad_frame=%d msgs=%d unzip_fail=%d conn_state=%d host_id=%04x shed=%d "
             "national=%d",
             names[g.state], g.child_connected, g.s.link.accepted, g.s.link.received, g.s.link.sent,
             g.s.link.decrypt_failures, g.s.link.reordered, g.frames_to_gba, g.frames_from_gba,
             g.s.link.out_count, g.s.link.repeated, g.s.link.overflow, g.s.link.hold_dropped,
             g.s.link.rx_seen, g.s.link.rx_wrong_source, g.s.link.rx_short, g.s.link.rx_bad_frame,
             g.s.link.rx_messages, g.s.link.rx_unzip_fail, g.s.link.conn.state, g.s.link.conn.host_id, g.s.link.shed,
             (bin_u16(g.beacon[0] + 2) >> 7) & 1);
    size_t at = strlen(out);
    if (at + 80 < cap)
    {
        out[at++] = ' ';
        trade_shim_status(out + at, cap - at);
        at += strlen(out + at);
    }
    if (g.s.link.rx_first_len > 0 && at + 64 < cap)
    {
        at += (size_t)snprintf(out + at, cap - at, " first(zip=%d pad=%d foot=%d)=",
                               g.s.link.rx_first_zipped, g.s.link.rx_first_pad, g.s.link.rx_first_footer);
        for (int i = 0; i < g.s.link.rx_first_len && at + 3 < cap; ++i)
            at += (size_t)snprintf(out + at, cap - at, "%02x", g.s.link.rx_first[i]);
    }
}
