#include "pia_link.h"

#include <stdio.h>
#include "esp_random.h"
#include "esp_timer.h"

static const uint8_t kMetadata[46] = {
    0x4a, 0x00, 0x2a, 0x00, 0x58, 0x01, 0x00, 0x4c, 0x65, 0x61, 0x66, 0x47,
    0x72, 0x65, 0x65, 0x6e, 0x5f, 0x65};

void pia_link_init(pia_link_t *l, const uint8_t ssid[16], const uint8_t our_mac[6],
                   const uint8_t host_mac[6], const char *our_ip, const char *host_ip,
                   pia_send_fn send, pia_gba_fn deliver, pia_log_fn log, void *user)
{
    memset(l, 0, sizeof(*l));
    pia_crypto_init(&l->crypto, ssid);
    pia_conn_init(&l->conn, our_mac, host_mac, our_ip);
    pia_reliable_init(&l->reliable);
    snprintf(l->ours, sizeof(l->ours), "%s", our_ip);
    snprintf(l->host, sizeof(l->host), "%s", host_ip);
    l->send = send; l->deliver = deliver; l->log = log; l->user = user;
    l->next_deliver = -1;
    l->last_ack = -100;
    l->timestamp = 0x362e;   /* base of the frame counter stamped on WT frames */
    l->connect_wanted = true;
    uint8_t seed[8];
    esp_fill_random(seed, sizeof(seed));
    l->nonce = bin_b64(seed) | 1;
    esp_fill_random(l->connect_id, sizeof(l->connect_id));
    l->connect_id[0] |= 1;
}

void pia_link_set_connect(pia_link_t *l, bool wanted) { l->connect_wanted = wanted; }

static void emit(pia_link_t *l, const char *message) { if (l->log) l->log(message, l->user); }

static int64_t link_now_ms(void) { return esp_timer_get_time() / 1000; }

static void note_peer_low(pia_link_t *l, uint16_t low)
{
    if (l->peer_low_valid && low == l->peer_low) return;
    l->peer_low = low;
    l->peer_low_moved_ms = link_now_ms();
    l->peer_low_valid = true;
}

static void note_peer_ack(pia_link_t *l, uint16_t next)
{
    int64_t now = link_now_ms();
    l->peer_ack_seen_ms = now;
    if (l->peer_ack_valid && next == l->peer_ack_next) return;
    l->peer_ack_next = next;
    l->peer_ack_moved_ms = now;
    l->peer_ack_valid = true;
}

/* ---- duplicate suppression ------------------------------------------------ */

static bool seen_add(pia_link_t *l, uint16_t seq)
{
    int bit = seq & (PIA_SEEN_BITS - 1);
    if (l->seen_bits[bit >> 3] & (1 << (bit & 7))) return false;
    l->seen_bits[bit >> 3] |= (uint8_t)(1 << (bit & 7));
    if (l->seen_count == PIA_SEEN_RING)
    {
        int old = l->seen_ring[l->seen_head] & (PIA_SEEN_BITS - 1);
        l->seen_bits[old >> 3] &= (uint8_t)~(1 << (old & 7));
        l->seen_head = (l->seen_head + 1) % PIA_SEEN_RING;
        --l->seen_count;
    }
    l->seen_ring[(l->seen_head + l->seen_count) % PIA_SEEN_RING] = seq;
    ++l->seen_count;
    return true;
}

static bool time_add(pia_link_t *l, uint32_t time)
{
    for (int i = 0; i < l->time_count; ++i)
        if (l->time_ring[(l->time_head + i) % PIA_TIME_RING] == time) return false;
    if (l->time_count == PIA_TIME_RING) { l->time_head = (l->time_head + 1) % PIA_TIME_RING; --l->time_count; }
    l->time_ring[(l->time_head + l->time_count) % PIA_TIME_RING] = time;
    ++l->time_count;
    return true;
}

/* ---- outbound child queue -------------------------------------------------- */

static bool is_idle(const uint8_t *p, size_t len)
{
    for (size_t i = 2; i < len; ++i) if (p[i]) return false;
    return true;
}

