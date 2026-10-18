#include "pia_conn.h"

#include <stdio.h>

#include "esp_random.h"

/* Random station id, nonzero and not `taken`. */
static uint16_t station_id(uint16_t taken)
{
    uint16_t id;
    do id = (uint16_t)esp_random(); while (id == 0 || id == taken);
    return id;
}

static pia_outbox_t *outbox_add(pia_conn_t *c, uint8_t protocol, const uint8_t *payload, size_t len,
                                uint16_t dst, uint16_t src)
{
    if (c->outbox_count >= PIA_OUTBOX_SLOTS || len > PIA_OUTBOX_PAYLOAD) return NULL;
    pia_outbox_t *o = &c->outbox[c->outbox_count++];
    memset(o, 0, sizeof(*o));
    memcpy(o->payload, payload, len);
    o->length = (uint16_t)len;
    o->protocol = protocol;
    o->dst = dst;
    o->src = src;
    o->footer = true;
    o->packet = -1;
    o->footer_id = -1;
    return o;
}

void pia_conn_init(pia_conn_t *c, const uint8_t our_mac[6], const uint8_t host_mac[6], const char *our_ip)
{
    memset(c, 0, sizeof(*c));
    c->our_id = station_id(0);
    c->last_rtt_tick = -100;
    c->system_time = 0x10000;
    memcpy(c->our_mac, our_mac, 6);
    memcpy(c->remote_mac, host_mac, 6);
    esp_fill_random(c->random, sizeof(c->random));
    snprintf(c->our_ip, sizeof(c->our_ip), "%s", our_ip);
}

bool pia_conn_connected(const pia_conn_t *c) { return c->state == 2; }

static const uint8_t kJoinPrefix[16] = {
    0x00, 0x06, 0x01, 0x00, 0x03, 0x05, 0x05, 0x01, 0x0a, 0x03, 0x0d, 0x07, 0x0f, 0x00, 0x00, 0x58};
static const uint8_t kJoinTail[24] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x01, 0x45, 0x4d, 0x55};

static int build_join(const pia_conn_t *c, uint8_t *out, size_t cap)
{
    unsigned a, b, d, e;
    if (sscanf(c->our_ip, "%u.%u.%u.%u", &a, &b, &d, &e) != 4) return -1;
    size_t need = 16 + 4 + 6 + 2 + 2 + 34 + 6 + 2 + 2 + 3 + 4 + 2 + sizeof(kJoinTail);
    if (need > cap) return -1;
    size_t o = 0;
    memcpy(out + o, kJoinPrefix, 16); o += 16;
    memcpy(out + o, c->random, 4); o += 4;
    memcpy(out + o, c->our_mac, 6); o += 6;
    memset(out + o, 0, 2); o += 2;
    bin_wb16(out + o, c->our_id); o += 2;
    memset(out + o, 0, 34); o += 34;
    memcpy(out + o, c->remote_mac, 6); o += 6;
    memset(out + o, 0, 2); o += 2;
    bin_wb16(out + o, c->host_id); o += 2;
    out[o++] = 1; out[o++] = 1; out[o++] = 0;
    out[o++] = (uint8_t)a; out[o++] = (uint8_t)b; out[o++] = (uint8_t)d; out[o++] = (uint8_t)e;
    out[o++] = 0x30; out[o++] = 0x39;
    memcpy(out + o, kJoinTail, sizeof(kJoinTail)); o += sizeof(kJoinTail);
    return (int)o;
}

/* Net protocol ack: request type + 1 and its sequence id, packet id 0 like the host's
   own net messages. */
static void net_ack(pia_conn_t *c, uint8_t type, const uint8_t *sequence, uint16_t src)
{
    uint8_t reply[8] = {0x01, type, 0x00, 0x00, sequence[0], sequence[1], sequence[2], sequence[3]};
    pia_outbox_t *o = outbox_add(c, 1, reply, sizeof(reply), 0, src);
    if (o) { o->footer = false; o->establishing = true; o->packet = 0; }
}

