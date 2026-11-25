#include "pia_host.h"

#include <stdio.h>
#include <string.h>
#include "esp_random.h"

#define PIA_PORT 0x3039
#define STATUS_REPEAT_MS 500
#define PROPERTY_REPEAT_MS 500
#define JOIN_REPLY_REPEAT_MS 500
#define JOIN_REPLIES 5
/* A Switch host resends a station list every 2 s until the station acknowledges it. */
#define UPDATE_REPEAT_MS 2000
#define FIRST_PROBE_MS 117
#define PROBE_PERIOD_MS 302
#define IDLE_TICKS 16
#define NOTICE_COPIES 3           /* kind 9, 1 s apart */
#define NOTICE_PERIOD_MS 1000
#define BROADCAST_COPIES 5        /* kind 13 and each 0x11 form, 0.5 s apart */
#define BROADCAST_PERIOD_MS 500
#define MAX_FRAMES PIA_HOST_FRAMES
/* Frames in flight to a child, below its 20-slot receive queue. */
#define INFLIGHT_MAX 12
/* The child's game runs a link frame only when a parent frame is waiting and sends one
   datagram per frame run. Each datagram from it allows one data frame to it, so no more
   than CREDIT_CAP wait at the child; the cap must cover the round trip or the child
   stalls between frames. While the child is quiet only INFLIGHT_MAX applies. */
#define CREDIT_CAP 6
#define CHILD_QUIET_MS 250
/* A gap in a station's stream is given up after this long: its frame is not coming. */
#define GAP_GIVE_UP_MS 300


/* Per-console value in the host's station record; the same in every captured session. */
static const uint8_t kHostBlob[11] = {0x38, 0xf9, 0x50, 0x7e, 0x55, 0xd0, 0x40, 0x19, 0x13, 0xc5, 0x89};
static const char kDefaultName[] = "GBA";

enum { STATUS_NORMAL, STATUS_MARKED, STATUS_CLEARED };

static void emit(pia_host_t *h, const char *message) { if (h->log) h->log(message, h->user); }

static int slot_of(const pia_host_t *h, const pia_host_station_t *s) { return (int)(s - h->st); }

static bool ip_bytes(const char *text, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

/* Host name in the application data at 26: length, 1, text. */
static void host_name(const pia_host_t *h, const uint8_t **name, int *length)
{
    int n = h->app_data_len >= 28 ? h->app_data[26] : 0;
    if (n >= 1 && n <= 32 && h->app_data[27] == 1 && 28 + n <= h->app_data_len)
    {
        *name = h->app_data + 28;
        *length = n;
        return;
    }
    *name = (const uint8_t *)kDefaultName;
    *length = (int)sizeof(kDefaultName) - 1;
}

static bool is_uni(const uint8_t *p, size_t len)
{
    if (len < 3) return false;
    uint32_t f = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    return ((f >> 14) & 0xf) == 4;
}

static bool has_command(const uint8_t *p, size_t len)
{
    for (size_t o = 3; o + 2 <= len; o += 14) if (p[o] || p[o + 1]) return true;
    return false;
}

/* A frame the child ignores: a command-less UNI frame or the all-zero idle carrier. */
static bool state_only(const uint8_t *p, size_t len)
{
    if (is_uni(p, len)) return !has_command(p, len);
    for (size_t i = 0; i < len; ++i) if (p[i]) return false;
    return true;
}

/* ---- stations -------------------------------------------------------------- */

/* Slots of the stations in the room from `min_state` on, in the order they joined. */
static int stations_in_order(const pia_host_t *h, int min_state, int order[PIA_HOST_STATIONS])
{
    int n = 0;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
    {
        if (h->st[i].state < min_state || h->st[i].state == 0) continue;
        int j = n++;
        while (j > 0 && h->st[order[j - 1]].join_number > h->st[i].join_number) { order[j] = order[j - 1]; --j; }
        order[j] = i;
    }
    return n;
}

static pia_host_station_t *station_by_ip(pia_host_t *h, const char *ip)
{
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
        if (h->st[i].state != 0 && strcmp(h->st[i].ip, ip) == 0) return &h->st[i];
    return NULL;
}

/* A station whose child has the GBA's WA and has not been dropped. */
static bool in_group(const pia_host_station_t *s) { return s->state != 0 && s->child_accepted && !s->child_disconnected; }

static bool group_running(const pia_host_t *h)
{
    for (int i = 0; i < PIA_HOST_STATIONS; ++i) if (in_group(&h->st[i])) return true;
    return false;
}

/* Every station sends its next status soon: the membership changed. */
static void status_changed(pia_host_t *h)
{
    ++h->net_seq;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
    {
        h->st[i].status_acked = false;
        h->st[i].next_net_ms = h->now_ms;
    }
}

/* ---- datagram assembly ----------------------------------------------------- */

/* Packet ids: one counter for the 0001 channel (protocol 3, kind 5), one for the child's
   id (protocol 10, kinds 2, 9, 13). */
static uint16_t next_packet_id(pia_host_station_t *s, uint16_t dst)
{
    uint16_t *counter = &s->packet_ids[dst == 1 ? 0 : 1];
    if (*counter == 0) *counter = 1;
    uint16_t id = *counter;
    *counter = (uint16_t)(id == 65535 ? 1 : id + 1);
    return id;
}

static void send_messages(pia_host_t *h, pia_host_station_t *s, const pia_message_t *messages, int count,
                          uint16_t dst, uint16_t src, int packet, bool zip, bool establishing, bool footer)
{
    pia_host_scratch_t *sc = &h->sc;
    uint8_t *body = sc->body, *staged = sc->staged, *datagram = sc->datagram;
    size_t length = 0;
    for (int i = 0; i < count; ++i)
    {
        int n = pia_message_encode(&messages[i], body + length, sizeof(sc->body) - length);
        if (n < 0) return;
        length += (size_t)n;
    }
    /* Zipped as the Switch sends them: session messages always, other bodies from 62
       bytes. */
    bool zipped = zip || length >= 62;
    if (zipped)
    {
        int n = pia_compress_raw(body, length, staged, sizeof(sc->staged));
        if (n < 0) return;
        memcpy(body, staged, (size_t)n);
        length = (size_t)n;
    }
    if (footer)
    {
        if (length + 2 > sizeof(sc->body)) return;
        bin_wb16(body + length, s->id);
        length += 2;
    }
    size_t padding = (16 - (length & 15)) & 15;
    if (length + padding > sizeof(sc->body)) return;
    memset(body + length, 0xff, padding);
    length += padding;

    uint16_t pid = packet >= 0 ? (uint16_t)packet : next_packet_id(s, dst);
    uint8_t flags = (uint8_t)((padding << 4) | (zipped ? 1 : 0) | (establishing ? 2 : 0));
    int n = pia_encrypt(&h->crypto, body, length, h->ours, dst, src, pid, h->nonce++, flags,
                        footer ? 2 : 0, datagram, sizeof(sc->datagram));
    if (h->nonce == 0) h->nonce = 1;
    if (n < 0) return;
    h->send(datagram, (size_t)n, s->ip, h->user);
    ++s->sent;
}

/* Session message: dst 0000, establishing, no footer. */
static void send_session(pia_host_t *h, pia_host_station_t *s, uint8_t protocol, const uint8_t *payload, size_t len,
                         uint16_t src, int packet, bool zip)
{
    pia_message_t m = {.protocol = protocol, .payload = payload, .length = (uint16_t)len, .has_flags = false};
    send_messages(h, s, &m, 1, 0, src, packet, zip, true, false);
}

/* Connected message to the child or the 0001 channel, footer = the child's id. */
static void send_connected(pia_host_t *h, pia_host_station_t *s, uint8_t protocol, const uint8_t *payload, size_t len,
                           uint16_t dst, bool zip)
{
    pia_message_t m = {.protocol = protocol, .payload = payload, .length = (uint16_t)len, .has_flags = false};
    send_messages(h, s, &m, 1, dst, h->our_id, -1, zip, false, true);
}

/* ---- protocol 1: network status and property ------------------------------- */

/* The host, then the stations in the order they joined, each at its position. A station
   on its way out is marked in its own copy, then left out; the others no longer see it. */
static void send_status(pia_host_t *h, pia_host_station_t *s, int form)
{
    uint8_t p[162] = {0};
    p[0] = 1; p[1] = 0x11;
    bin_wb16(p + 2, 132);
    bin_wb32(p + 4, h->net_seq);
    bin_wb16(p + 8, h->our_id);
    memcpy(p + 10, h->our_mac, 6);
    bin_wb32(p + 22, h->crypto.net_id);
    p[26] = 1; p[28] = 6;
    p[29] = form == STATUS_NORMAL ? 0 : 1;
    for (int i = 0; i < 6; ++i) p[30 + 22 * i + 1] = 0xff;
    int order[PIA_HOST_STATIONS];
    const int n = stations_in_order(h, 1, order);
    int at = 0;
    for (int k = -1; k < n && at < 6; ++k)
    {
        const char *ip = h->ours;
        bool marked = false;
        if (k >= 0)
        {
            const pia_host_station_t *o = &h->st[order[k]];
            if (o == s && form == STATUS_CLEARED) continue;
            if (o != s && o->teardown >= 3) continue;
            marked = o == s && form == STATUS_MARKED;
            ip = o->ip;
        }
        uint8_t *e = p + 30 + 22 * at;
        if (!ip_bytes(ip, e + 4)) continue;
        e[1] = (uint8_t)at;
        if (marked) e[3] = 1;
        bin_wb16(e + 20, PIA_PORT);
        ++at;
    }
    /* Unconnected form once the child's connection is gone. */
    send_session(h, s, 1, p, sizeof(p), form == STATUS_NORMAL ? h->our_id : 0, 0, true);
}

static void send_property(pia_host_t *h, pia_host_station_t *s)
{
    uint8_t *p = h->sc.property;
    int len = h->app_data_len, a = len < 92 ? len : 92;
    int stations = 1;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i) stations += h->st[i].state != 0 && h->st[i].teardown < 3;
    memset(p, 0, 38);
    p[0] = 1; p[1] = 0x50;
    bin_wb16(p + 2, (uint16_t)len);
    bin_wb32(p + 4, h->property_seq);
    bin_wb32(p + 12, h->crypto.net_id);
    p[17] = (uint8_t)stations; p[19] = 6;       /* stations, of a maximum of six */
    p[26] = 0x57; p[27] = 0x0f; p[28] = 1; p[29] = 1;
    bin_wb32(p + 30, (uint32_t)a);             /* game header length, string length */
    bin_wb32(p + 34, (uint32_t)(len - a));
    memcpy(p + 38, h->app_data, (size_t)len);
    send_session(h, s, 1, p, 38 + (size_t)len, h->our_id, 0, true);
}