void pia_link_enqueue(pia_link_t *l, const uint8_t *payload, size_t length)
{
    if (length > PIA_OUT_BYTES) return;
    if (is_idle(payload, length)) { memcpy(l->idle, payload, length); l->idle_len = (uint16_t)length; l->has_idle = true; }
    /* Drop only an exact repeat of the last queued frame (an RFU-level retransmit).
       Any difference, including the mod-8 sequence tag, is a distinct frame. */
    if (l->last_enqueued_len == length && memcmp(l->last_enqueued, payload, length) == 0) return;
    if (l->out_count >= PIA_OUT_SLOTS)
    {
        /* Full: drop the incoming frame if idle, else the newest queued idle one. */
        if (is_idle(payload, length)) { ++l->idle_evicted; return; }
        int victim = -1;
        for (int i = l->out_count - 1; i >= 0 && victim < 0; --i)
        {
            int at = (l->out_head + i) % PIA_OUT_SLOTS;
            if (is_idle(l->outbound[at].data, l->outbound[at].length)) victim = i;
        }
        if (victim < 0) { ++l->overflow; return; }
        for (int i = victim; i < l->out_count - 1; ++i)
            l->outbound[(l->out_head + i) % PIA_OUT_SLOTS] = l->outbound[(l->out_head + i + 1) % PIA_OUT_SLOTS];
        --l->out_count;
        ++l->idle_evicted;
    }
    int slot = (l->out_head + l->out_count) % PIA_OUT_SLOTS;
    memcpy(l->outbound[slot].data, payload, length);
    l->outbound[slot].length = (uint16_t)length;
    ++l->out_count;
    /* The GBA sends a held-keys report every frame; the Switch takes about one child frame
       per datagram, so reports back up while walking. The avatar moves from the parent's
       echo, and shedding happens before the sequence stamp, so both sides stay in step. */
    for (int i = 0; l->sheddable && l->out_count > PIA_OUT_SHED_AT && i < l->out_count - 1;)
    {
        const int at = (l->out_head + i) % PIA_OUT_SLOTS;
        if (!l->sheddable(l->outbound[at].data, l->outbound[at].length)) { ++i; continue; }
        for (int j = i; j < l->out_count - 1; ++j)
            l->outbound[(l->out_head + j) % PIA_OUT_SLOTS] = l->outbound[(l->out_head + j + 1) % PIA_OUT_SLOTS];
        --l->out_count;
        ++l->shed;
    }
    if (l->out_count > l->high_water) l->high_water = l->out_count;
    memcpy(l->last_enqueued, payload, length);
    l->last_enqueued_len = (uint16_t)length;
}

static int wrap_wt(const uint8_t *slot, size_t len, uint32_t time, uint8_t *out, size_t cap)
{
    size_t total = 12 + ((len + 3) & ~(size_t)3);
    if (total > cap) return -1;
    memset(out, 0, total);
    out[0] = 0x57; out[1] = 0x54;
    bin_w16(out + 2, (uint16_t)(total - 4));
    bin_w32(out + 4, time);
    out[9] = (uint8_t)len;
    memcpy(out + 12, slot, len);
    return (int)total;
}

static int wrap_ack(uint32_t sequence, uint32_t middle, uint32_t time, uint8_t out[16])
{
    memset(out, 0, 16);
    out[0] = 0x57; out[1] = 0x4b; out[2] = 12;
    bin_w32(out + 4, sequence);
    bin_w32(out + 8, middle);
    bin_w32(out + 12, time);
    return 16;
}

/* Next child frame to send, or the idle frame when the queue is empty. Commands are never
   repeated: the games discard a frame only when every slot's command word is zero, so a
   repeated command keeps the peer's receive queue non-empty and deadlocks standby. */
static int next_outbound(pia_link_t *l, uint8_t *out, size_t cap)
{
    if (l->out_count > 0)
    {
        int slot = l->out_head;
        l->out_head = (l->out_head + 1) % PIA_OUT_SLOTS;
        --l->out_count;
        if (l->stamp) l->stamp(l->outbound[slot].data, l->outbound[slot].length);
        return wrap_wt(l->outbound[slot].data, l->outbound[slot].length, l->timestamp + (uint32_t)l->tick, out, cap);
    }
    if (!l->has_idle) return -1;
    ++l->repeated;
    return wrap_wt(l->idle, l->idle_len, l->timestamp + (uint32_t)l->tick, out, cap);
}

/* ---- datagram assembly ----------------------------------------------------- */