void pia_conn_feed(pia_conn_t *c, const pia_message_t *m, int tick)
{
    const uint8_t *p = m->payload;
    if (m->protocol == 1 && m->length >= 8)
    {
        /* Net protocol (Pia 6.3x): the host repeats status update (0x11), property update
           (0x50) and keep-alive (0x80) every 500 ms until each station acks with type + 1
           and the same sequence id. An unacked property update is retried for ~10 min,
           then the host drops this station's traffic and the trade freezes. */
        const uint8_t type = p[1];
        const bool request = type == 0x11 || type == 0x50 || type == 0x80;
        if (request)
        {
            const uint32_t sequence = bin_b32(p + 4);
            if (type == c->net_last_type && sequence == c->net_last_seq) ++c->net_repeats;
            else { c->net_last_type = type; c->net_last_seq = sequence; c->net_repeats = 0; }
            ++c->net_requests;
        }
        if (type == 0x11 && c->state == 0 && m->length >= 16)
        {
            memcpy(c->remote_mac, p + 10, 6);
            if (c->host_id == 0) c->host_id = bin_b16(p + 8);
            if (c->our_id == c->host_id) c->our_id = station_id(c->host_id);

            /* Source id 0, packet id 0: the ack form the host accepts. */
            net_ack(c, 0x12, p + 4, 0);

            uint8_t join[PIA_OUTBOX_PAYLOAD];
            int n = build_join(c, join, sizeof(join));
            if (n > 0)
            {
                pia_outbox_t *j = outbox_add(c, 13, join, (size_t)n, 0, c->our_id);
                if (j) { j->compress = true; j->footer = false; j->establishing = true; j->packet = 0; }
            }
        }
        else if (request)
        {
            /* The host ignores acks with the station id and a running packet id. Send both
               source 0 / packet 0 (accepted) and station id / packet 0 (the host's own
               form). Duplicates are harmless. */
            net_ack(c, (uint8_t)(type + 1), p + 4, 0);
            net_ack(c, (uint8_t)(type + 1), p + 4, c->our_id);
        }
    }
    else if (m->protocol == 13 && m->length > 0)
    {
        if (p[0] == 5 && c->host_id != 0 && c->state != 2)
        {
            uint8_t reply[14] = {6};
            memcpy(reply + 1, c->our_mac, 6);
            memset(reply + 7, 0, 6);
            reply[13] = 1;
            outbox_add(c, 13, reply, sizeof(reply), c->host_id, c->our_id);
        }
        if (c->state == 0) c->state = 1;
    }
    else if (m->protocol == 3 || m->protocol == 10)
    {
        if (c->state == 1) c->state = 2;
        if (m->protocol == 3 && c->state != 0 && m->length >= 16 && c->host_id != 0)
        {
            if (p[0] == 0)
            {
                memset(c->template_, 0, sizeof(c->template_));
                memcpy(c->template_, p, m->length < 21 ? m->length : 21);
                c->has_template = true;
                uint8_t response[21];
                memcpy(response, c->template_, 21);
                response[0] = 1;
                pia_outbox_t *o = outbox_add(c, 3, response, sizeof(response), 1, c->our_id);
                if (o) o->footer_id = c->host_id;
            }
            else if (p[0] == 1)
            {
                uint64_t stamp = bin_u64(p + 8);
                for (int i = 0; i < PIA_RTT_PENDING; ++i)
                    if (c->pending[i].used && c->pending[i].stamp == stamp)
                    {
                        if (c->rtt_count < PIA_RTT_QUEUE)
                            c->rtt[c->rtt_count++] = (tick - c->pending[i].tick) * (1000.0 / 59.727);
                        c->pending[i].used = false;
                        break;
                    }
            }
        }
    }
}

void pia_conn_tick(pia_conn_t *c, int tick)
{
    if (!pia_conn_connected(c) || !c->has_template || tick - c->last_rtt_tick < 10) return;
    c->last_rtt_tick = tick;
    ++c->system_time;

    uint8_t probe[21];
    memcpy(probe, c->template_, 21);
    probe[0] = 0;
    bin_w64(probe + 8, c->system_time);

    int slot = -1, oldest = -1;
    for (int i = 0; i < PIA_RTT_PENDING; ++i)
    {
        if (!c->pending[i].used) { slot = i; break; }
        if (oldest < 0 || c->pending[i].tick < c->pending[oldest].tick) oldest = i;
    }
    if (slot < 0) slot = oldest;
    c->pending[slot].stamp = c->system_time;
    c->pending[slot].tick = tick;
    c->pending[slot].used = true;

    pia_outbox_t *o = outbox_add(c, 3, probe, sizeof(probe), 1, c->our_id);
    if (o) o->footer_id = c->host_id;
}

bool pia_conn_take_rtt(pia_conn_t *c, double *out)
{
    if (c->rtt_count == 0) return false;
    *out = c->rtt[0];
    memmove(c->rtt, c->rtt + 1, sizeof(double) * (size_t)(--c->rtt_count));
    return true;
}