/* ---- protocol 13: session -------------------------------------------------- */

/* Station list: header, the host's record, the stations in the order they joined (each
   with its position and join number), then the stations that left. */
static void send_station_list(pia_host_t *h, pia_host_station_t *s)
{
    uint8_t *p = h->sc.station_list;
    const uint8_t *name;
    int name_len;
    host_name(h, &name, &name_len);
    uint8_t our_ip[4] = {0};
    ip_bytes(h->ours, our_ip);
    int order[PIA_HOST_STATIONS];
    const int n = stations_in_order(h, 2, order);
    memset(p, 0, sizeof(h->sc.station_list));
    size_t o = 0;
    p[o++] = 5;
    bin_wb16(p + o, (uint16_t)(h->list_seq - 1)); o += 2;
    p[o++] = 1; p[o++] = 0;                    /* one fragment */
    p[o++] = 0; p[o++] = 3;
    memcpy(p + o, h->our_mac, 6); o += 8;
    bin_wb16(p + o, h->our_id); o += 2;
    p[o++] = (uint8_t)(1 + n);
    p[o++] = (uint8_t)h->left_count;
    bin_wb16(p + o, h->list_seq); o += 2;
    p[o++] = 0; p[o++] = h->member_left ? 1 : 0;
    o += 4;
    /* Host record: address, per-console values, player name. */
    memcpy(p + o, h->our_mac, 6); o += 8;
    bin_wb16(p + o, h->our_id); o += 2;
    memcpy(p + o, our_ip, 4); o += 4;
    bin_wb16(p + o, PIA_PORT); o += 2;
    o += 37;
    p[o++] = 1; p[o++] = 1; p[o++] = 0; p[o++] = 0; p[o++] = 0x10;
    o += 4;
    memcpy(p + o, kHostBlob, sizeof(kHostBlob)); o += sizeof(kHostBlob);
    o += 3;
    p[o++] = (uint8_t)name_len; p[o++] = 1;
    memcpy(p + o, name, (size_t)name_len); o += (size_t)name_len;
    /* Station records: address, position and join number, then the tail of its join. */
    for (int k = 0; k < n; ++k)
    {
        const pia_host_station_t *e = &h->st[order[k]];
        uint8_t ip[4] = {0};
        ip_bytes(e->ip, ip);
        memcpy(p + o, e->mac, 6); o += 8;
        bin_wb16(p + o, e->id); o += 2;
        memcpy(p + o, ip, 4); o += 4;
        bin_wb16(p + o, PIA_PORT); o += 2;
        p[o++] = (uint8_t)(k + 1); p[o++] = 0; p[o++] = e->join_number; p[o++] = 0;
        o += 33;
        p[o++] = 1; p[o++] = 1; p[o++] = 0; p[o++] = 0;
        memcpy(p + o, e->joiner_record, (size_t)e->joiner_record_len); o += (size_t)e->joiner_record_len;
    }
    /* Left records: the list versions that added and removed it, its join number. */
    for (int k = 0; k < h->left_count; ++k)
    {
        const pia_host_left_t *l = &h->left[k];
        memcpy(p + o, l->mac, 6); o += 8;
        bin_wb16(p + o, l->id); o += 2;
        bin_wb16(p + o, l->joined_seq); o += 2;
        bin_wb16(p + o, l->left_seq); o += 2;
        p[o++] = l->join_number; p[o++] = 1; p[o++] = 1;
        o += 32;
        memcpy(p + o, l->record, l->record_len); o += l->record_len;
    }
    send_connected(h, s, 13, p, o, 1, true);
}