static void send_messages(pia_link_t *l, const pia_message_t *messages, int count, uint16_t dst,
                          uint16_t src, bool compress, bool footer, bool establishing,
                          int packet, int footer_id)
{
    if (count == 0) return;
    static uint8_t body[PIA_MAX_BODY], staged[PIA_MAX_BODY], datagram[PIA_MAX_DATAGRAM];
    size_t length = 0;
    for (int i = 0; i < count; ++i)
    {
        int n = pia_message_encode(&messages[i], body + length, sizeof(body) - length);
        if (n < 0) return;
        length += (size_t)n;
    }

    bool zipped = compress || length >= 62;
    if (zipped)
    {
        int n = pia_compress_raw(body, length, staged, sizeof(staged));
        if (n < 0) return;
        memcpy(body, staged, (size_t)n);
        length = (size_t)n;
    }
    if (footer)
    {
        if (length + 2 > sizeof(body)) return;
        bin_wb16(body + length, (uint16_t)(footer_id >= 0 ? footer_id : dst));
        length += 2;
    }
    size_t padding = (16 - (length & 15)) & 15;
    if (length + padding > sizeof(body)) return;
    memset(body + length, 255, padding);
    length += padding;

    int slot = dst & 3;
    int pid = packet >= 0 ? packet : (l->packet_ids[slot] ? l->packet_ids[slot] : 1);
    if (packet < 0) l->packet_ids[slot] = (uint16_t)(pid == 65535 ? 1 : pid + 1);

    uint8_t flags = (uint8_t)((padding << 4) | (zipped ? 1 : 0) | (establishing ? 2 : 0));
    int n = pia_encrypt(&l->crypto, body, length, l->ours, dst, src, (uint16_t)pid, l->nonce++,
                        flags, footer ? 2 : 0, datagram, sizeof(datagram));
    if (l->nonce == 0) l->nonce = 1;
    if (n < 0) return;
    l->send(datagram, (size_t)n, l->host, l->user);
    ++l->sent;
}

static void batch(pia_link_t *l, const pia_packet_t *frames, int count)
{
    static uint8_t wrapped[9][PIA_RELIABLE_PAYLOAD + 8];
    pia_message_t messages[9];
    for (int start = 0; start < count; start += 9)
    {
        int n = count - start < 9 ? count - start : 9;
        for (int i = 0; i < n; ++i)
        {
            int size = pia_reliable_wrap(&l->reliable, &frames[start + i], wrapped[i], sizeof(wrapped[i]));
            if (size < 0) return;
            messages[i].protocol = 10;
            messages[i].payload = wrapped[i];
            messages[i].length = (uint16_t)size;
            messages[i].has_flags = frames[start + i].flags == 0;
            messages[i].flags = 0x40;
        }
        send_messages(l, messages, n, l->conn.host_id, l->conn.our_id, false, true, false, -1, -1);
    }
}

/* ---- inbound --------------------------------------------------------------- */

static void feed_gba(pia_link_t *l, const uint8_t *p, size_t len)
{
    if (len < 4 || (size_t)(bin_u16(p + 2) + 4) != len) return;
    if (p[1] == 0x41) { l->accepted = true; emit(l, "Host accepted RFU connection"); return; }
    if (p[1] == 0x44) { l->host_disconnected = true; return; }
    if (p[1] != 0x54 || len < 9) return;

    uint32_t time = bin_u32(p + 4);
    if (time_add(l, time) && l->k_count < PIA_K_QUEUE)
    {
        int slot = (l->k_head + l->k_count) % PIA_K_QUEUE;
        l->k_queue[slot].sequence = ++l->k_sequence;
        l->k_queue[slot].time = time;
        ++l->k_count;
    }
    l->credits = l->credits < 2 ? l->credits + 1 : 2;
    if (l->deliver) l->deliver(p, len, l->user);
}

/* Deliver in order. Pia retransmits arrive late, and the games check a +1 mod-8
   sequence; one reordered pair desyncs them permanently. */
