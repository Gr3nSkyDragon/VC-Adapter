#include "pia_host.h"

#include <stdio.h>
#include <string.h>
#include "esp_random.h"

#define PIA_PORT 0x3039
#define STATUS_REPEAT_MS 500
#define PROPERTY_REPEAT_MS 500
#define JOIN_REPLY_REPEAT_MS 500
#define JOIN_REPLIES 5
#define FIRST_PROBE_MS 117
#define PROBE_PERIOD_MS 302
#define IDLE_TICKS 16
#define NOTICE_COPIES 3           /* kind 9, 1 s apart */
#define NOTICE_PERIOD_MS 1000
#define BROADCAST_COPIES 5        /* kind 13 and each 0x11 form, 0.5 s apart */
#define BROADCAST_PERIOD_MS 500
#define MAX_FRAMES PIA_HOST_FRAMES
/* Frames in flight to the child, below its 20-slot receive queue. */
#define INFLIGHT_MAX 12
/* The child's game runs a link frame only when a parent frame is waiting and sends one
   datagram per frame run. Each datagram from it allows one data frame to it, so no more
   than CREDIT_CAP wait at the child; the cap must cover the round trip or the child
   stalls between frames. While the child is quiet only INFLIGHT_MAX applies. */
#define CREDIT_CAP 6
#define CHILD_QUIET_MS 250


/* Per-console value in the host's station record; the same in every captured session. */
static const uint8_t kHostBlob[11] = {0x38, 0xf9, 0x50, 0x7e, 0x55, 0xd0, 0x40, 0x19, 0x13, 0xc5, 0x89};
static const char kDefaultName[] = "GBA";

enum { STATUS_NORMAL, STATUS_MARKED, STATUS_CLEARED };

static void emit(pia_host_t *h, const char *message) { if (h->log) h->log(message, h->user); }

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

/* ---- datagram assembly ----------------------------------------------------- */

/* Packet ids: one counter for the 0001 channel (protocol 3, kind 5), one for the child's
   id (protocol 10, kinds 2, 9, 13). */
static uint16_t next_packet_id(pia_host_t *h, uint16_t dst)
{
    uint16_t *counter = &h->packet_ids[dst == 1 ? 0 : 1];
    if (*counter == 0) *counter = 1;
    uint16_t id = *counter;
    *counter = (uint16_t)(id == 65535 ? 1 : id + 1);
    return id;
}

static void send_messages(pia_host_t *h, const pia_message_t *messages, int count, uint16_t dst, uint16_t src,
                          int packet, bool zip, bool establishing, bool footer)
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
        bin_wb16(body + length, h->station_id);
        length += 2;
    }
    size_t padding = (16 - (length & 15)) & 15;
    if (length + padding > sizeof(sc->body)) return;
    memset(body + length, 0xff, padding);
    length += padding;

    uint16_t pid = packet >= 0 ? (uint16_t)packet : next_packet_id(h, dst);
    uint8_t flags = (uint8_t)((padding << 4) | (zipped ? 1 : 0) | (establishing ? 2 : 0));
    int n = pia_encrypt(&h->crypto, body, length, h->ours, dst, src, pid, h->nonce++, flags,
                        footer ? 2 : 0, datagram, sizeof(sc->datagram));
    if (h->nonce == 0) h->nonce = 1;
    if (n < 0) return;
    h->send(datagram, (size_t)n, h->station_ip, h->user);
    ++h->sent;
}

/* Session message: dst 0000, establishing, no footer. */
static void send_session(pia_host_t *h, uint8_t protocol, const uint8_t *payload, size_t len, uint16_t src,
                         int packet, bool zip)
{
    pia_message_t m = {.protocol = protocol, .payload = payload, .length = (uint16_t)len, .has_flags = false};
    send_messages(h, &m, 1, 0, src, packet, zip, true, false);
}

/* Connected message to the child or the 0001 channel, footer = the child's id. */
static void send_connected(pia_host_t *h, uint8_t protocol, const uint8_t *payload, size_t len, uint16_t dst, bool zip)
{
    pia_message_t m = {.protocol = protocol, .payload = payload, .length = (uint16_t)len, .has_flags = false};
    send_messages(h, &m, 1, dst, h->our_id, -1, zip, false, true);
}

/* ---- protocol 1: network status and property ------------------------------- */