static int list_position(const pia_host_t *h, const pia_host_station_t *s)
{
    int order[PIA_HOST_STATIONS];
    const int n = stations_in_order(h, 2, order);
    for (int k = 0; k < n; ++k) if (&h->st[order[k]] == s) return k + 1;
    return n + 1;
}

static void send_join_response(pia_host_t *h, pia_host_station_t *s)
{
    uint8_t p[37] = {2, 0x0d, 7, 1};
    memcpy(p + 8, s->join_token, 4);
    memcpy(p + 12, h->our_mac, 6);
    bin_wb16(p + 20, h->our_id);
    memcpy(p + 22, s->mac, 6);
    bin_wb16(p + 30, s->id);
    p[32] = (uint8_t)list_position(h, s);
    p[34] = s->join_number;
    p[36] = (uint8_t)(s->join_number - 1);
    send_connected(h, s, 13, p, sizeof(p), s->id, false);
}

static void send_disconnect_notice(pia_host_t *h, pia_host_station_t *s)
{
    uint8_t p[30] = {9};
    memcpy(p + 1, h->our_mac, 6);
    bin_wb16(p + 9, h->our_id);
    ip_bytes(h->ours, p + 12);
    bin_wb16(p + 16, PIA_PORT);
    memcpy(p + 18, s->mac, 6);
    bin_wb16(p + 26, s->id);
    send_connected(h, s, 13, p, sizeof(p), s->id, false);
}

static void send_removed_broadcast(pia_host_t *h, pia_host_station_t *s, bool last)
{
    uint8_t p[10] = {0x0d};
    memcpy(p + 1, h->our_mac, 6);
    p[9] = 2;
    send_session(h, s, 13, p, sizeof(p), h->our_id, last ? 0 : -1, false);
}

/* Left records stay until every station in the list has acknowledged one naming them. */
static void prune_left(pia_host_t *h)
{
    for (int k = 0; k < h->left_count;)
    {
        bool seen = true;
        for (int i = 0; i < PIA_HOST_STATIONS && seen; ++i)
        {
            const pia_host_station_t *s = &h->st[i];
            if (s->state >= 2 && (s->update_acked == 0 || bin_less(s->update_acked, h->left[k].left_seq))) seen = false;
        }
        if (!seen) { ++k; continue; }
        memmove(&h->left[k], &h->left[k + 1], sizeof(h->left[0]) * (size_t)(h->left_count - k - 1));
        --h->left_count;
    }
}

static void on_update_ack(pia_host_t *h, pia_host_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 15 || bin_b16(p + 13) != h->list_seq) return;
    s->update_acked = h->list_seq;
    prune_left(h);
}

/* Keep-alive timeout report: the stations this one has not heard from. A Switch host drops
   them; here the GBA decides who is in its group, so the report is only noted. */
static void on_silent_report(pia_host_t *h, pia_host_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 14) return;
    const int count = p[13];
    for (int k = 0; k < count && 14 + 11 * (size_t)k + 11 <= len; ++k)
    {
        const uint16_t id = bin_b16(p + 14 + 11 * k + 8);
        for (int i = 0; i < PIA_HOST_STATIONS; ++i)
        {
            const pia_host_station_t *o = &h->st[i];
            if (o->state < 2 || o->id != id || (s->silent_noted & (1u << i))) continue;
            s->silent_noted |= (uint8_t)(1u << i);
            char line[64];
            snprintf(line, sizeof(line), "member %u does not hear member %u", s->index, o->index);
            emit(h, line);
        }
    }
}

/* ---- protocol 3: round trips ----------------------------------------------- */

static void send_probe(pia_host_t *h, pia_host_station_t *s)
{
    uint8_t p[21] = {0};
    uint64_t stamp = (uint64_t)h->now_ms * 19200;          /* the 19.2 MHz system counter */
    bin_wb64(p + 1, stamp);
    bin_wb32(p + 13, (uint32_t)((uint64_t)h->now_ms * 1000));
    bin_wb16(p + 19, h->our_id);
    int slot = 0;
    for (int i = 0; i < PIA_HOST_PROBES; ++i)
    {
        if (!s->probes[i].used) { slot = i; break; }
        if (s->probes[i].sent_ms < s->probes[slot].sent_ms) slot = i;
    }
    s->probes[slot].stamp = stamp;
    s->probes[slot].sent_ms = h->now_ms;
    s->probes[slot].used = true;
    send_connected(h, s, 3, p, sizeof(p), 1, false);
}

/* Echo the child's probe: bytes 1..8 kept, the rest rewritten. */
static void reply_probe(pia_host_t *h, pia_host_station_t *s, const uint8_t *q)
{
    uint8_t p[21] = {1};
    memcpy(p + 1, q + 1, 8);
    bin_wb32(p + 13, (uint32_t)((uint64_t)h->now_ms * 1000));
    bin_wb16(p + 17, s->id);
    bin_wb16(p + 19, h->our_id);
    send_connected(h, s, 3, p, sizeof(p), 1, false);
}

static void match_probe(pia_host_t *h, pia_host_station_t *s, const uint8_t *p)
{
    uint64_t stamp = bin_b64(p + 1);
    for (int i = 0; i < PIA_HOST_PROBES; ++i)
        if (s->probes[i].used && s->probes[i].stamp == stamp)
        {
            s->probes[i].used = false;
            pia_reliable_add_rtt(&s->reliable, (double)(h->now_ms - s->probes[i].sent_ms));
            return;
        }
}

/* ---- protocol 10: the reliable stream -------------------------------------- */

static bool queue_reliable(pia_host_t *h, pia_host_station_t *s, const uint8_t *data, size_t len, pia_packet_t *out)
{
    uint8_t flags = s->stream_opened ? 7 : 15;     /* the first frame of a stream is marked */
    int seq = pia_reliable_queue(&s->reliable, data, len, flags, (double)h->now_ms);
    if (seq < 0) return false;
    s->stream_opened = true;
    const pia_pending_t *e = pia_reliable_entry(&s->reliable, (uint16_t)seq);
    out->seq = (uint16_t)seq;
    out->flags = flags;
    out->data = e->data;
    out->length = e->length;
    return true;
}