static void resequence(pia_link_t *l, uint16_t seq, const uint8_t *inner, size_t len)
{
    if (l->next_deliver < 0) l->next_deliver = seq;
    if (seq != (uint16_t)l->next_deliver)
    {
        if (bin_less((uint16_t)l->next_deliver, seq) && (uint16_t)(seq - l->next_deliver) < 4096 &&
            len <= PIA_HOLD_BYTES)
        {
            bool held = false;
            for (int i = 0; i < PIA_HOLD_SLOTS && !held; ++i)
                if (!l->hold[i].used)
                {
                    l->hold[i].used = true;
                    l->hold[i].seq = seq;
                    l->hold[i].length = (uint16_t)len;
                    memcpy(l->hold[i].data, inner, len);
                    ++l->reordered;
                    held = true;
                }
            if (!held) ++l->hold_dropped;   /* lost to the GBA */
        }
        return;
    }
    feed_gba(l, inner, len);
    l->next_deliver = (uint16_t)(l->next_deliver + 1);
    for (bool again = true; again;)
    {
        again = false;
        for (int i = 0; i < PIA_HOLD_SLOTS; ++i)
            if (l->hold[i].used && l->hold[i].seq == (uint16_t)l->next_deliver)
            {
                l->hold[i].used = false;
                feed_gba(l, l->hold[i].data, l->hold[i].length);
                l->next_deliver = (uint16_t)(l->next_deliver + 1);
                again = true;
                break;
            }
    }
}

void pia_link_receive(pia_link_t *l, const uint8_t *data, size_t length, const char *source)
{
    ++l->rx_seen;
    if (strcmp(source, l->host) != 0) { ++l->rx_wrong_source; return; }
    if (length < 29) { ++l->rx_short; return; }
    static uint8_t plain[PIA_MAX_BODY], app[PIA_MAX_INFLATE];
    int n = pia_decrypt(&l->crypto, data, length, source, plain, sizeof(plain));
    if (n < 0) { ++l->decrypt_failures; return; }

    if (l->conn.host_id == 0 && bin_b16(data + 8) != 0) l->conn.host_id = bin_b16(data + 8);
    int padding = data[5] >> 4, footer = data[12];
    if (padding + footer > n) return;
    n -= padding + footer;

    int size = n;
    const uint8_t *body = plain;
    if (data[5] & 1)
    {
        size = pia_decompress(plain, (size_t)n, app, sizeof(app));
        if (size < 0)
        {
            /* Datagram lost; the Switch resends it in a larger batch. */
            ++l->rx_unzip_fail;
            l->rx_unzip_last_error = -size;
            l->rx_unzip_last_len = (int)length;
            return;
        }
        body = app;
    }
    if (size > l->rx_body_max) l->rx_body_max = size;
    if (l->rx_first_len == 0 && size > 0)
    {
        l->rx_first_len = size < 16 ? size : 16;
        memcpy(l->rx_first, body, (size_t)l->rx_first_len);
        l->rx_first_zipped = data[5] & 1;
        l->rx_first_pad = padding;
        l->rx_first_footer = footer;
    }

    /* Process every message. A skipped frame is never acked and the Switch keeps
       resending it in ever larger batches. */
    pia_message_iter_t it;
    pia_message_iter_init(&it, body, (size_t)size);
    pia_message_t message;
    int count = 0;
    while (pia_message_next(&it, &message))
    {
        ++l->rx_messages;
        if (++count > l->rx_msgs_max) l->rx_msgs_max = count;
        if (message.protocol != 10) { pia_conn_feed(&l->conn, &message, l->tick); continue; }
        const uint8_t *p = message.payload;
        if (message.length < 8 || bin_b16(p + 1) > message.length - 8 || p[7] != 0)
        { ++l->rx_bad_frame; return; }
        uint16_t seq = bin_b16(p + 3);
        const uint8_t *inner = p + 8;
        uint16_t inner_len = bin_b16(p + 1);
        if ((p[0] & 1) == 0)
        {
            if (inner_len >= 4) note_peer_ack(l, bin_b16(inner + 2));
            pia_reliable_acknowledge(&l->reliable, inner, inner_len, l->tick * (1000.0 / 59.727));
            continue;
        }
        note_peer_low(l, bin_b16(p + 5));
        l->ack_owed = true;
        if (seen_add(l, seq)) resequence(l, seq, inner, inner_len);
        pia_reliable_receive(&l->reliable, seq, bin_b16(p + 5));
    }
    ++l->received;
}

/* ---- tick ------------------------------------------------------------------ */

