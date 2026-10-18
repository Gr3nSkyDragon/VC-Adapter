#include "ldn_host.h"
#include "ldn_keys.h"
#include "pia_crypto.h"
#include "pia_bin.h"

#include <stdio.h>
#include <string.h>

/* Protocol constants, as in ldn_keys.c. kChallengeKey is a fixed LDN constant
   (kinnay/LDN), not console key material. */
static const uint8_t kAdvHeaderMagic[8] = {0x7f, 0x00, 0x22, 0xaa, 0x04, 0x00, 0x01, 0x01};
static const uint8_t kAuthMagic[6] = {0x00, 0x22, 0xaa, 0x01, 0x02, 0x00};
static const uint8_t kChallengeKey[32] = {
    0xf8, 0x4b, 0x48, 0x7f, 0xb3, 0x72, 0x51, 0xc2, 0x63, 0xbf, 0x11, 0x60, 0x90, 0x36, 0x58, 0x92,
    0x66, 0xaf, 0x70, 0xca, 0x79, 0xb4, 0x4c, 0x93, 0xc7, 0x37, 0x0c, 0x57, 0x69, 0xc0, 0xf6, 0x02};

static size_t name_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

/* Network id as the authentication header carries it: communication id little-endian, the
   word at 10 byte-swapped (ldn_keys.c little_id()). */
static void host_little_id(const ldn_network_t *net, uint8_t out[32])
{
    memcpy(out, net->id, 32);
    bin_w64(out, net->communication_id);
    bin_w16(out + 10, bin_b16(net->id + 10));
}