/* One datagram of reliable frames, plus any owed ack. */
static void send_frames(pia_host_t *h, pia_host_station_t *s, pia_packet_t *frames, int count)
{
    pia_host_scratch_t *sc = &h->sc;
    uint8_t (*wrapped)[PIA_HOST_FRAME + 8] = sc->wrapped;
    uint8_t *ack = sc->ack;
    if (count < MAX_FRAMES && (s->ack_owed || pia_reliable_has_gap(&s->reliable)))
    {
        pia_reliable_ack(&s->reliable, ack);
        frames[count].seq = 0xfff0; frames[count].flags = 0; frames[count].data = ack; frames[count].length = 20;
        ++count;
        s->ack_owed = false;
    }
    if (count == 0) return;
    pia_message_t messages[MAX_FRAMES];
    for (int i = 0; i < count; ++i)
    {
        int size = pia_reliable_wrap(&s->reliable, &frames[i], wrapped[i], sizeof(wrapped[i]));
        if (size < 0) return;
        messages[i].protocol = 10;
        messages[i].payload = wrapped[i];
        messages[i].length = (uint16_t)size;
        messages[i].has_flags = frames[i].flags == 0;
        messages[i].flags = 0x40;
    }
    send_messages(h, s, messages, count, s->id, h->our_id, -1, false, false, true);
}

/* One W frame to every station whose stream is up, as a Switch host sends WA and WD: a
   station knows its own by the connect id. */
static void send_to_group(pia_host_t *h, const uint8_t *frame, size_t len, pia_host_station_t *also)
{
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
    {
        pia_host_station_t *s = &h->st[i];
        if (s != also && (s->state != 3 || s->child_disconnected)) continue;
        pia_packet_t frames[MAX_FRAMES];
        if (queue_reliable(h, s, frame, len, &frames[0])) send_frames(h, s, frames, 1);
    }
}

/* WT: 'W' 'T', total - 4, stamp, send length at 8, data padded to 4. The 1-byte idle
   frame carries no data. */
static int wrap_wt(const uint8_t *data, size_t n, uint32_t stamp, uint8_t *out, size_t cap)
{
    size_t padded = n == 1 && data[0] == 0 ? 0 : (n + 3) & ~(size_t)3;
    size_t total = 12 + padded;
    if (n == 0 || n > 0x7f || total > cap) return -1;
    memset(out, 0, total);
    out[0] = 0x57; out[1] = 0x54;
    bin_w16(out + 2, (uint16_t)(total - 4));
    bin_w32(out + 4, stamp);
    out[8] = (uint8_t)n;
    memcpy(out + 12, data, padded ? n : 0);
    return (int)total;
}

static void note_wt(pia_host_t *h, pia_host_station_t *s, uint32_t stamp, uint16_t seq, bool idle)
{
    s->wt_ring[s->wt_ring_head].stamp = stamp;
    s->wt_ring[s->wt_ring_head].seq = seq;
    s->wt_ring[s->wt_ring_head].sent_ms = h->now_ms;
    s->wt_ring[s->wt_ring_head].used = true;
    s->wt_ring[s->wt_ring_head].idle = idle;
    s->wt_ring_head = (s->wt_ring_head + 1) % PIA_HOST_WT_RING;
}

/* A WK gives an RTT sample for a WT sent once. */
static void take_wk(pia_host_t *h, pia_host_station_t *s, uint32_t stamp)
{
    ++s->wk_acks;
    for (int i = 0; i < PIA_HOST_WT_RING; ++i)
    {
        if (!s->wt_ring[i].used || s->wt_ring[i].stamp != stamp) continue;
        s->wt_ring[i].used = false;
        const pia_pending_t *e = pia_reliable_entry(&s->reliable, s->wt_ring[i].seq);
        if (e && e->resends == 0) pia_reliable_add_rtt(&s->reliable, (double)(h->now_ms - s->wt_ring[i].sent_ms));
        return;
    }
}

static void note_child_stamp(pia_host_station_t *s, uint32_t stamp)
{
    if (s->child_stamp_valid)
    {
        uint32_t elapsed = stamp - s->child_stamp;
        if (elapsed > 1 && elapsed < 300) s->child_skipped += (int)elapsed - 1;
    }
    s->child_stamp = stamp;
    s->child_stamp_valid = true;
}

static void deliver_child_wt(pia_host_t *h, pia_host_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 12) return;
    note_child_stamp(s, bin_u32(p + 4));
    /* The send header (bytes 8-11) holds a child's length at bit 8 + 5 × its slot. Our
       joiner as a later client also puts it in byte 8, where a host's frames carry theirs. */
    size_t n = (bin_u32(p + 8) >> (8 + 5 * s->client_slot)) & 0x1f;
    if (n == 0) n = p[8] & 0x7f;
    if (n > len - 12) n = len - 12;
    ++s->child_frames;
    if (n > 0 && h->deliver) h->deliver(p + 12, n, slot_of(h, s), h->user);
}

/* One child stream frame, in order. */
static void feed(pia_host_t *h, pia_host_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 4 || p[0] != 0x57 || (size_t)bin_u16(p + 2) + 4 != len) return;   /* metadata: nothing to do */
    if (p[1] == 0x43 && len >= 6)
    {
        if (s->child_disconnected) return;
        memcpy(s->connect_id, p + 4, 2);
        if (!s->connect_requested)
        {
            char line[32];
            snprintf(line, sizeof(line), "WC from member %u", s->index);
            emit(h, line);
        }
        s->connect_requested = true;
    }
    else if (p[1] == 0x54) deliver_child_wt(h, s, p, len);
    else if (p[1] == 0x4b && len >= 16) take_wk(h, s, bin_u32(p + 12));
}

static int held_count(const pia_host_station_t *s)
{
    int held = 0;
    for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i) held += s->hold[i].used;
    return held;
}

/* Held frames that follow on from `next`, in order. Returns the new next. */
static uint16_t release_held(pia_host_t *h, pia_host_station_t *s, uint16_t next)
{
    for (bool again = true; again;)
    {
        again = false;
        for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
            if (s->hold[i].used && s->hold[i].seq == next)
            {
                s->hold[i].used = false;
                feed(h, s, s->hold[i].data, s->hold[i].length);
                next = (uint16_t)(next + 1);
                again = true;
                break;
            }
    }
    return next;
}

/* The frames missing before the first held one are given up: acknowledged as if they had
   arrived, so the station's window moves on, and the held frames after them delivered. */