void pia_link_tick(pia_link_t *l)
{
    ++l->tick;
    double rtt;
    while (pia_conn_take_rtt(&l->conn, &rtt)) pia_reliable_add_rtt(&l->reliable, rtt);
    pia_conn_tick(&l->conn, l->tick);

    for (int i = 0; i < l->conn.outbox_count; ++i)
    {
        pia_outbox_t *o = &l->conn.outbox[i];
        pia_message_t m = {.protocol = o->protocol, .payload = o->payload, .length = o->length, .has_flags = false};
        send_messages(l, &m, 1, o->dst, o->src, o->compress, o->footer, o->establishing, o->packet, o->footer_id);
    }
    l->conn.outbox_count = 0;
    if (!pia_conn_connected(&l->conn)) return;

    double now = l->tick * (1000.0 / 59.727);
    if (!l->opened)
    {
        int seq = pia_reliable_queue(&l->reliable, kMetadata, sizeof(kMetadata), 15, now);
        if (seq >= 0)
        {
            pia_packet_t p = {.seq = (uint16_t)seq, .flags = 15, .data = kMetadata, .length = sizeof(kMetadata)};
            batch(l, &p, 1);
            l->opened = true;
        }
        return;
    }

    pia_packet_t frames[16];
    int count = 0;

    if (!l->connect_sent)
    {
        if (l->connect_wanted)
        {
            uint8_t connect[6] = {0x57, 0x43, 0x02, 0x00, l->connect_id[0], l->connect_id[1]};
            int seq = pia_reliable_queue(&l->reliable, connect, sizeof(connect), 7, now);
            if (seq >= 0)
            {
                pia_packet_t p = {.seq = (uint16_t)seq, .flags = 7, .data = connect, .length = sizeof(connect)};
                batch(l, &p, 1);
                l->connect_sent = true;
            }
            return;
        }
        /* Before WC the host still expects its stream acked. */
        count = pia_reliable_retransmit(&l->reliable, now, 2, frames, 16);
        uint8_t ack[20];
        if (l->tick - l->last_ack >= 2 && (l->ack_owed || pia_reliable_has_gap(&l->reliable)))
        {
            pia_reliable_ack(&l->reliable, ack);
            frames[count].seq = 0xfff0; frames[count].flags = 0; frames[count].data = ack; frames[count].length = 20;
            ++count;
            l->ack_owed = false;
            l->last_ack = l->tick;
        }
        batch(l, frames, count);
        return;
    }

    count = pia_reliable_retransmit(&l->reliable, now, l->accepted ? 1 : 2, frames, 16);

    for (int i = 0; i < l->k_inflight_count;)
    {
        bool still = false;
        for (uint16_t s = l->reliable.low; s != l->reliable.next; ++s)
            if (s == l->k_inflight[i]) { still = true; break; }
        if (still) ++i;
        else l->k_inflight[i] = l->k_inflight[--l->k_inflight_count];
    }

    static uint8_t k_frames[3][16];
    int middle = 0;
    while (l->k_count > 0 && pia_reliable_pending(&l->reliable) < 6 && middle < 3 &&
           l->k_inflight_count < 3 && count < 16)
    {
        int slot = l->k_head;
        l->k_head = (l->k_head + 1) % PIA_K_QUEUE;
        --l->k_count;
        ++middle;
        uint8_t *frame = k_frames[middle - 1];
        wrap_ack(l->k_queue[slot].sequence, (uint32_t)middle, l->k_queue[slot].time, frame);
        int seq = pia_reliable_queue(&l->reliable, frame, 16, 7, now);
        if (seq < 0) break;
        l->k_inflight[l->k_inflight_count++] = (uint16_t)seq;
        frames[count].seq = (uint16_t)seq; frames[count].flags = 7;
        frames[count].data = frame; frames[count].length = 16;
        ++count;
    }

    static uint8_t payload[PIA_RELIABLE_PAYLOAD];
    if (l->accepted && pia_reliable_pending(&l->reliable) < 6 && l->credits > 0 && count < 16)
    {
        int n = next_outbound(l, payload, sizeof(payload));
        if (n > 0)
        {
            int seq = pia_reliable_queue(&l->reliable, payload, (size_t)n, 7, now);
            if (seq >= 0)
            {
                --l->credits;
                frames[count].seq = (uint16_t)seq; frames[count].flags = 7;
                frames[count].data = payload; frames[count].length = (uint16_t)n;
                ++count;
            }
        }
    }

    uint8_t ack[20];
    if (l->tick - l->last_ack >= 2 && (l->ack_owed || pia_reliable_has_gap(&l->reliable)) && count < 16)
    {
        pia_reliable_ack(&l->reliable, ack);
        frames[count].seq = 0xfff0; frames[count].flags = 0; frames[count].data = ack; frames[count].length = 20;
        ++count;
        l->ack_owed = false;
        l->last_ack = l->tick;
    }
    batch(l, frames, count);
}
