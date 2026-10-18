#pragma once
#include "pia_conn.h"
#include "pia_reliable.h"

/* Pia session for the GBA relay: reliable stream, K acknowledgements, host polling
   credits, in-order delivery, and the adapter's W frames (WC connect, WA accepted,
   WT data, WD disconnect). */

#define PIA_SEEN_BITS 8192
#define PIA_SEEN_RING 1024
#define PIA_HOLD_SLOTS 32
#define PIA_HOLD_BYTES 128
#define PIA_TIME_RING 256
#define PIA_K_QUEUE 32
#define PIA_OUT_SLOTS 64
/* Queue depth above which superseded child frames are shed. The GBA moves its avatar
   only when the parent echoes its input, so each queued frame is a frame of lag. */
#define PIA_OUT_SHED_AT 3
#define PIA_OUT_BYTES 128

typedef void (*pia_send_fn)(const uint8_t *datagram, size_t length, const char *destination, void *user);
typedef void (*pia_gba_fn)(const uint8_t *payload, size_t length, void *user);
typedef void (*pia_log_fn)(const char *message, void *user);

typedef struct
{
    pia_crypto_t crypto;
    pia_conn_t conn;
    pia_reliable_t reliable;

    char ours[16], host[16];
    pia_send_fn send;
    pia_gba_fn deliver;
    pia_log_fn log;
    void *user;

    uint16_t packet_ids[4];
    uint8_t seen_bits[PIA_SEEN_BITS / 8];
    uint16_t seen_ring[PIA_SEEN_RING];
    int seen_head, seen_count;

    struct { uint16_t seq; uint8_t data[PIA_HOLD_BYTES]; uint16_t length; bool used; } hold[PIA_HOLD_SLOTS];
    int next_deliver;                 /* -1 until the first reliable frame arrives */

    uint32_t time_ring[PIA_TIME_RING];
    int time_head, time_count;
    struct { uint32_t sequence, time; } k_queue[PIA_K_QUEUE];
    int k_head, k_count;
    uint16_t k_inflight[3];
    int k_inflight_count;

    bool opened, connect_sent, ack_owed;
    int credits, last_ack, tick;
    uint64_t nonce;
    uint32_t timestamp, k_sequence;
    uint8_t connect_id[2];

    bool accepted, host_disconnected, connect_wanted;
    int received, decrypt_failures, sent, reordered, hold_dropped;
    int rx_seen, rx_wrong_source, rx_short, rx_bad_frame, rx_messages, rx_unzip_fail;
    int rx_body_max, rx_msgs_max;                  /* per-datagram maxima */
    int rx_unzip_last_error, rx_unzip_last_len;   /* last unzip error code, wire size */
    uint8_t rx_first[16];
    int rx_first_len, rx_first_zipped, rx_first_pad, rx_first_footer;

    /* Child frames waiting for the Switch. Each carries a mod-8 sequence the parent
       checks; a dropped or reordered frame desyncs the trade. */
    struct { uint8_t data[PIA_OUT_BYTES]; uint16_t length; } outbound[PIA_OUT_SLOTS];
    int out_head, out_count, high_water, repeated, overflow;
    uint8_t last_enqueued[PIA_OUT_BYTES];
    uint16_t last_enqueued_len;
    uint8_t idle[PIA_OUT_BYTES];
    uint16_t idle_len;
    bool has_idle;
    int idle_evicted;                 /* idle frames dropped for commands */

    /* Called on each child frame just before it is sent; stamps the per-frame sequence. */
    void (*stamp)(uint8_t *payload, size_t length);
    /* True when a queued child frame is superseded by later ones. Only these are shed,
       oldest first, never the newest. */
    bool (*sheddable)(const uint8_t *payload, size_t length);
    int shed;                         /* superseded child frames dropped while backlogged */

    /* Switch's reliable-stream state, for stall diagnosis: its send-window base (oldest
       unacked frame) and the next local frame it expects, with when each last moved. */
    uint16_t peer_low, peer_ack_next;
    int64_t peer_low_moved_ms, peer_ack_moved_ms, peer_ack_seen_ms;
    bool peer_low_valid, peer_ack_valid;
} pia_link_t;

void pia_link_init(pia_link_t *l, const uint8_t ssid[16], const uint8_t our_mac[6],
                   const uint8_t host_mac[6], const char *our_ip, const char *host_ip,
                   pia_send_fn send, pia_gba_fn deliver, pia_log_fn log, void *user);
void pia_link_receive(pia_link_t *l, const uint8_t *data, size_t length, const char *source);
void pia_link_tick(pia_link_t *l);
/* Queue one child payload for the Switch. */
void pia_link_enqueue(pia_link_t *l, const uint8_t *payload, size_t length);
void pia_link_set_connect(pia_link_t *l, bool wanted);