static void give_up_gap(pia_host_t *h, pia_host_station_t *s)
{
    int lowest = -1;
    for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
        if (s->hold[i].used && (lowest < 0 || bin_less(s->hold[i].seq, (uint16_t)lowest))) lowest = s->hold[i].seq;
    if (lowest < 0 || s->next_deliver < 0) return;
    const uint16_t from = (uint16_t)s->next_deliver, to = (uint16_t)lowest;
    for (uint16_t q = from; q != to; ++q) pia_reliable_receive(&s->reliable, q, q);
    char line[96];
    if ((uint16_t)(to - from) == 1)
        snprintf(line, sizeof(line), "Switch %u did not resend frame %04x: going on without it", s->index, from);
    else
        snprintf(line, sizeof(line), "Switch %u did not resend frames %04x-%04x: going on without them", s->index, from,
                 (uint16_t)(to - 1));
    emit(h, line);
    ++s->gaps_given_up;
    s->next_deliver = release_held(h, s, to);
    s->gap_since_ms = h->now_ms;
}

/* Pia retransmits out of order, but the games need their +1 mod 8 sequence, so child
   frames are delivered in order. A Switch station does not resend a frame lost on the way
   here (its window stays at the frame while newer ones keep coming), so a gap is waited for
   only briefly, and not at all once the hold is full. */
static void resequence(pia_host_t *h, pia_host_station_t *s, uint16_t seq, uint16_t low, const uint8_t *inner, size_t len)
{
    if (s->next_deliver < 0) s->next_deliver = low;
    uint16_t next = (uint16_t)s->next_deliver;
    if (seq == next)
    {
        pia_reliable_receive(&s->reliable, seq, low);
        feed(h, s, inner, len);
        s->next_deliver = release_held(h, s, (uint16_t)(next + 1));
        s->gap_since_ms = h->now_ms;
        return;
    }
    if (!bin_less(next, seq) || (uint16_t)(seq - next) >= 4096)
    {
        pia_reliable_receive(&s->reliable, seq, low);    /* a copy of a delivered frame */
        return;
    }
    if (len > PIA_HOST_HOLD_BYTES) { ++s->hold_dropped; return; }
    for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
        if (s->hold[i].used && s->hold[i].seq == seq) return;
    const int held = held_count(s);
    if (held == PIA_HOST_HOLD_SLOTS)
    {
        give_up_gap(h, s);
        resequence(h, s, seq, low, inner, len);
        return;
    }
    if (held == 0) s->gap_since_ms = h->now_ms;
    for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
        if (!s->hold[i].used)
        {
            s->hold[i].used = true;
            s->hold[i].seq = seq;
            s->hold[i].length = (uint8_t)len;
            memcpy(s->hold[i].data, inner, len);
            pia_reliable_receive(&s->reliable, seq, low);
            return;
        }
}

static void check_gap(pia_host_t *h, pia_host_station_t *s)
{
    if (s->next_deliver >= 0 && h->now_ms - s->gap_since_ms >= GAP_GIVE_UP_MS && held_count(s) > 0) give_up_gap(h, s);
}

static void on_reliable(pia_host_t *h, pia_host_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 8 || bin_b16(p + 1) > len - 8 || p[7] != 0) { ++s->rx_bad_frame; return; }
    uint16_t inner_len = bin_b16(p + 1), seq = bin_b16(p + 3), low = bin_b16(p + 5);
    const uint8_t *inner = p + 8;
    if ((p[0] & 1) == 0)
    {
        pia_reliable_acknowledge(&s->reliable, inner, inner_len, (double)h->now_ms);
        return;
    }
    s->ack_owed = true;
    resequence(h, s, seq, low, inner, inner_len);
}

/* ---- inbound --------------------------------------------------------------- */

static void on_join(pia_host_t *h, pia_host_station_t *s, const pia_message_t *m, uint16_t src)
{
    const uint8_t *p = m->payload;
    if (m->length < 84) { ++s->rx_bad_frame; return; }
    uint16_t id = bin_b16(p + 28);
    if (id == 0) id = src;
    if (id == 0 || id == h->our_id) return;
    if (s->state >= 2)
    {
        /* Join repeated: our reply was lost. */
        if (id != s->id) return;
        send_station_list(h, s);
        send_join_response(h, s);
        return;
    }
    s->id = id;
    static const uint8_t zero_mac[6];
    if (memcmp(p + 20, zero_mac, 6) != 0) memcpy(s->mac, p + 20, 6);
    int tail = m->length - 83;
    if (tail > PIA_HOST_RECORD_BYTES) tail = PIA_HOST_RECORD_BYTES;
    memcpy(s->joiner_record, p + 83, (size_t)tail);
    s->joiner_record_len = tail;
    esp_fill_random(s->join_token, sizeof(s->join_token));
    s->state = 2;
    s->joined_ms = h->now_ms;
    s->join_replies = 1;
    s->next_join_reply_ms = h->now_ms + JOIN_REPLY_REPEAT_MS;
    s->next_probe_ms = h->now_ms + FIRST_PROBE_MS;
    /* A new list version; the stations already in hear of it too. */
    s->joined_seq = ++h->list_seq;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
        if (&h->st[i] != s && h->st[i].state >= 2) h->st[i].next_update_ms = h->now_ms;
    char line[40];
    snprintf(line, sizeof(line), "join from station id=%04x", s->id);
    emit(h, line);
    send_station_list(h, s);
    send_join_response(h, s);
    s->next_update_ms = h->now_ms + UPDATE_REPEAT_MS;
}

static void dispatch(pia_host_t *h, pia_host_station_t *s, const pia_message_t *m, uint16_t src)
{
    const uint8_t *p = m->payload;
    if (m->protocol == 1)
    {
        if (m->length < 8 || p[0] != 1) return;
        uint32_t seq = bin_b32(p + 4);
        if (p[1] == 0x12 && seq == h->net_seq) s->status_acked = true;
        else if (p[1] == 0x51 && seq == h->property_seq) s->property_acked = true;
    }
    else if (m->protocol == 13)
    {
        if (m->length < 1) return;
        if (p[0] == 0) on_join(h, s, m, src);
        else if (s->state >= 2)
        {
            s->child_alive = true;
            if (p[0] == 6) on_update_ack(h, s, p, m->length);
            else if (p[0] == 0x0b) on_silent_report(h, s, p, m->length);
        }
    }
    else if (m->protocol == 3)
    {
        if (s->state < 2 || m->length < 21) return;
        s->child_alive = true;
        if (p[0] == 0) reply_probe(h, s, p);
        else if (p[0] == 1) match_probe(h, s, p);
    }
    else if (m->protocol == 10)
    {
        if (s->state < 2) return;
        s->child_alive = true;
        if (s->state == 2) { s->state = 3; emit(h, "session up"); }
        on_reliable(h, s, p, m->length);
    }
}