static void send_status(pia_host_t *h, int form)
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
    for (int i = 0; i < 6; ++i)
    {
        uint8_t *e = p + 30 + 22 * i;
        e[1] = 0xff;
        const char *ip = NULL;
        if (i == 0) ip = h->ours;
        else if (i == h->station_index && form != STATUS_CLEARED) ip = h->station_ip;
        if (!ip || !ip_bytes(ip, e + 4)) continue;
        e[1] = (uint8_t)i;
        if (form == STATUS_MARKED && i != 0) e[3] = 1;
        bin_wb16(e + 20, PIA_PORT);
    }
    /* Unconnected form once the child's connection is gone. */
    send_session(h, 1, p, sizeof(p), form == STATUS_NORMAL ? h->our_id : 0, 0, true);
}

static void send_property(pia_host_t *h)
{
    uint8_t *p = h->sc.property;
    int len = h->app_data_len, a = len < 92 ? len : 92;
    memset(p, 0, 38);
    p[0] = 1; p[1] = 0x50;
    bin_wb16(p + 2, (uint16_t)len);
    bin_wb32(p + 4, h->property_seq);
    bin_wb32(p + 12, h->crypto.net_id);
    p[17] = 2; p[19] = 6;                      /* stations, of a maximum of six */
    p[26] = 0x57; p[27] = 0x0f; p[28] = 1; p[29] = 1;
    bin_wb32(p + 30, (uint32_t)a);             /* game header length, string length */
    bin_wb32(p + 34, (uint32_t)(len - a));
    memcpy(p + 38, h->app_data, (size_t)len);
    send_session(h, 1, p, 38 + (size_t)len, h->our_id, 0, true);
}

/* ---- protocol 13: session -------------------------------------------------- */

static void send_station_list(pia_host_t *h)
{
    uint8_t *p = h->sc.station_list;
    const uint8_t *name;
    int name_len;
    host_name(h, &name, &name_len);
    uint8_t our_ip[4] = {0}, station_ip[4] = {0};
    ip_bytes(h->ours, our_ip);
    ip_bytes(h->station_ip, station_ip);
    memset(p, 0, sizeof(h->sc.station_list));
    size_t o = 0;
    p[o++] = 5;
    p[o++] = 0; p[o++] = 0; p[o++] = 1;
    p[o++] = 0; p[o++] = 0; p[o++] = 3;
    memcpy(p + o, h->our_mac, 6); o += 8;
    bin_wb16(p + o, h->our_id); o += 2;
    p[o++] = 2;                                /* stations */
    p[o++] = 0; p[o++] = 0; p[o++] = 1;
    o += 6;
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
    /* Child record: address, then the tail of its join. */
    memcpy(p + o, h->station_mac, 6); o += 8;
    bin_wb16(p + o, h->station_id); o += 2;
    memcpy(p + o, station_ip, 4); o += 4;
    bin_wb16(p + o, PIA_PORT); o += 2;
    p[o++] = 1; p[o++] = 0; p[o++] = 1; p[o++] = 0;
    o += 33;
    p[o++] = 1; p[o++] = 1; p[o++] = 0; p[o++] = 0;
    memcpy(p + o, h->joiner_record, (size_t)h->joiner_record_len); o += (size_t)h->joiner_record_len;
    send_connected(h, 13, p, o, 1, true);
}

static void send_join_response(pia_host_t *h)
{
    uint8_t p[37] = {2, 0x0d, 7, 1};
    memcpy(p + 8, h->join_token, 4);
    memcpy(p + 12, h->our_mac, 6);
    bin_wb16(p + 20, h->our_id);
    memcpy(p + 22, h->station_mac, 6);
    bin_wb16(p + 30, h->station_id);
    p[32] = 1; p[34] = 1;
    send_connected(h, 13, p, sizeof(p), h->station_id, false);
}

static void send_disconnect_notice(pia_host_t *h)
{
    uint8_t p[30] = {9};
    memcpy(p + 1, h->our_mac, 6);
    bin_wb16(p + 9, h->our_id);
    ip_bytes(h->ours, p + 12);
    bin_wb16(p + 16, PIA_PORT);
    memcpy(p + 18, h->station_mac, 6);
    bin_wb16(p + 26, h->station_id);
    send_connected(h, 13, p, sizeof(p), h->station_id, false);
}

static void send_removed_broadcast(pia_host_t *h, bool last)
{
    uint8_t p[10] = {0x0d};
    memcpy(p + 1, h->our_mac, 6);
    p[9] = 2;
    send_session(h, 13, p, sizeof(p), h->our_id, last ? 0 : -1, false);
}

