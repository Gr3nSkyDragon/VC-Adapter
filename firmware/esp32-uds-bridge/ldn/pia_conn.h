#pragma once
#include "pia_crypto.h"

/* Pia connection setup and liveness: station handshake (protocol 1), session join (13),
   RTT probes (3) that feed the retransmit timer. */

#define PIA_OUTBOX_SLOTS 6
#define PIA_OUTBOX_PAYLOAD 128
#define PIA_RTT_PENDING 32
#define PIA_RTT_QUEUE 16

typedef struct
{
    uint8_t payload[PIA_OUTBOX_PAYLOAD];
    uint16_t length;
    uint8_t protocol;
    uint16_t dst, src;
    bool compress, footer, establishing;
    int packet;      /* -1 for the running per-destination counter */
    int footer_id;   /* -1 to use dst */
} pia_outbox_t;

typedef struct
{
    uint16_t our_id, host_id;
    int state;
    int last_rtt_tick;
    uint8_t our_mac[6], remote_mac[6], random[4];
    char our_ip[16];
    uint8_t template_[21];
    bool has_template;
    uint64_t system_time;
    struct { uint64_t stamp; int tick; bool used; } pending[PIA_RTT_PENDING];
    double rtt[PIA_RTT_QUEUE];
    int rtt_count;
    pia_outbox_t outbox[PIA_OUTBOX_SLOTS];
    int outbox_count;
    /* Net protocol requests from the host (repeated until acked) and copies of the newest
       one received. A copy after our ack means the host did not get the ack. */
    uint32_t net_requests;
    uint8_t net_last_type;
    uint32_t net_last_seq;
    int net_repeats;
} pia_conn_t;

void pia_conn_init(pia_conn_t *c, const uint8_t our_mac[6], const uint8_t host_mac[6], const char *our_ip);
bool pia_conn_connected(const pia_conn_t *c);
void pia_conn_feed(pia_conn_t *c, const pia_message_t *m, int tick);
void pia_conn_tick(pia_conn_t *c, int tick);
bool pia_conn_take_rtt(pia_conn_t *c, double *out);