void pia_host_receive(pia_host_t *h, const uint8_t *data, size_t length, const char *source)
{
    pia_host_station_t *s = station_by_ip(h, source);
    if (!s) { ++h->rx_wrong_source; return; }
    if (length < 29) { ++s->rx_bad_frame; return; }
    pia_host_scratch_t *sc = &h->sc;
    uint8_t *plain = sc->plain, *app = sc->app;
    int n = pia_decrypt(&h->crypto, data, length, source, plain, sizeof(sc->plain));
    if (n < 0) { ++s->decrypt_failures; return; }
    int padding = data[5] >> 4, footer = data[12];
    if (padding + footer > n) { ++s->rx_bad_frame; return; }
    n -= padding + footer;

    const uint8_t *body = plain;
    int size = n;
    if (data[5] & 1)
    {
        size = pia_decompress(plain, (size_t)n, app, sizeof(sc->app));
        if (size < 0) { ++s->rx_unzip_fail; return; }
        body = app;
    }
    uint16_t src = bin_b16(data + 8);
    pia_message_iter_t it;
    pia_message_t message;
    pia_message_iter_init(&it, body, (size_t)size);
    while (pia_message_next(&it, &message))
    {
        ++s->rx_messages;
        dispatch(h, s, &message, src);
        if (s->state == 0) return;
    }
    ++s->received;
    if (s->credits < h->credit_cap) ++s->credits;
    s->last_child_datagram_ms = h->now_ms;
}

/* ---- the parent's frames --------------------------------------------------- */

static void parent_enqueue(pia_host_t *h, pia_host_station_t *s, const uint8_t *payload, size_t length)
{
    bool plain = state_only(payload, length);
    if (plain)
    {
        if (s->parent_count > 0)
        {
            int at = (s->parent_head + s->parent_count - 1) % PIA_HOST_PARENT_SLOTS;
            if (state_only(s->parent[at].data, s->parent[at].length))
            {
                memcpy(s->parent[at].data, payload, length);
                s->parent[at].length = (uint8_t)length;
                return;
            }
        }
    }
    if (s->parent_count == PIA_HOST_PARENT_SLOTS)
    {
        ++s->parent_dropped;
        if (plain) return;
        int victim = 0;
        for (int i = 0; i < s->parent_count; ++i)
        {
            int at = (s->parent_head + i) % PIA_HOST_PARENT_SLOTS;
            if (state_only(s->parent[at].data, s->parent[at].length)) { victim = i; break; }
        }
        for (int i = victim; i < s->parent_count - 1; ++i)
            s->parent[(s->parent_head + i) % PIA_HOST_PARENT_SLOTS] = s->parent[(s->parent_head + i + 1) % PIA_HOST_PARENT_SLOTS];
        --s->parent_count;
    }
    /* Superseded key reports shed while frames wait: each one waiting is lag on both
       screens. */
    for (int i = 0; h->sheddable && s->parent_count > PIA_HOST_SHED_AT && i < s->parent_count - 1;)
    {
        const int at = (s->parent_head + i) % PIA_HOST_PARENT_SLOTS;
        if (!h->sheddable(s->parent[at].data, s->parent[at].length)) { ++i; continue; }
        for (int j = i; j < s->parent_count - 1; ++j)
            s->parent[(s->parent_head + j) % PIA_HOST_PARENT_SLOTS] = s->parent[(s->parent_head + j + 1) % PIA_HOST_PARENT_SLOTS];
        --s->parent_count;
        ++s->shed;
    }
    int at = (s->parent_head + s->parent_count) % PIA_HOST_PARENT_SLOTS;
    memcpy(s->parent[at].data, payload, length);
    s->parent[at].length = (uint8_t)length;
    ++s->parent_count;
}

void pia_host_parent_frame(pia_host_t *h, const uint8_t *payload, size_t length)
{
    if (length == 0 || length > PIA_HOST_PARENT_BYTES) return;
    if (state_only(payload, length) && pia_host_station_count(h) > 0)
    {
        memcpy(h->idle, payload, length);
        h->idle_len = (uint8_t)length;
        h->has_idle = true;
    }
    /* Until the first child is in, the GBA's frames wait for its WA; a child joining a
       running group starts from its own. */
    const bool running = group_running(h);
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
    {
        pia_host_station_t *s = &h->st[i];
        if (s->state == 0 || s->child_disconnected || (running && !s->child_accepted)) continue;
        parent_enqueue(h, s, payload, length);
    }
}

void pia_host_parent_frame_to(pia_host_t *h, int station, const uint8_t *payload, size_t length)
{
    if (station < 0 || station >= PIA_HOST_STATIONS || length == 0 || length > PIA_HOST_PARENT_BYTES) return;
    pia_host_station_t *s = &h->st[station];
    if (s->state == 0 || !s->child_accepted || s->child_disconnected) return;
    parent_enqueue(h, s, payload, length);
}

/* ---- tick ------------------------------------------------------------------ */

static void stream_tick(pia_host_t *h, pia_host_station_t *s)
{
    pia_packet_t frames[MAX_FRAMES];
    int count = pia_reliable_retransmit(&s->reliable, (double)h->now_ms, 2, frames, MAX_FRAMES);
    s->resends += count;
    if (s->child_accepted && !s->child_disconnected)
    {
        const bool paced = h->now_ms - s->last_child_datagram_ms <= CHILD_QUIET_MS;
        const bool room = pia_reliable_pending(&s->reliable) < INFLIGHT_MAX && (!paced || s->credits > 0);
        if (room && s->wg_pending >= 0 && count < MAX_FRAMES)
        {
            uint8_t wg[8] = {0x57, 0x47, 4, 0};
            bin_w32(wg + 4, (uint32_t)s->wg_pending);
            if (queue_reliable(h, s, wg, sizeof(wg), &frames[count])) { ++count; s->wg_pending = -1; }
        }
        uint8_t *wt = h->sc.wt;
        static const uint8_t idle_byte[1];
        int n = -1;
        bool queued = s->parent_count > 0;
        if (queued && room)
        {
            const uint8_t *data = s->parent[s->parent_head].data;
            size_t len = s->parent[s->parent_head].length;
            /* WG 1 ends the join phase, before the first UNI frame. */
            bool uni = is_uni(data, len);
            if (uni && !s->uni_sent && count < MAX_FRAMES)
            {
                uint8_t wg[8] = {0x57, 0x47, 4, 0, 1, 0, 0, 0};
                if (queue_reliable(h, s, wg, sizeof(wg), &frames[count])) { ++count; s->uni_sent = true; }
            }
            if (!uni || s->uni_sent) n = wrap_wt(data, len, h->timestamp, wt, PIA_HOST_FRAME);
        }
        else if (!queued && room && h->tick - s->last_wt_tick >= IDLE_TICKS)
            n = h->has_idle ? wrap_wt(h->idle, h->idle_len, h->timestamp, wt, PIA_HOST_FRAME)
                            : wrap_wt(idle_byte, 1, h->timestamp, wt, PIA_HOST_FRAME);
        if (n > 0 && count < MAX_FRAMES && queue_reliable(h, s, wt, (size_t)n, &frames[count]))
        {
            note_wt(h, s, h->timestamp, frames[count].seq, !queued);
            if (paced) --s->credits;
            ++count;
            s->last_wt_tick = h->tick;
            if (queued)
            {
                s->parent_head = (s->parent_head + 1) % PIA_HOST_PARENT_SLOTS;
                --s->parent_count;
                ++s->parent_frames;
            }
            if (!s->wt_sent) { s->wt_sent = true; s->wg_pending = 0; }
        }
        else if (queued && s->parent_count > 1 && state_only(s->parent[s->parent_head].data, s->parent[s->parent_head].length))
        {
            /* Stream full: a state-only frame gives way to the next. */
            s->parent_head = (s->parent_head + 1) % PIA_HOST_PARENT_SLOTS;
            --s->parent_count;
        }
    }
    send_frames(h, s, frames, count);
}