/* ---- protocol 3: round trips ----------------------------------------------- */

static void send_probe(pia_host_t *h)
{
    uint8_t p[21] = {0};
    uint64_t stamp = (uint64_t)h->now_ms * 19200;          /* the 19.2 MHz system counter */
    bin_wb64(p + 1, stamp);
    bin_wb32(p + 13, (uint32_t)((uint64_t)h->now_ms * 1000));
    bin_wb16(p + 19, h->our_id);
    int slot = 0;
    for (int i = 0; i < PIA_HOST_PROBES; ++i)
    {
        if (!h->probes[i].used) { slot = i; break; }
        if (h->probes[i].sent_ms < h->probes[slot].sent_ms) slot = i;
    }
    h->probes[slot].stamp = stamp;
    h->probes[slot].sent_ms = h->now_ms;
    h->probes[slot].used = true;
    send_connected(h, 3, p, sizeof(p), 1, false);
}

/* Echo the child's probe: bytes 1..8 kept, the rest rewritten. */
static void reply_probe(pia_host_t *h, const uint8_t *q)
{
    uint8_t p[21] = {1};
    memcpy(p + 1, q + 1, 8);
    bin_wb32(p + 13, (uint32_t)((uint64_t)h->now_ms * 1000));
    bin_wb16(p + 17, h->station_id);
    bin_wb16(p + 19, h->our_id);
    send_connected(h, 3, p, sizeof(p), 1, false);
}

static void match_probe(pia_host_t *h, const uint8_t *p)
{
    uint64_t stamp = bin_b64(p + 1);
    for (int i = 0; i < PIA_HOST_PROBES; ++i)
        if (h->probes[i].used && h->probes[i].stamp == stamp)
        {
            h->probes[i].used = false;
            pia_reliable_add_rtt(&h->reliable, (double)(h->now_ms - h->probes[i].sent_ms));
            return;
        }
}

/* ---- protocol 10: the reliable stream -------------------------------------- */

static bool queue_reliable(pia_host_t *h, const uint8_t *data, size_t len, pia_packet_t *out)
{
    uint8_t flags = h->stream_opened ? 7 : 15;     /* the first frame of a stream is marked */
    int seq = pia_reliable_queue(&h->reliable, data, len, flags, (double)h->now_ms);
    if (seq < 0) return false;
    h->stream_opened = true;
    const pia_pending_t *e = &h->reliable.pending[(uint16_t)seq % PIA_RELIABLE_SLOTS];
    out->seq = (uint16_t)seq;
    out->flags = flags;
    out->data = e->data;
    out->length = e->length;
    return true;
}