int ldn_host_encode_advertisement(const ldn_network_t *net, const uint8_t nonce4[4],
                                  uint8_t *out, size_t cap, uint8_t work[LDN_HOST_WORK])
{
    if (net->protocol != 3) return -1;   /* only the authenticated advertisement is produced */
    int count = net->member_count;
    if (count < 1 || count > LDN_MAX_MEMBERS) return -1;
    if (net->app_data_len < 0 || net->app_data_len > LDN_MAX_APP_DATA) return -1;

    uint8_t *plain = work;
    int app_at = 40 + 48 * count;
    int length = app_at + 2 + net->app_data_len;
    if (length > LDN_HOST_WORK) return -1;
    memset(plain, 0, (size_t)length);
    memcpy(plain, net->random, 16);
    bin_wb64(plain + 16, net->challenge);
    plain[24] = (uint8_t)net->security;
    plain[25] = (uint8_t)net->policy;
    bin_wb16(plain + 26, (uint16_t)net->app_version);
    /* plain[28..36] is padding. The channel word holds the band in its top bits, 2 for
       2.4 GHz. */
    bin_wb16(plain + 36, (uint16_t)(2 << 10 | net->channel));
    plain[38] = net->maximum;
    plain[39] = (uint8_t)count;
    for (int i = 0; i < count; ++i)
    {
        uint8_t *m = plain + 40 + 48 * i;
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (sscanf(net->members[i].ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
        m[0] = (uint8_t)a; m[1] = (uint8_t)b; m[2] = (uint8_t)c; m[3] = (uint8_t)d;
        memcpy(m + 4, net->members[i].mac, 6);
        m[10] = net->members[i].index;
        m[11] = net->members[i].platform;
        memcpy(m + 12, net->members[i].name, name_len(net->members[i].name, 32));
    }
    bin_wb16(plain + app_at, (uint16_t)net->app_data_len);
    memcpy(plain + app_at + 2, net->app_data, (size_t)net->app_data_len);

    int total = 68 + length;
    if ((size_t)total > cap) return -1;
    memcpy(out, kAdvHeaderMagic, 8);
    memset(out + 8, 0, 4);                        /* raw[8..12] unused */
    memcpy(out + 12, net->id, 32);
    out[44] = (uint8_t)net->version;
    out[45] = 3;                                  /* protocol-3 advertisement */
    bin_wb16(out + 46, (uint16_t)length);
    memcpy(out + 48, nonce4, 4);

    uint8_t key[16];
    if (!ldn_keys_derive(net->protocol, net->id, 32, true, key)) return -1;
    uint8_t nonce[12] = {0};
    memcpy(nonce, nonce4, 4);
    /* AAD is raw[12..52]; the tag precedes the ciphertext at raw[52..68]. */
    bool ok = pia_gcm(key, 16, nonce, 12, out + 12, 40, plain, (size_t)length,
                      out + 68, out + 52, 16, true);
    memset(key, 0, sizeof(key));
    return ok ? total : -1;
}

bool ldn_host_parse_request(const ldn_network_t *net, const uint8_t *frame, size_t len,
                            ldn_host_request_t *out, uint8_t work[LDN_HOST_WORK])
{
    if (len < 78 || memcmp(frame, kAuthMagic, 6) != 0) return false;
    const uint8_t *header = frame + 6;
    uint8_t expected[32];
    host_little_id(net, expected);
    if (header[0] != net->version || header[3] != 0 ||
        header[5] != (net->protocol == 3 ? 1 : 0) ||
        memcmp(header + 8, expected, 32) != 0 ||
        memcmp(header + 40, net->random, 16) != 0)
        return false;

    int size = header[1] | header[4] << 8;
    if (len != (size_t)(78 + size + (net->protocol == 3 ? 16 : 0))) return false;

    uint8_t joiner_random[16];
    memcpy(joiner_random, header + 56, 16);

    uint8_t *payload = work;
    if (size > LDN_HOST_WORK) return false;
    if (net->protocol == 3)
    {
        uint8_t key[16];
        if (!ldn_keys_derive(net->protocol, joiner_random, 16, false, key)) return false;
        bool ok = pia_gcm(key, 16, header, 12, header, 72, frame + 94, (size_t)size,
                          payload, (uint8_t *)(frame + 78), 16, false);
        memset(key, 0, sizeof(key));
        if (!ok) return false;
    }
    else memcpy(payload, frame + 78, (size_t)size);

    memset(out, 0, sizeof(*out));
    memcpy(out->name, payload, 32);
    out->name[32] = 0;
    out->app_version = bin_b16(payload + 32);
    out->platform = payload[34];
    memcpy(out->joiner_random, joiner_random, 16);

    if (net->version >= 3)
    {
        if (size != 868) return false;
        const uint8_t *body = payload + 148;
        uint8_t digest[32];
        pia_hmac_sha256(kChallengeKey, sizeof(kChallengeKey), body, 720, digest);
        if (memcmp(payload + 104, digest, 32) != 0) return false;
        if (bin_u64(body + 8) != net->challenge) return false;   /* challenge is little-endian here */
        memcpy(out->nonce, body + 16, 8);
        memcpy(out->device, body + 24, 8);
        memcpy(out->extra, body + 32, 16);
    }
    return true;
}

int ldn_host_build_response(const ldn_network_t *net, const ldn_host_request_t *req,
                            const uint8_t host_device[8], bool followup, uint8_t *out, size_t cap,
                            uint8_t work[LDN_HOST_WORK])
{
    const int payload_len = followup ? 132 : 388;
    int total = 78 + payload_len + (net->protocol == 3 ? 16 : 0);
    if ((size_t)total > cap) return -1;

    /* The host's platform byte, then zeros up to the challenge. */
    uint8_t *payload = work;
    memset(payload, 0, (size_t)payload_len);
    payload[0] = net->members[0].platform;
    if (!followup)
    {
        /* 256-byte challenge reply at 132, HMAC over block[48..]: the joiner's nonce,
           device and extra bytes, and the host's device. */
        uint8_t *block = payload + 132;
        block[52] = 2;                            /* flags, little-endian */
        memcpy(block + 56, req->nonce, 8);
        memcpy(block + 64, req->device, 8);
        memcpy(block + 72, host_device, 8);
        memcpy(block + 80, req->extra, 16);
        uint8_t digest[32];
        pia_hmac_sha256(kChallengeKey, sizeof(kChallengeKey), block + 48,
                        (size_t)(payload_len - 132 - 48), digest);
        memcpy(block + 4, digest, 32);
    }

    uint8_t header[72];
    memset(header, 0, sizeof(header));
    header[0] = (uint8_t)net->version;
    header[1] = (uint8_t)payload_len;
    /* header[2] 0 = accepted, header[3] 1 = response */
    header[3] = 1;
    header[4] = (uint8_t)(payload_len >> 8);
    header[5] = (uint8_t)(net->protocol == 3 ? 1 : 0);
    host_little_id(net, header + 8);
    memcpy(header + 40, net->random, 16);
    memcpy(header + 56, req->joiner_random, 16);

    memcpy(out, kAuthMagic, 6);
    memcpy(out + 6, header, 72);
    if (net->protocol == 3)
    {
        uint8_t key[16];
        if (!ldn_keys_derive(net->protocol, req->joiner_random, 16, false, key)) return -1;
        bool ok = pia_gcm(key, 16, header, 12, header, 72, payload, (size_t)payload_len,
                          out + 94, out + 78, 16, true);
        memset(key, 0, sizeof(key));
        if (!ok) return -1;
    }
    else memcpy(out + 78, payload, (size_t)payload_len);
    return total;
}

#ifdef ESP_PLATFORM

#include <inttypes.h>
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "ldn_session.h"
#include "esp_wifi_default.h"
#include "esp_private/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ldn_private_wifi.h"
#include "ldn_udp.h"
#include "ldn_wire.h"

#define printf ldn_wire_printf

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define HOST_FRAME_MAX 1024   /* an authentication request is 976 bytes */
#define ASSOC_TIMEOUT_MS 15000
#define REASON_LEAVING 3   /* WLAN_REASON_DEAUTH_LEAVING */

/* The RSN element a Switch station sends (kinnay/LDN): CCMP, PSK, capabilities 0x000c. */
static uint8_t s_host_rsn_ie[] = {
    0x30, 0x14, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
    0x0c, 0x00,
};

typedef struct { ldn_host_member_t pub; int64_t assoc_ms; } host_station_t;
typedef struct { uint16_t len; uint8_t bytes[HOST_FRAME_MAX]; } host_frame_t;

static struct
{
    bool running;
    uint8_t ap_mac[6], ssid[16], ccmp_key[16], id[32], random[16], device[8];
    const uint8_t *app_data;        /* the caller's, valid while the host runs */
    uint64_t communication_id, challenge;
    int channel, app_version, maximum, app_data_len;
    char host_name[33], ip[16];
    uint8_t subnet;
    uint32_t adv_nonce;             /* stepped whenever the advertisement changes */
    ldn_host_scratch_t *sc;
    esp_netif_t *ap_netif;
    host_station_t stations[LDN_MAX_MEMBERS];
    int station_count;
    size_t adv_len;
    int64_t next_adv;
    bool (*orig_ap_join)(ldn_wpa_station_join_param_t *);
    bool (*orig_ap_remove)(uint8_t *);
    uint8_t *(*orig_ap_get_wpa_ie)(size_t *);
    void (*joined)(const ldn_host_member_t *);
    void (*left)(const ldn_host_member_t *);
    QueueHandle_t rx;
    int64_t joined_ms, next_probe_ms;
} s_host;

/* Joins and leaves from the Wi-Fi task's hooks, for the poll loop. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_join_q[LDN_MAX_MEMBERS][6];
static uint8_t s_leave_q[LDN_MAX_MEMBERS][6];
static int s_join_n, s_leave_n;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void hex16(const uint8_t *in, char out[33])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) { out[2 * i] = digits[in[i] >> 4]; out[2 * i + 1] = digits[in[i] & 15]; }
    out[32] = 0;
}

static int find_station(const uint8_t mac[6])
{
    for (int i = 0; i < s_host.station_count; ++i)
        if (memcmp(s_host.stations[i].pub.mac, mac, 6) == 0) return i;
    return -1;
}

static int next_free_index(void)
{
    for (int index = 1; index < s_host.maximum && index < LDN_MAX_MEMBERS; ++index)
    {
        bool used = false;
        for (int i = 0; i < s_host.station_count; ++i) used |= s_host.stations[i].pub.index == index;
        if (!used) return index;
    }
    return -1;
}

/* The advertised network: the host is member 0, then authenticated stations. */
static void build_net(ldn_network_t *net)
{
    memset(net, 0, sizeof(*net));
    net->protocol = 3;
    net->version = 4;
    net->channel = s_host.channel;
    net->security = 1;
    net->policy = 0;
    net->app_version = s_host.app_version;
    net->maximum = s_host.maximum;
    net->communication_id = s_host.communication_id;
    net->challenge = s_host.challenge;
    memcpy(net->id, s_host.id, 32);
    memcpy(net->ssid, s_host.ssid, 16);
    memcpy(net->random, s_host.random, 16);
    memcpy(net->host, s_host.ap_mac, 6);
    if (s_host.app_data_len > 0) memcpy(net->app_data, s_host.app_data, (size_t)s_host.app_data_len);
    net->app_data_len = s_host.app_data_len;

    ldn_member_t *h = &net->members[0];
    memcpy(h->mac, s_host.ap_mac, 6);
    snprintf(h->ip, sizeof(h->ip), "%s", s_host.ip);
    snprintf(h->name, sizeof(h->name), "%s", s_host.host_name);
    h->index = 0;
    net->member_count = 1;
    for (int i = 0; i < s_host.station_count && net->member_count < LDN_MAX_MEMBERS; ++i)
    {
        if (!s_host.stations[i].pub.authenticated) continue;
        ldn_member_t *m = &net->members[net->member_count++];
        memcpy(m->mac, s_host.stations[i].pub.mac, 6);
        snprintf(m->ip, sizeof(m->ip), "%s", s_host.stations[i].pub.ip);
        snprintf(m->name, sizeof(m->name), "%s", s_host.stations[i].pub.name);
        m->index = s_host.stations[i].pub.index;
        m->platform = s_host.stations[i].pub.platform;
    }
}

static void reencode(void)
{
    ldn_host_scratch_t *sc = s_host.sc;
    build_net(&sc->net);
    uint8_t nonce4[4];
    bin_wb32(nonce4, ++s_host.adv_nonce);
    int n = ldn_host_encode_advertisement(&sc->net, nonce4, sc->adv, sizeof(sc->adv), sc->work);
    s_host.adv_len = n > 0 ? (size_t)n : 0;
}

/* The advertisement as an action frame from the AP: to broadcast, BSSID broadcast, as a
   Switch sends it. */
static void send_advertisement(void)
{
    if (s_host.adv_len == 0) return;
    uint8_t *frame = s_host.sc->frame;
    memset(frame, 0, 24);
    frame[0] = 0xd0;                     /* management, action */
    memset(frame + 4, 0xff, 6);
    memcpy(frame + 10, s_host.ap_mac, 6);
    memset(frame + 16, 0xff, 6);
    memcpy(frame + 24, s_host.sc->adv, s_host.adv_len);
    static esp_err_t last;
    const esp_err_t sent = esp_wifi_80211_tx(WIFI_IF_AP, frame, (int)(24 + s_host.adv_len), true);
    if (sent != last) { if (sent != ESP_OK) printf("LDN_HOST advertisement not sent: %d\n", (int)sent); last = sent; }
}

static void send_action_reply(const uint8_t dst[6], const uint8_t *body, size_t len)
{
    uint8_t *frame = s_host.sc->frame;
    if (14 + len > sizeof(s_host.sc->frame)) return;
    memcpy(frame, dst, 6);
    memcpy(frame + 6, s_host.ap_mac, 6);
    frame[12] = 0x88;
    frame[13] = 0xb7;
    memcpy(frame + 14, body, len);
    const int sent = esp_wifi_internal_tx(WIFI_IF_AP, frame, (uint16_t)(14 + len));
    if (sent != 0) printf("LDN_HOST reply not sent: %d\n", sent);
}

static void remove_station(int slot)
{
    if (slot < 0 || slot >= s_host.station_count) return;
    for (int i = slot; i + 1 < s_host.station_count; ++i) s_host.stations[i] = s_host.stations[i + 1];
    --s_host.station_count;
}

/* Replaces the stock join, which would start a 4-way handshake LDN does not use: register
   the station and answer the association here; the poll loop then installs the network
   key and opens the port (open_station). */
static bool host_ap_join(ldn_wpa_station_join_param_t *join)
{
    struct hostapd_data *hapd = hostapd_get_hapd_data();
    if (!hapd || !join || !join->bssid) return false;
    struct sta_info *sta = ap_get_sta(hapd, join->bssid);
    if (!sta) sta = ap_sta_add(hapd, join->bssid);
    if (!sta) return false;
    if (esp_send_assoc_resp(hapd, join->bssid, 0, true, join->subtype) != 0) return false;
    if (join->pmf_enable) *join->pmf_enable = false;
    if (join->pairwise_cipher) *join->pairwise_cipher = 3;   /* the CCMP bit */
    *join->sm = sta;
    portENTER_CRITICAL(&s_lock);
    if (s_join_n < LDN_MAX_MEMBERS) memcpy(s_join_q[s_join_n++], join->bssid, 6);
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void open_station(const uint8_t mac[6])
{
    uint8_t addr[6];
    memcpy(addr, mac, 6);
    const int key = esp_wifi_set_ap_key_internal(LDN_WIFI_WPA_ALG_CCMP, addr, 0, s_host.ccmp_key, sizeof(s_host.ccmp_key));
    if (key != 0) printf("LDN_HOST station key not installed: %d\n", key);
    if (!esp_wifi_wpa_ptk_init_done_internal(addr)) printf("LDN_HOST station port not opened\n");
}

/* RSN element for beacons and probe responses: the Switch's, whose capabilities hostapd's
   lacks. */
static uint8_t *host_ap_get_wpa_ie(size_t *length)
{
    *length = sizeof(s_host_rsn_ie);
    return s_host_rsn_ie;
}

static bool host_ap_remove(uint8_t *mac)
{
    if (mac)
    {
        portENTER_CRITICAL(&s_lock);
        if (s_leave_n < LDN_MAX_MEMBERS) memcpy(s_leave_q[s_leave_n++], mac, 6);
        portEXIT_CRITICAL(&s_lock);
    }
    /* The stock handler frees the station. */
    return s_host.orig_ap_remove ? s_host.orig_ap_remove(mac) : true;
}

static bool our_ip_bytes(uint8_t out[4])
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s_host.ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

/* ARP request to the station. Its own ARP for us goes to the group address, which the AP
   path never delivers; answering ours teaches it our address. */
static void probe_arp(const host_station_t *st)
{
    uint8_t ours[4], theirs[4];
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!our_ip_bytes(ours) || sscanf(st->pub.ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return;
    theirs[0] = (uint8_t)a; theirs[1] = (uint8_t)b; theirs[2] = (uint8_t)c; theirs[3] = (uint8_t)d;
    uint8_t frame[14 + 28] = {0};
    memcpy(frame, st->pub.mac, 6);
    memcpy(frame + 6, s_host.ap_mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;
    uint8_t *r = frame + 14;
    r[1] = 1; r[2] = 0x08; r[4] = 6; r[5] = 4; r[7] = 1;
    memcpy(r + 8, s_host.ap_mac, 6);
    memcpy(r + 14, ours, 4);
    memcpy(r + 24, theirs, 4);
    esp_wifi_internal_tx(WIFI_IF_AP, frame, sizeof(frame));
}

static esp_err_t host_ap_rx(void *buffer, uint16_t length, void *eb)
{
    const uint8_t *bytes = buffer;
    if (length >= 14 && bytes[12] == 0x88 && bytes[13] == 0xb7 && memcmp(bytes, s_host.ap_mac, 6) == 0)
    {
        if (length <= HOST_FRAME_MAX)
        {
            host_frame_t frame;
            frame.len = length;
            memcpy(frame.bytes, bytes, length);
            xQueueSend(s_host.rx, &frame, 0);
        }
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    return esp_netif_receive(s_host.ap_netif, buffer, length, eb);
}

static void on_associated(const uint8_t mac[6])
{
    int slot = find_station(mac);
    if (slot >= 0) { s_host.stations[slot].assoc_ms = now_ms(); return; }
    int index = next_free_index();
    if (index < 0) { printf("LDN_HOST_ERROR ROOM_FULL\n"); return; }
    host_station_t *st = &s_host.stations[s_host.station_count++];
    memset(st, 0, sizeof(*st));
    memcpy(st->pub.mac, mac, 6);
    st->pub.index = (uint8_t)index;
    snprintf(st->pub.ip, sizeof(st->pub.ip), "169.254.%u.%u", s_host.subnet, (unsigned)(index + 1) & 0xffu);
    st->assoc_ms = now_ms();
    printf("LDN_HOST station associated " MACSTR "\n", MAC2STR(mac));
}

static void on_left(const uint8_t mac[6])
{
    int slot = find_station(mac);
    if (slot < 0) return;
    host_station_t st = s_host.stations[slot];
    remove_station(slot);
    if (st.pub.authenticated)
    {
        ldn_udp_remove_peer(st.pub.ip);
        reencode();
        if (s_host.left) s_host.left(&st.pub);
        printf("LDN_HOST member left\n");
    }
}

static void handle_auth(const uint8_t *src, const uint8_t *body, size_t n)
{
    ldn_host_scratch_t *sc = s_host.sc;
    build_net(&sc->net);
    ldn_host_request_t req;
    if (!ldn_host_parse_request(&sc->net, body, n, &req, sc->work)) return;
    int slot = find_station(src);
    if (slot < 0) return;   /* not associated */
    printf("LDN_HOST auth request from " MACSTR " name=%s app=%u platform=%u\n",
           MAC2STR(src), req.name, (unsigned)req.app_version, (unsigned)req.platform);

    int len = ldn_host_build_response(&sc->net, &req, s_host.device, false, sc->response, sizeof(sc->response), sc->work);
    if (len < 0) return;
    send_action_reply(src, sc->response, (size_t)len);

    host_station_t *st = &s_host.stations[slot];
    if (!st->pub.authenticated)
    {
        st->pub.authenticated = true;
        st->pub.platform = req.platform;
        snprintf(st->pub.name, sizeof(st->pub.name), "%s", req.name);
        ldn_udp_add_peer(st->pub.ip, src);
        s_host.joined_ms = now_ms();
        s_host.next_probe_ms = s_host.joined_ms + 200;
        reencode();
        if (s_host.joined) s_host.joined(&st->pub);
        printf("LDN_HOST member %d joined ip=%s\n", st->pub.index, st->pub.ip);
    }
}

bool ldn_host_start(const ldn_host_config_t *cfg, ldn_host_scratch_t *scratch)
{
    if (s_host.running) ldn_host_stop();
    if (!cfg || cfg->channel < 1 || cfg->channel > 11) { printf("LDN_HOST_ERROR BAD_CHANNEL\n"); return false; }
    if (!ldn_keys_supports(3)) { printf("LDN_HOST_ERROR NO_KEYS\n"); return false; }
    if (!scratch) return false;

    /* The AP netif, RX queue and handlers are kept across sessions. */
    esp_netif_t *ap_netif = s_host.ap_netif;
    QueueHandle_t rx = s_host.rx;
    void (*joined)(const ldn_host_member_t *) = s_host.joined;
    void (*left)(const ldn_host_member_t *) = s_host.left;
    memset(&s_host, 0, sizeof(s_host));
    s_host.sc = scratch;
    s_host.ap_netif = ap_netif;
    s_host.rx = rx;
    s_host.joined = joined;
    s_host.left = left;
    s_host.channel = cfg->channel;
    s_host.communication_id = cfg->communication_id;
    s_host.app_version = cfg->app_version;
    s_host.maximum = cfg->maximum ? cfg->maximum : LDN_MAX_MEMBERS;
    if (s_host.maximum > LDN_MAX_MEMBERS) s_host.maximum = LDN_MAX_MEMBERS;
    snprintf(s_host.host_name, sizeof(s_host.host_name), "%s", cfg->host_name);
    if (cfg->app_data_len > 0 && cfg->app_data_len <= LDN_MAX_APP_DATA)
    {
        s_host.app_data = cfg->app_data;
        s_host.app_data_len = cfg->app_data_len;
    }

    /* Room identity, fixed for its life: random SSID, advertisement random, challenge and
       device id. */
    esp_fill_random(s_host.ssid, sizeof(s_host.ssid));
    esp_fill_random(s_host.random, sizeof(s_host.random));
    esp_fill_random(s_host.device, sizeof(s_host.device));
    s_host.challenge = ((uint64_t)esp_random() << 32) | esp_random();
    s_host.adv_nonce = esp_random();
    bin_wb64(s_host.id, s_host.communication_id);
    bin_wb16(s_host.id + 10, cfg->scene_id);
    memcpy(s_host.id + 16, s_host.ssid, 16);
    s_host.subnet = (uint8_t)(1 + (esp_random() % 254));
    snprintf(s_host.ip, sizeof(s_host.ip), "169.254.%u.1", s_host.subnet);

    /* Static: the main task's stack is needed by esp_wifi_start. */
    ldn_network_t *derive = &s_host.sc->net;
    memset(derive, 0, sizeof(*derive));
    derive->protocol = 3;
    memcpy(derive->random, s_host.random, 16);
    if (!ldn_keys_network(derive, s_host.ccmp_key)) { printf("LDN_HOST_ERROR KEY_DERIVE\n"); return false; }

    /* AP mode on the chosen channel, random locally administered MAC. */
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_fill_random(s_host.ap_mac, 6);
    s_host.ap_mac[0] = (s_host.ap_mac[0] & 0xfc) | 2;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) { printf("LDN_HOST_ERROR SET_MODE\n"); return false; }
    if (esp_wifi_set_mac(WIFI_IF_AP, s_host.ap_mac) != ESP_OK) { printf("LDN_HOST_ERROR SET_MAC\n"); return false; }

    if (!s_host.ap_netif)
    {
        s_host.ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_host.ap_netif) { printf("LDN_HOST_ERROR NETIF\n"); return false; }
    }
    if (!s_host.rx) s_host.rx = xQueueCreate(3, sizeof(host_frame_t));
    if (!s_host.rx) { printf("LDN_HOST_ERROR NO_MEMORY\n"); return false; }
    xQueueReset(s_host.rx);
    portENTER_CRITICAL(&s_lock);
    s_join_n = s_leave_n = 0;
    portEXIT_CRITICAL(&s_lock);

    char ssid_hex[33];
    hex16(s_host.ssid, ssid_hex);
    wifi_config_t config = {0};
    memcpy(config.ap.ssid, ssid_hex, 32);
    config.ap.ssid_len = 32;
    config.ap.channel = (uint8_t)s_host.channel;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    memcpy(config.ap.password, "gblink00", 8);    /* unused: LDN authenticates, not the PSK */
    config.ap.max_connection = (uint8_t)(s_host.maximum > 1 ? s_host.maximum - 1 : 1);
    config.ap.beacon_interval = 100;
    config.ap.ssid_hidden = 1;                    /* as a Switch's beacons */
    config.ap.pmf_cfg.capable = false;
    config.ap.pmf_cfg.required = false;
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) { printf("LDN_HOST_ERROR CONFIG\n"); return false; }
    /* 802.11b/g as a Switch: no HT elements. */
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    /* Hooked before esp_wifi_start, which reads the RSN element. */
    if (wpa_cb)
    {
        s_host.orig_ap_join = wpa_cb->wpa_ap_join;
        s_host.orig_ap_remove = wpa_cb->wpa_ap_remove;
        s_host.orig_ap_get_wpa_ie = wpa_cb->wpa_ap_get_wpa_ie;
        wpa_cb->wpa_ap_join = host_ap_join;
        wpa_cb->wpa_ap_remove = host_ap_remove;
        wpa_cb->wpa_ap_get_wpa_ie = host_ap_get_wpa_ie;
    }
    if (esp_wifi_start() != ESP_OK) { printf("LDN_HOST_ERROR START\n"); return false; }
    bridge_wifi_tx_power();
#if CONFIG_BRIDGE_HOST_MAX_TX_POWER
    int8_t power = 0;
    if (esp_wifi_get_max_tx_power(&power) != ESP_OK || power > CONFIG_BRIDGE_HOST_MAX_TX_POWER)
        esp_wifi_set_max_tx_power(CONFIG_BRIDGE_HOST_MAX_TX_POWER);
#endif
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_dhcps_stop(s_host.ap_netif);
    esp_netif_ip_info_t info = {0};
    esp_netif_str_to_ip4(s_host.ip, &info.ip);
    esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
    esp_netif_set_ip_info(s_host.ap_netif, &info);

    /* Group key now, pairwise keys per station on association. */
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    esp_wifi_set_ap_key_internal(LDN_WIFI_WPA_ALG_CCMP, broadcast, 1, s_host.ccmp_key, sizeof(s_host.ccmp_key));

    esp_wifi_internal_reg_rxcb(WIFI_IF_AP, host_ap_rx);
    ldn_udp_host_start(s_host.ap_netif, s_host.ip);

    reencode();
    s_host.next_adv = esp_timer_get_time();
    s_host.running = true;
    printf("LDN_HOST started ch=%d ssid=%s\n", s_host.channel, ssid_hex);
    return true;
}

void ldn_host_stop(void)
{
    if (!s_host.running) return;
    for (int i = 0; i < s_host.station_count; ++i)
    {
        uint8_t mac[6];
        memcpy(mac, s_host.stations[i].pub.mac, 6);
        esp_wifi_ap_deauth_internal(mac, REASON_LEAVING);
    }
    if (wpa_cb)
    {
        if (s_host.orig_ap_join) wpa_cb->wpa_ap_join = s_host.orig_ap_join;
        if (s_host.orig_ap_remove) wpa_cb->wpa_ap_remove = s_host.orig_ap_remove;
        if (s_host.orig_ap_get_wpa_ie) wpa_cb->wpa_ap_get_wpa_ie = s_host.orig_ap_get_wpa_ie;
    }
    ldn_udp_stop();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);   /* back to station mode for the joiner */
    s_host.running = false;
    s_host.station_count = 0;
    memset(s_host.ccmp_key, 0, sizeof(s_host.ccmp_key));
    printf("LDN_HOST stopped\n");
}

bool ldn_host_running(void) { return s_host.running; }

void ldn_host_poll(void)
{
    if (!s_host.running) return;

    uint8_t joins[LDN_MAX_MEMBERS][6], leaves[LDN_MAX_MEMBERS][6];
    int nj, nl;
    portENTER_CRITICAL(&s_lock);
    nj = s_join_n; memcpy(joins, s_join_q, (size_t)nj * 6); s_join_n = 0;
    nl = s_leave_n; memcpy(leaves, s_leave_q, (size_t)nl * 6); s_leave_n = 0;
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < nj; ++i) { open_station(joins[i]); on_associated(joins[i]); }
    for (int i = 0; i < nl; ++i) on_left(leaves[i]);

    host_frame_t frame;
    while (xQueueReceive(s_host.rx, &frame, 0) == pdTRUE)
        handle_auth(frame.bytes + 6, frame.bytes + 14, frame.len - 14);

    int64_t now_us = esp_timer_get_time();
    if (now_us >= s_host.next_adv) { send_advertisement(); s_host.next_adv = now_us + 100000; }

    int64_t now = now_ms();
    /* Repeated after a join in case a copy is lost. */
    if (s_host.joined_ms && now - s_host.joined_ms < 12000 && now >= s_host.next_probe_ms)
    {
        for (int i = 0; i < s_host.station_count; ++i)
            if (s_host.stations[i].pub.authenticated) probe_arp(&s_host.stations[i]);
        s_host.next_probe_ms = now + 1000;
    }
    for (int i = 0; i < s_host.station_count; ++i)
        if (!s_host.stations[i].pub.authenticated && now - s_host.stations[i].assoc_ms > ASSOC_TIMEOUT_MS)
        {
            uint8_t mac[6];
            memcpy(mac, s_host.stations[i].pub.mac, 6);
            esp_wifi_ap_deauth_internal(mac, REASON_LEAVING);
            remove_station(i--);
        }

    ldn_udp_heartbeat();
    ldn_udp_poll(true);
}

void ldn_host_set_app_data(const uint8_t *data, int length)
{
    if (length < 0 || length > LDN_MAX_APP_DATA) return;
    s_host.app_data = data;
    s_host.app_data_len = length;
    if (s_host.running) reencode();
}

int ldn_host_member_count(void) { return s_host.station_count; }

const ldn_host_member_t *ldn_host_member(int i)
{
    return i >= 0 && i < s_host.station_count ? &s_host.stations[i].pub : NULL;
}

void ldn_host_set_handlers(void (*joined)(const ldn_host_member_t *),
                           void (*left)(const ldn_host_member_t *))
{
    s_host.joined = joined;
    s_host.left = left;
}

const char *ldn_host_ip(void) { return s_host.ip; }
void ldn_host_mac(uint8_t out[6]) { memcpy(out, s_host.ap_mac, 6); }
const uint8_t *ldn_host_ssid(void) { return s_host.ssid; }

#endif /* ESP_PLATFORM */