static void clear_station(pia_host_t *h, pia_host_station_t *s)
{
    pia_reliable_init(&s->reliable, s->pending, &s->pending_data[0][0], PIA_HOST_WINDOW, PIA_HOST_FRAME);
    memset(s->hold, 0, sizeof(s->hold));
    memset(s->wt_ring, 0, sizeof(s->wt_ring));
    memset(s->probes, 0, sizeof(s->probes));
    memset(s->packet_ids, 0, sizeof(s->packet_ids));
    s->state = 0;
    s->id = 0;
    s->ip[0] = 0;
    s->child_stamp_valid = false;
    s->credits = 0;
    s->last_child_datagram_ms = h->now_ms - CHILD_QUIET_MS - 1;
    s->connect_requested = s->child_accepted = s->child_disconnected = false;
    s->parent_head = s->parent_count = 0;
    s->status_acked = s->property_acked = false;
    s->update_acked = 0;
    s->child_alive = s->stream_opened = s->ack_owed = s->wt_sent = s->uni_sent = false;
    s->join_replies = 0;
    s->wg_pending = -1;
    s->next_deliver = -1;
    s->gap_since_ms = 0;
    s->gaps_given_up = 0;
    s->wt_ring_head = 0;
    s->teardown = s->teardown_sent = 0;
    s->silent_noted = 0;
}

/* A station gone from the room. The others get a station list naming it as left; the
   last one out ends the session's numbering. */
static void remove_station(pia_host_t *h, pia_host_station_t *s)
{
    bool others = false;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i) others |= &h->st[i] != s && h->st[i].state >= 2;
    if (s->state >= 2 && others)
    {
        if (h->left_count == PIA_HOST_LEFT)
        {
            memmove(&h->left[0], &h->left[1], sizeof(h->left[0]) * (PIA_HOST_LEFT - 1));
            --h->left_count;
        }
        pia_host_left_t *l = &h->left[h->left_count++];
        memcpy(l->mac, s->mac, 6);
        l->id = s->id;
        l->joined_seq = s->joined_seq;
        l->left_seq = ++h->list_seq;
        l->join_number = s->join_number;
        l->record_len = (uint8_t)s->joiner_record_len;
        memcpy(l->record, s->joiner_record, (size_t)s->joiner_record_len);
        h->member_left = true;
        for (int i = 0; i < PIA_HOST_STATIONS; ++i)
            if (&h->st[i] != s && h->st[i].state >= 2) h->st[i].next_update_ms = h->now_ms;
    }
    clear_station(h, s);
    if (pia_host_station_count(h) == 0)
    {
        /* A new session: its station list and property start over. */
        h->list_seq = 0;
        h->left_count = 0;
        h->member_left = false;
        h->joins = 0;
        h->property_seq = 0;
        h->property_pending = false;
        h->has_idle = false;
    }
}

static void start_teardown(pia_host_t *h, pia_host_station_t *s)
{
    s->teardown = 1;
    s->teardown_sent = 0;
    s->next_teardown_ms = h->now_ms + NOTICE_PERIOD_MS;
}

/* Teardown: three disconnect notices, five removal broadcasts, then the status with the
   slot marked and then cleared. */
static void teardown_tick(pia_host_t *h, pia_host_station_t *s)
{
    if (h->now_ms < s->next_teardown_ms) return;
    int64_t period = BROADCAST_PERIOD_MS;
    switch (s->teardown)
    {
    case 1:
        send_disconnect_notice(h, s);
        period = NOTICE_PERIOD_MS;
        if (++s->teardown_sent >= NOTICE_COPIES) { s->teardown = 2; s->teardown_sent = 0; period = BROADCAST_PERIOD_MS; }
        break;
    case 2:
        send_removed_broadcast(h, s, s->teardown_sent + 1 == BROADCAST_COPIES);
        if (++s->teardown_sent >= BROADCAST_COPIES)
        {
            s->teardown = 3; s->teardown_sent = 0;
            status_changed(h);
        }
        break;
    case 3:
        if (s->status_acked || s->teardown_sent >= BROADCAST_COPIES)
        {
            s->teardown = 4; s->teardown_sent = 0;
            status_changed(h);
        }
        else { send_status(h, s, STATUS_MARKED); ++s->teardown_sent; break; }
        /* fall through */
    case 4:
        if (s->status_acked || s->teardown_sent >= BROADCAST_COPIES) { remove_station(h, s); return; }
        send_status(h, s, STATUS_CLEARED);
        ++s->teardown_sent;
        break;
    default:
        return;
    }
    s->next_teardown_ms = h->now_ms + period;
}

static void station_tick(pia_host_t *h, pia_host_station_t *s)
{
    const int64_t now_ms = h->now_ms;
    if (s->teardown) teardown_tick(h, s);
    if (s->state == 0) return;

    /* Repeated until acknowledged and until the join: a child whose join was lost resends
       it on the next copy. */
    if (s->teardown < 3 && (!s->status_acked || s->state < 2) && now_ms >= s->next_net_ms)
    {
        send_status(h, s, STATUS_NORMAL);
        s->next_net_ms = now_ms + STATUS_REPEAT_MS;
    }
    if (s->state < 2) return;

    if (!s->child_alive && s->join_replies < JOIN_REPLIES && now_ms >= s->next_join_reply_ms)
    {
        send_station_list(h, s);
        send_join_response(h, s);
        ++s->join_replies;
        s->next_join_reply_ms = now_ms + JOIN_REPLY_REPEAT_MS;
        s->next_update_ms = now_ms + UPDATE_REPEAT_MS;
    }
    else if (s->child_alive && s->teardown == 0 && s->update_acked != h->list_seq && now_ms >= s->next_update_ms)
    {
        send_station_list(h, s);
        s->next_update_ms = now_ms + UPDATE_REPEAT_MS;
    }
    if (now_ms >= s->next_probe_ms)
    {
        send_probe(h, s);
        s->next_probe_ms = now_ms + PROBE_PERIOD_MS;
    }
    if (s->child_accepted && h->app_data_len > 0 && !s->property_acked && now_ms >= s->next_property_ms)
    {
        send_property(h, s);
        s->next_property_ms = now_ms + PROPERTY_REPEAT_MS;
    }
    stream_tick(h, s);
}