/* One datagram of reliable frames, plus any owed ack. */
static void send_frames(pia_host_t *h, pia_packet_t *frames, int count)
{
    pia_host_scratch_t *sc = &h->sc;
    uint8_t (*wrapped)[PIA_RELIABLE_PAYLOAD + 8] = sc->wrapped;
    uint8_t *ack = sc->ack;
    if (count < MAX_FRAMES && (h->ack_owed || pia_reliable_has_gap(&h->reliable)))
    {
        pia_reliable_ack(&h->reliable, ack);
        frames[count].seq = 0xfff0; frames[count].flags = 0; frames[count].data = ack; frames[count].length = 20;
        ++count;
        h->ack_owed = false;
    }
    if (count == 0) return;
    pia_message_t messages[MAX_FRAMES];
    for (int i = 0; i < count; ++i)
    {
        int size = pia_reliable_wrap(&h->reliable, &frames[i], wrapped[i], sizeof(wrapped[i]));
        if (size < 0) return;
        messages[i].protocol = 10;
        messages[i].payload = wrapped[i];
        messages[i].length = (uint16_t)size;
        messages[i].has_flags = frames[i].flags == 0;
        messages[i].flags = 0x40;
    }
    send_messages(h, messages, count, h->station_id, h->our_id, -1, false, false, true);
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

static void note_wt(pia_host_t *h, uint32_t stamp, uint16_t seq, bool idle)
{
    h->wt_ring[h->wt_ring_head].stamp = stamp;
    h->wt_ring[h->wt_ring_head].seq = seq;
    h->wt_ring[h->wt_ring_head].sent_ms = h->now_ms;
    h->wt_ring[h->wt_ring_head].used = true;
    h->wt_ring[h->wt_ring_head].idle = idle;
    h->wt_ring_head = (h->wt_ring_head + 1) % PIA_HOST_WT_RING;
}

/* A WK gives an RTT sample for a WT sent once. */
static void take_wk(pia_host_t *h, uint32_t stamp)
{
    ++h->wk_acks;
    for (int i = 0; i < PIA_HOST_WT_RING; ++i)
    {
        if (!h->wt_ring[i].used || h->wt_ring[i].stamp != stamp) continue;
        h->wt_ring[i].used = false;
        const pia_pending_t *e = &h->reliable.pending[h->wt_ring[i].seq % PIA_RELIABLE_SLOTS];
        if (e->used && e->seq == h->wt_ring[i].seq && e->resends == 0)
            pia_reliable_add_rtt(&h->reliable, (double)(h->now_ms - h->wt_ring[i].sent_ms));
        return;
    }
}

static void note_child_stamp(pia_host_t *h, uint32_t stamp)
{
    if (h->child_stamp_valid)
    {
        uint32_t elapsed = stamp - h->child_stamp;
        if (elapsed > 1 && elapsed < 300) h->child_skipped += (int)elapsed - 1;
    }
    h->child_stamp = stamp;
    h->child_stamp_valid = true;
}

static void deliver_child_wt(pia_host_t *h, const uint8_t *p, size_t len)
{
    if (len < 12) return;
    note_child_stamp(h, bin_u32(p + 4));
    /* Length at byte 8 from a Switch, byte 9 from our joiner. */
    size_t n = p[8] ? (size_t)(p[8] & 0x7f) : p[9];
    if (n > len - 12) n = len - 12;
    ++h->child_frames;
    if (n > 0 && h->deliver) h->deliver(p + 12, n, h->user);
}

/* One child stream frame, in order. */
static void feed(pia_host_t *h, const uint8_t *p, size_t len)
{
    if (len < 4 || p[0] != 0x57 || (size_t)bin_u16(p + 2) + 4 != len) return;   /* metadata: nothing to do */
    if (p[1] == 0x43 && len >= 6)
    {
        if (h->child_disconnected) return;
        memcpy(h->connect_id, p + 4, 2);
        if (!h->connect_requested) emit(h, "WC");
        h->connect_requested = true;
    }
    else if (p[1] == 0x54) deliver_child_wt(h, p, len);
    else if (p[1] == 0x4b && len >= 16) take_wk(h, bin_u32(p + 12));
}

/* Pia retransmits out of order, but the games need their +1 mod 8 sequence, so child
   frames are delivered in order. A frame with no room to wait is left unacked and resent. */
static void resequence(pia_host_t *h, uint16_t seq, uint16_t low, const uint8_t *inner, size_t len)
{
    if (h->next_deliver < 0) h->next_deliver = low;
    uint16_t next = (uint16_t)h->next_deliver;
    if (seq == next)
    {
        pia_reliable_receive(&h->reliable, seq, low);
        feed(h, inner, len);
        next = (uint16_t)(next + 1);
        for (bool again = true; again;)
        {
            again = false;
            for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
                if (h->hold[i].used && h->hold[i].seq == next)
                {
                    h->hold[i].used = false;
                    feed(h, h->hold[i].data, h->hold[i].length);
                    next = (uint16_t)(next + 1);
                    again = true;
                    break;
                }
        }
        h->next_deliver = next;
        return;
    }
    if (!bin_less(next, seq) || (uint16_t)(seq - next) >= 4096)
    {
        pia_reliable_receive(&h->reliable, seq, low);    /* a copy of a delivered frame */
        return;
    }
    if (len > PIA_HOST_HOLD_BYTES) { ++h->hold_dropped; return; }
    for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
        if (h->hold[i].used && h->hold[i].seq == seq) return;
    for (int i = 0; i < PIA_HOST_HOLD_SLOTS; ++i)
        if (!h->hold[i].used)
        {
            h->hold[i].used = true;
            h->hold[i].seq = seq;
            h->hold[i].length = (uint16_t)len;
            memcpy(h->hold[i].data, inner, len);
            pia_reliable_receive(&h->reliable, seq, low);
            return;
        }
    ++h->hold_dropped;
}

static void on_reliable(pia_host_t *h, const uint8_t *p, size_t len)
{
    if (len < 8 || bin_b16(p + 1) > len - 8 || p[7] != 0) { ++h->rx_bad_frame; return; }
    uint16_t inner_len = bin_b16(p + 1), seq = bin_b16(p + 3), low = bin_b16(p + 5);
    const uint8_t *inner = p + 8;
    if ((p[0] & 1) == 0)
    {
        pia_reliable_acknowledge(&h->reliable, inner, inner_len, (double)h->now_ms);
        return;
    }
    h->ack_owed = true;
    resequence(h, seq, low, inner, inner_len);
}

/* ---- inbound --------------------------------------------------------------- */

static void on_join(pia_host_t *h, const pia_message_t *m, uint16_t src)
{
    const uint8_t *p = m->payload;
    if (m->length < 84) { ++h->rx_bad_frame; return; }
    uint16_t id = bin_b16(p + 28);
    if (id == 0) id = src;
    if (id == 0 || id == h->our_id) return;
    if (h->state >= 2)
    {
        /* Join repeated: our reply was lost. One station only. */
        if (id != h->station_id) return;
        send_station_list(h);
        send_join_response(h);
        return;
    }
    h->station_id = id;
    static const uint8_t zero_mac[6];
    if (memcmp(p + 20, zero_mac, 6) != 0) memcpy(h->station_mac, p + 20, 6);
    int tail = m->length - 83;
    if (tail > PIA_HOST_RECORD_BYTES) tail = PIA_HOST_RECORD_BYTES;
    memcpy(h->joiner_record, p + 83, (size_t)tail);
    h->joiner_record_len = tail;
    esp_fill_random(h->join_token, sizeof(h->join_token));
    h->state = 2;
    h->joined_ms = h->now_ms;
    h->join_replies = 1;
    h->next_join_reply_ms = h->now_ms + JOIN_REPLY_REPEAT_MS;
    h->next_probe_ms = h->now_ms + FIRST_PROBE_MS;
    char line[40];
    snprintf(line, sizeof(line), "join from station id=%04x", h->station_id);
    emit(h, line);
    send_station_list(h);
    send_join_response(h);
}

static void dispatch(pia_host_t *h, const pia_message_t *m, uint16_t src)
{
    const uint8_t *p = m->payload;
    if (m->protocol == 1)
    {
        if (m->length < 8 || p[0] != 1) return;
        uint32_t seq = bin_b32(p + 4);
        if (p[1] == 0x12 && seq == h->net_seq) h->status_acked = true;
        else if (p[1] == 0x51 && seq == h->property_seq) h->property_acked = true;
    }
    else if (m->protocol == 13)
    {
        if (m->length < 1) return;
        if (p[0] == 0) on_join(h, m, src);
        else if (h->state >= 2) h->child_alive = true;
    }
    else if (m->protocol == 3)
    {
        if (h->state < 2 || m->length < 21) return;
        h->child_alive = true;
        if (p[0] == 0) reply_probe(h, p);
        else if (p[0] == 1) match_probe(h, p);
    }
    else if (m->protocol == 10)
    {
        if (h->state < 2) return;
        h->child_alive = true;
        if (h->state == 2) { h->state = 3; emit(h, "session up"); }
        on_reliable(h, p, m->length);
    }
}

void pia_host_receive(pia_host_t *h, const uint8_t *data, size_t length, const char *source)
{
    if (h->state == 0) return;
    if (strcmp(source, h->station_ip) != 0) { ++h->rx_wrong_source; return; }
    if (length < 29) { ++h->rx_bad_frame; return; }
    pia_host_scratch_t *sc = &h->sc;
    uint8_t *plain = sc->plain, *app = sc->app;
    int n = pia_decrypt(&h->crypto, data, length, source, plain, sizeof(sc->plain));
    if (n < 0) { ++h->decrypt_failures; return; }
    int padding = data[5] >> 4, footer = data[12];
    if (padding + footer > n) { ++h->rx_bad_frame; return; }
    n -= padding + footer;

    const uint8_t *body = plain;
    int size = n;
    if (data[5] & 1)
    {
        size = pia_decompress(plain, (size_t)n, app, sizeof(sc->app));
        if (size < 0) { ++h->rx_unzip_fail; return; }
        body = app;
    }
    uint16_t src = bin_b16(data + 8);
    pia_message_iter_t it;
    pia_message_t message;
    pia_message_iter_init(&it, body, (size_t)size);
    while (pia_message_next(&it, &message))
    {
        ++h->rx_messages;
        dispatch(h, &message, src);
    }
    ++h->received;
    if (h->credits < CREDIT_CAP) ++h->credits;
    h->last_child_datagram_ms = h->now_ms;
}

/* ---- the parent's frames --------------------------------------------------- */

void pia_host_parent_frame(pia_host_t *h, const uint8_t *payload, size_t length)
{
    if (length == 0 || length > PIA_HOST_PARENT_BYTES) return;
    bool plain = state_only(payload, length);
    if (plain)
    {
        memcpy(h->idle, payload, length);
        h->idle_len = (uint8_t)length;
        h->has_idle = true;
        if (h->parent_count > 0)
        {
            int at = (h->parent_head + h->parent_count - 1) % PIA_HOST_PARENT_SLOTS;
            if (state_only(h->parent[at].data, h->parent[at].length))
            {
                memcpy(h->parent[at].data, payload, length);
                h->parent[at].length = (uint8_t)length;
                return;
            }
        }
    }
    if (h->parent_count == PIA_HOST_PARENT_SLOTS)
    {
        ++h->parent_dropped;
        if (plain) return;
        int victim = 0;
        for (int i = 0; i < h->parent_count; ++i)
        {
            int at = (h->parent_head + i) % PIA_HOST_PARENT_SLOTS;
            if (state_only(h->parent[at].data, h->parent[at].length)) { victim = i; break; }
        }
        for (int i = victim; i < h->parent_count - 1; ++i)
            h->parent[(h->parent_head + i) % PIA_HOST_PARENT_SLOTS] = h->parent[(h->parent_head + i + 1) % PIA_HOST_PARENT_SLOTS];
        --h->parent_count;
    }
    /* Superseded key reports shed while frames wait: each one waiting is lag on both
       screens. */
    for (int i = 0; h->sheddable && h->parent_count > PIA_HOST_SHED_AT && i < h->parent_count - 1;)
    {
        const int at = (h->parent_head + i) % PIA_HOST_PARENT_SLOTS;
        if (!h->sheddable(h->parent[at].data, h->parent[at].length)) { ++i; continue; }
        for (int j = i; j < h->parent_count - 1; ++j)
            h->parent[(h->parent_head + j) % PIA_HOST_PARENT_SLOTS] = h->parent[(h->parent_head + j + 1) % PIA_HOST_PARENT_SLOTS];
        --h->parent_count;
        ++h->shed;
    }
    int at = (h->parent_head + h->parent_count) % PIA_HOST_PARENT_SLOTS;
    memcpy(h->parent[at].data, payload, length);
    h->parent[at].length = (uint8_t)length;
    ++h->parent_count;
}

/* ---- tick ------------------------------------------------------------------ */

static void stream_tick(pia_host_t *h)
{
    pia_packet_t frames[MAX_FRAMES];
    int count = pia_reliable_retransmit(&h->reliable, (double)h->now_ms, 2, frames, MAX_FRAMES);
    h->resends += count;
    if (h->child_accepted && !h->child_disconnected)
    {
        const bool paced = h->now_ms - h->last_child_datagram_ms <= CHILD_QUIET_MS;
        const bool room = pia_reliable_pending(&h->reliable) < INFLIGHT_MAX && (!paced || h->credits > 0);
        if (room && h->wg_pending >= 0 && count < MAX_FRAMES)
        {
            uint8_t wg[8] = {0x57, 0x47, 4, 0};
            bin_w32(wg + 4, (uint32_t)h->wg_pending);
            if (queue_reliable(h, wg, sizeof(wg), &frames[count])) { ++count; h->wg_pending = -1; }
        }
        uint8_t *wt = h->sc.wt;
        static const uint8_t idle_byte[1];
        int n = -1;
        bool queued = h->parent_count > 0;
        if (queued && room)
        {
            const uint8_t *data = h->parent[h->parent_head].data;
            size_t len = h->parent[h->parent_head].length;
            /* WG 1 ends the join phase, before the first UNI frame. */
            bool uni = is_uni(data, len);
            if (uni && !h->uni_sent && count < MAX_FRAMES)
            {
                uint8_t wg[8] = {0x57, 0x47, 4, 0, 1, 0, 0, 0};
                if (queue_reliable(h, wg, sizeof(wg), &frames[count])) { ++count; h->uni_sent = true; }
            }
            if (!uni || h->uni_sent) n = wrap_wt(data, len, h->timestamp, wt, PIA_RELIABLE_PAYLOAD);
        }
        else if (!queued && room && h->tick - h->last_wt_tick >= IDLE_TICKS)
            n = h->has_idle ? wrap_wt(h->idle, h->idle_len, h->timestamp, wt, PIA_RELIABLE_PAYLOAD)
                            : wrap_wt(idle_byte, 1, h->timestamp, wt, PIA_RELIABLE_PAYLOAD);
        if (n > 0 && count < MAX_FRAMES && queue_reliable(h, wt, (size_t)n, &frames[count]))
        {
            note_wt(h, h->timestamp, frames[count].seq, !queued);
            if (paced) --h->credits;
            ++count;
            h->last_wt_tick = h->tick;
            if (queued)
            {
                h->parent_head = (h->parent_head + 1) % PIA_HOST_PARENT_SLOTS;
                --h->parent_count;
                ++h->parent_frames;
            }
            if (!h->wt_sent) { h->wt_sent = true; h->wg_pending = 0; }
        }
        else if (queued && h->parent_count > 1 && state_only(h->parent[h->parent_head].data, h->parent[h->parent_head].length))
        {
            /* Stream full: a state-only frame gives way to the next. */
            h->parent_head = (h->parent_head + 1) % PIA_HOST_PARENT_SLOTS;
            --h->parent_count;
        }
    }
    send_frames(h, frames, count);
}

static void clear_station(pia_host_t *h)
{
    pia_reliable_init(&h->reliable);
    memset(h->hold, 0, sizeof(h->hold));
    memset(h->wt_ring, 0, sizeof(h->wt_ring));
    memset(h->probes, 0, sizeof(h->probes));
    memset(h->packet_ids, 0, sizeof(h->packet_ids));
    h->state = 0;
    h->station_id = 0;
    h->child_stamp_valid = false;
    h->credits = 0;
    h->last_child_datagram_ms = h->now_ms - CHILD_QUIET_MS - 1;
    h->connect_requested = h->child_accepted = h->child_disconnected = false;
    h->parent_head = h->parent_count = 0;
    h->has_idle = false;
    h->status_acked = h->property_acked = h->property_pending = false;
    h->property_seq = 0;
    h->child_alive = h->stream_opened = h->ack_owed = h->wt_sent = h->uni_sent = false;
    h->join_replies = 0;
    h->wg_pending = -1;
    h->next_deliver = -1;
    h->wt_ring_head = 0;
    h->teardown = h->teardown_sent = 0;
}

static void start_teardown(pia_host_t *h)
{
    h->teardown = 1;
    h->teardown_sent = 0;
    h->next_teardown_ms = h->now_ms + NOTICE_PERIOD_MS;
}

/* Teardown: three disconnect notices, five removal broadcasts, then the status with the
   slot marked and then cleared. */
static void teardown_tick(pia_host_t *h)
{
    if (h->now_ms < h->next_teardown_ms) return;
    int64_t period = BROADCAST_PERIOD_MS;
    switch (h->teardown)
    {
    case 1:
        send_disconnect_notice(h);
        period = NOTICE_PERIOD_MS;
        if (++h->teardown_sent >= NOTICE_COPIES) { h->teardown = 2; h->teardown_sent = 0; period = BROADCAST_PERIOD_MS; }
        break;
    case 2:
        send_removed_broadcast(h, h->teardown_sent + 1 == BROADCAST_COPIES);
        if (++h->teardown_sent >= BROADCAST_COPIES)
        {
            h->teardown = 3; h->teardown_sent = 0;
            ++h->net_seq;
            h->status_acked = false;
        }
        break;
    case 3:
        if (h->status_acked || h->teardown_sent >= BROADCAST_COPIES)
        {
            h->teardown = 4; h->teardown_sent = 0;
            ++h->net_seq;
            h->status_acked = false;
        }
        else { send_status(h, STATUS_MARKED); ++h->teardown_sent; break; }
        /* fall through */
    case 4:
        if (h->status_acked || h->teardown_sent >= BROADCAST_COPIES) { clear_station(h); return; }
        send_status(h, STATUS_CLEARED);
        ++h->teardown_sent;
        break;
    default:
        return;
    }
    h->next_teardown_ms = h->now_ms + period;
}

void pia_host_tick(pia_host_t *h, int64_t now_ms)
{
    h->now_ms = now_ms;
    ++h->tick;
    ++h->timestamp;
    if (h->state == 0) return;
    if (h->teardown) teardown_tick(h);
    if (h->state == 0) return;

    /* Repeated until acknowledged and until the join: a child whose join was lost resends
       it on the next copy. */
    if (h->teardown < 3 && (!h->status_acked || h->state < 2) && now_ms >= h->next_net_ms)
    {
        send_status(h, STATUS_NORMAL);
        h->next_net_ms = now_ms + STATUS_REPEAT_MS;
    }
    if (h->state < 2) return;

    if (!h->child_alive && h->join_replies < JOIN_REPLIES && now_ms >= h->next_join_reply_ms)
    {
        send_station_list(h);
        send_join_response(h);
        ++h->join_replies;
        h->next_join_reply_ms = now_ms + JOIN_REPLY_REPEAT_MS;
    }
    if (now_ms >= h->next_probe_ms)
    {
        send_probe(h);
        h->next_probe_ms = now_ms + PROBE_PERIOD_MS;
    }
    if (h->child_accepted && h->app_data_len > 0)
    {
        if (h->property_pending)
        {
            ++h->property_seq;
            h->property_pending = h->property_acked = false;
            h->next_keepalive_ms = now_ms;
        }
        if (!h->property_acked && now_ms >= h->next_keepalive_ms)
        {
            send_property(h);
            h->next_keepalive_ms = now_ms + PROPERTY_REPEAT_MS;
        }
    }
    stream_tick(h);
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
    clear_station(h);
}

void pia_host_set_app_data(pia_host_t *h, const uint8_t *data, int length)
{
    if (length <= 0 || length > PIA_HOST_APP_DATA) return;
    if (length == h->app_data_len && memcmp(h->app_data, data, (size_t)length) == 0) return;
    memcpy(h->app_data, data, (size_t)length);
    h->app_data_len = length;
    h->property_pending = true;
}

void pia_host_station_joined(pia_host_t *h, const uint8_t mac[6], const char *ip, uint8_t index)
{
    clear_station(h);
    memcpy(h->station_mac, mac, 6);
    snprintf(h->station_ip, sizeof(h->station_ip), "%s", ip);
    h->station_index = index >= 1 && index <= 5 ? index : 1;
    ++h->net_seq;
    h->property_pending = h->app_data_len > 0;
    h->state = 1;
    send_status(h, STATUS_NORMAL);
    h->next_net_ms = h->now_ms + STATUS_REPEAT_MS;
    emit(h, "status sent");
}

void pia_host_station_left(pia_host_t *h)
{
    if (h->state == 0) return;
    /* Membership changes the teardown had not announced yet. */
    h->net_seq += h->teardown < 3 ? 2 : h->teardown == 3 ? 1 : 0;
    clear_station(h);
    emit(h, "child left");
}

bool pia_host_connect_pending(const pia_host_t *h)
{
    return h->connect_requested && !h->child_accepted && !h->child_disconnected;
}

static void send_wd(pia_host_t *h)
{
    uint8_t wd[6] = {0x57, 0x44, 2, 0, h->connect_id[0], h->connect_id[1]};
    pia_packet_t frames[MAX_FRAMES];
    if (!queue_reliable(h, wd, sizeof(wd), &frames[0])) return;
    emit(h, "WD sent");
    send_frames(h, frames, 1);
}

void pia_host_accept(pia_host_t *h, bool accepted)
{
    if (!pia_host_connect_pending(h)) return;
    if (!accepted)
    {
        h->child_disconnected = true;
        start_teardown(h);
        send_wd(h);
        return;
    }
    uint8_t wa[10] = {0x57, 0x41, 6, 0};
    bin_w16(wa + 4, h->adapter_id);
    memcpy(wa + 6, h->connect_id, 2);
    pia_packet_t frames[MAX_FRAMES];
    if (!queue_reliable(h, wa, sizeof(wa), &frames[0])) return;
    h->adapter_id += 3;
    h->child_accepted = true;
    h->last_wt_tick = h->tick;
    emit(h, "WA sent");
    send_frames(h, frames, 1);
}

void pia_host_disconnect_child(pia_host_t *h)
{
    if (h->state < 2 || h->child_disconnected) return;
    h->child_disconnected = true;
    start_teardown(h);
    if (h->connect_requested) send_wd(h);
}

bool pia_host_session_up(const pia_host_t *h) { return h->state == 3; }
