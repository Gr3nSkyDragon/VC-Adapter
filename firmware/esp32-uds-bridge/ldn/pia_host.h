#pragma once
#include "pia_crypto.h"
#include "pia_reliable.h"

/* Pia session host: the board plays the Switch's side of a room a GBA leads, for one
   Switch station (the RFU child). Covers the net status and property messages (protocol
   1), the join (13), round-trip probes (3) and the reliable stream (10) carrying the
   emulated adapter's WC/WA/WD/WT/WK frames. */

#define PIA_HOST_PARENT_BYTES 92
#define PIA_HOST_PARENT_SLOTS 12
#define PIA_HOST_SHED_AT 3
#define PIA_HOST_APP_DATA 384
#define PIA_HOST_HOLD_SLOTS 32
#define PIA_HOST_HOLD_BYTES 96
#define PIA_HOST_RECORD_BYTES 64
#define PIA_HOST_WT_RING 16
#define PIA_HOST_PROBES 8
#define PIA_HOST_FRAMES 6
#define PIA_HOST_BODY 1024
#define PIA_HOST_INFLATE 1536

/* Per-session work buffers, sized for a child's small frames. */
typedef struct
{
    uint8_t body[PIA_HOST_BODY], staged[PIA_HOST_BODY], datagram[PIA_HOST_BODY];
    uint8_t property[38 + PIA_HOST_APP_DATA];
    uint8_t station_list[192 + 32 + PIA_HOST_RECORD_BYTES];
    uint8_t wrapped[PIA_HOST_FRAMES][PIA_RELIABLE_PAYLOAD + 8], ack[20];
    uint8_t plain[PIA_HOST_BODY], app[PIA_HOST_INFLATE];
    uint8_t wt[PIA_RELIABLE_PAYLOAD];
} pia_host_scratch_t;

typedef void (*pia_host_send_fn)(const uint8_t *datagram, size_t length, const char *destination, void *user);
/* A child adapter payload: 2-byte LLSF header and one 14-byte slot. */
typedef void (*pia_host_child_fn)(const uint8_t *payload, size_t length, void *user);
typedef void (*pia_host_log_fn)(const char *message, void *user);

typedef struct
{
    pia_crypto_t crypto;
    pia_reliable_t reliable;
    char ours[16], station_ip[16];
    uint8_t our_mac[6], station_mac[6];
    uint16_t our_id, station_id;      /* ours random, the child's from its join */
    uint8_t station_index;
    pia_host_send_fn send;
    pia_host_child_fn deliver;
    pia_host_log_fn log;
    /* True for a parent frame a later one replaces (no key held, a direction); shed
       beyond PIA_HOST_SHED_AT queued. */
    bool (*sheddable)(const uint8_t *payload, size_t length);
    void *user;
    pia_host_scratch_t sc;

    int state;                        /* 0 no station, 1 status sent, 2 joined, 3 session up */
    bool connect_requested;           /* WC arrived and was handed to the bridge */
    bool child_accepted;              /* WA sent */
    bool child_disconnected;          /* WD sent or the child left */
    uint8_t connect_id[2];

    /* Parent UNI frames waiting to go, one per tick. A command-less frame only carries
       state, so a newer one replaces it. */
    struct { uint8_t data[PIA_HOST_PARENT_BYTES]; uint8_t length; } parent[PIA_HOST_PARENT_SLOTS];
    int parent_head, parent_count;
    uint8_t idle[PIA_HOST_PARENT_BYTES];
    uint8_t idle_len;
    bool has_idle;

    int tick;
    uint32_t timestamp;               /* the frame counter stamped on WT frames */
    uint64_t nonce;
    uint16_t packet_ids[4];
    uint32_t net_seq;
    int64_t next_net_ms, next_keepalive_ms, joined_ms;
    bool status_acked, property_acked;

    int received, sent, decrypt_failures, child_frames, parent_frames;
    int rx_bad_frame, rx_messages, rx_unzip_fail, resends;
    int rx_wrong_source, parent_dropped, hold_dropped, wk_acks, shed;
    /* Pacing: each datagram from the child allows one data frame to it. child_stamp
       tracks the child's frame counter; skipped counts its gaps. */
    int credits;
    int64_t last_child_datagram_ms;
    uint32_t child_stamp;
    bool child_stamp_valid;
    int child_skipped;

    int64_t now_ms;                   /* the latest tick's clock, the stream's timebase */
    uint8_t app_data[PIA_HOST_APP_DATA];
    int app_data_len;
    uint32_t property_seq;
    bool property_pending;            /* new application data not yet sent */
    /* The child's name record from its join, echoed in the station list. */
    uint8_t joiner_record[PIA_HOST_RECORD_BYTES];
    int joiner_record_len;
    uint8_t join_token[4];
    bool child_alive;                 /* the join reply arrived */
    int join_replies;
    int64_t next_join_reply_ms, next_probe_ms;
    struct { uint64_t stamp; int64_t sent_ms; bool used; } probes[PIA_HOST_PROBES];
    uint16_t adapter_id;              /* the WA's first word, +3 per connection */
    bool stream_opened, ack_owed, wt_sent, uni_sent;
    int wg_pending;                   /* a WG value to send, -1 none */
    int last_wt_tick;
    /* Child frames are delivered in order; early ones wait here. */
    int next_deliver;                 /* -1 until the child's first frame */
    struct { uint16_t seq; uint8_t data[PIA_HOST_HOLD_BYTES]; uint16_t length; bool used; } hold[PIA_HOST_HOLD_SLOTS];
    /* Sent WT frames by stamp, for RTT samples from WKs. */
    struct { uint32_t stamp; uint16_t seq; int64_t sent_ms; bool used, idle; } wt_ring[PIA_HOST_WT_RING];
    int wt_ring_head;
    int teardown;                     /* 0 none, 1 kind 9, 2 kind 13, 3 status with the slot marked, 4 with it cleared */
    int teardown_sent;
    int64_t next_teardown_ms;
} pia_host_t;

void pia_host_init(pia_host_t *h, const uint8_t ssid[16], const uint8_t our_mac[6], const char *our_ip,
                   pia_host_send_fn send, pia_host_child_fn deliver, pia_host_log_fn log, void *user);
/* Start the Pia handshake with an authenticated LDN member. */
void pia_host_station_joined(pia_host_t *h, const uint8_t mac[6], const char *ip, uint8_t index);
void pia_host_station_left(pia_host_t *h);
void pia_host_receive(pia_host_t *h, const uint8_t *data, size_t length, const char *source);
/* Once per GBA frame (59.727 Hz). */
void pia_host_tick(pia_host_t *h, int64_t now_ms);
/* A parent payload from the GBA: 3-byte LLSF header and up to five 14-byte slots. */
void pia_host_parent_frame(pia_host_t *h, const uint8_t *payload, size_t length);
/* The child sent WC; the bridge answers with pia_host_accept once the GBA has. */
bool pia_host_connect_pending(const pia_host_t *h);
void pia_host_accept(pia_host_t *h, bool accepted);
/* The GBA dropped the child: send WD. */
void pia_host_disconnect_child(pia_host_t *h);
bool pia_host_session_up(const pia_host_t *h);
/* Application data for the property update (0x50). */
void pia_host_set_app_data(pia_host_t *h, const uint8_t *data, int length);