void pia_host_tick(pia_host_t *h, int64_t now_ms)
{
    h->now_ms = now_ms;
    ++h->tick;
    ++h->timestamp;
    if (h->property_pending && h->app_data_len > 0 && group_running(h))
    {
        ++h->property_seq;
        h->property_pending = false;
        for (int i = 0; i < PIA_HOST_STATIONS; ++i)
        {
            h->st[i].property_acked = false;
            h->st[i].next_property_ms = now_ms;
        }
    }
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
        if (h->st[i].state != 0) station_tick(h, &h->st[i]);
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
        if (h->st[i].state == 3) check_gap(h, &h->st[i]);
}

/* ---- the bridge's side ----------------------------------------------------- */

void pia_host_init(pia_host_t *h, const uint8_t ssid[16], const uint8_t our_mac[6], const char *our_ip,
                   pia_host_send_fn send, pia_host_child_fn deliver, pia_host_log_fn log, void *user)
{
    memset(h, 0, sizeof(*h));
    pia_crypto_init(&h->crypto, ssid);
    memcpy(h->our_mac, our_mac, 6);
    snprintf(h->ours, sizeof(h->ours), "%s", our_ip);
    h->send = send; h->deliver = deliver; h->log = log; h->user = user;
    do h->our_id = (uint16_t)esp_random(); while (h->our_id == 0);
    uint8_t seed[8];
    esp_fill_random(seed, sizeof(seed));
    h->nonce = bin_b64(seed) | 1;
    h->timestamp = 1;
    h->adapter_id = 1;
    h->net_seq = 1;
    h->credit_cap = CREDIT_CAP;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i) clear_station(h, &h->st[i]);
}

void pia_host_set_app_data(pia_host_t *h, const uint8_t *data, int length)
{
    if (length <= 0 || length > PIA_HOST_APP_DATA) return;
    if (length == h->app_data_len && memcmp(h->app_data, data, (size_t)length) == 0) return;
    memcpy(h->app_data, data, (size_t)length);
    h->app_data_len = length;
    h->property_pending = true;
}

int pia_host_station_joined(pia_host_t *h, const uint8_t mac[6], const char *ip, uint8_t index)
{
    pia_host_station_t *s = NULL;
    for (int i = 0; i < PIA_HOST_STATIONS && !s; ++i) if (h->st[i].state == 0) s = &h->st[i];
    if (!s) return -1;
    clear_station(h, s);
    memcpy(s->mac, mac, 6);
    snprintf(s->ip, sizeof(s->ip), "%s", ip);
    s->index = index >= 1 && index <= 5 ? index : 1;
    s->join_number = (uint8_t)++h->joins;
    status_changed(h);
    h->property_pending = h->app_data_len > 0;
    s->state = 1;
    send_status(h, s, STATUS_NORMAL);
    s->next_net_ms = h->now_ms + STATUS_REPEAT_MS;
    emit(h, "status sent");
    return slot_of(h, s);
}

void pia_host_station_left(pia_host_t *h, uint8_t index)
{
    pia_host_station_t *s = NULL;
    for (int i = 0; i < PIA_HOST_STATIONS && !s; ++i) if (h->st[i].state != 0 && h->st[i].index == index) s = &h->st[i];
    if (!s) return;
    /* Membership changes the teardown had not announced yet. */
    const int unannounced = s->teardown < 3 ? 2 : s->teardown == 3 ? 1 : 0;
    remove_station(h, s);
    for (int k = 0; k < unannounced; ++k) status_changed(h);
    emit(h, "child left");
}

int pia_host_connect_pending(const pia_host_t *h)
{
    int best = -1;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i)
    {
        const pia_host_station_t *s = &h->st[i];
        if (s->state == 0 || !s->connect_requested || s->child_accepted || s->child_disconnected) continue;
        if (best < 0 || s->join_number < h->st[best].join_number) best = i;
    }
    return best;
}

static void send_wd(pia_host_t *h, pia_host_station_t *s)
{
    uint8_t wd[6] = {0x57, 0x44, 2, 0, s->connect_id[0], s->connect_id[1]};
    send_to_group(h, wd, sizeof(wd), s);
    emit(h, "WD sent");
}

void pia_host_accept(pia_host_t *h, int station, bool accepted, uint8_t client_slot)
{
    if (station < 0 || station >= PIA_HOST_STATIONS) return;
    pia_host_station_t *s = &h->st[station];
    if (s->state == 0 || !s->connect_requested || s->child_accepted || s->child_disconnected) return;
    if (!accepted)
    {
        s->child_disconnected = true;
        start_teardown(h, s);
        send_wd(h, s);
        return;
    }
    /* One adapter id per group, the next group's 3 on. */
    if (!group_running(h))
    {
        h->wa_id = h->adapter_id;
        h->adapter_id += 3;
    }
    uint8_t wa[10] = {0x57, 0x41, 6, 0};
    bin_w16(wa + 4, h->wa_id);
    memcpy(wa + 6, s->connect_id, 2);
    wa[8] = client_slot;
    s->child_accepted = true;
    s->client_slot = client_slot;
    s->last_wt_tick = h->tick;
    send_to_group(h, wa, sizeof(wa), s);
    emit(h, "WA sent");
}

void pia_host_disconnect_child(pia_host_t *h, int station)
{
    if (station < 0 || station >= PIA_HOST_STATIONS) return;
    pia_host_station_t *s = &h->st[station];
    if (s->state < 2 || s->child_disconnected) return;
    s->child_disconnected = true;
    start_teardown(h, s);
    if (s->connect_requested) send_wd(h, s);
}

bool pia_host_session_up(const pia_host_t *h, int station)
{
    return station >= 0 && station < PIA_HOST_STATIONS && h->st[station].state == 3;
}

int pia_host_station_count(const pia_host_t *h)
{
    int n = 0;
    for (int i = 0; i < PIA_HOST_STATIONS; ++i) n += h->st[i].state != 0;
    return n;
}
