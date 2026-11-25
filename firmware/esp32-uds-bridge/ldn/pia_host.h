#pragma once
#include "pia_crypto.h"
#include "pia_reliable.h"

/* Pia session host: the board plays the Switch's side of a room a GBA leads, for up to
   PIA_HOST_STATIONS stations, each one of the GBA's RFU children. Covers the net status and
   property messages (protocol 1), the session (13), round-trip probes (3) and a reliable
   stream (10) per station carrying the emulated adapter's WC/WA/WD/WT/WK frames. Every
   station gets the GBA's frames; each station's frames go to the GBA as its child's. */

#define PIA_HOST_STATIONS 4               /* the adapter serves four children */
#define PIA_HOST_PARENT_BYTES 92
#define PIA_HOST_PARENT_SLOTS 12
#define PIA_HOST_SHED_AT 3
#define PIA_HOST_APP_DATA 384
#define PIA_HOST_HOLD_SLOTS 32
#define PIA_HOST_HOLD_BYTES 48            /* a child's frames: WT, WK, WC, and the 46-byte opener */
#define PIA_HOST_RECORD_BYTES 64
#define PIA_HOST_WT_RING 16
#define PIA_HOST_PROBES 8
#define PIA_HOST_FRAMES 6
/* A station's send window, and its largest frame: a 92-byte parent frame in a WT. */
#define PIA_HOST_WINDOW 16
#define PIA_HOST_FRAME (12 + PIA_HOST_PARENT_BYTES)
#define PIA_HOST_BODY 1280
#define PIA_HOST_INFLATE 1536
/* Stations that left, named in the station list until every station has acknowledged it. */
#define PIA_HOST_LEFT 4

/* Per-session work buffers. A message is built in one of the union's buffers and encoded
   into body before the datagram is written over it. */
typedef struct
{
    uint8_t body[PIA_HOST_BODY], staged[PIA_HOST_BODY];
    union
    {
        uint8_t datagram[PIA_HOST_BODY + 32];
        uint8_t property[38 + PIA_HOST_APP_DATA];
        uint8_t station_list[1152];
    };
    uint8_t wrapped[PIA_HOST_FRAMES][PIA_HOST_FRAME + 8], ack[20];
    uint8_t plain[PIA_HOST_BODY], app[PIA_HOST_INFLATE];
    uint8_t wt[PIA_HOST_FRAME];
} pia_host_scratch_t;

typedef void (*pia_host_send_fn)(const uint8_t *datagram, size_t length, const char *destination, void *user);
/* A child adapter payload from station `station`: 2-byte LLSF header and one 14-byte slot. */
typedef void (*pia_host_child_fn)(const uint8_t *payload, size_t length, int station, void *user);
typedef void (*pia_host_log_fn)(const char *message, void *user);

typedef struct
{
    int state;                        /* 0 free, 1 status sent, 2 joined, 3 session up */
    char ip[16];
    uint8_t mac[6];                   /* its constant id: the LDN MAC until its join names one */
    uint16_t id;                      /* the station's, from its join */
    uint8_t index;                    /* its LDN member index */
    uint8_t join_number;              /* the session's joins so far, this one included */
    uint16_t joined_seq;              /* the station list version that added it */

    pia_reliable_t reliable;
    pia_pending_t pending[PIA_HOST_WINDOW];
    uint8_t pending_data[PIA_HOST_WINDOW][PIA_HOST_FRAME];

    bool connect_requested;           /* WC arrived and was handed to the bridge */
    bool child_accepted;              /* WA sent */
    bool child_disconnected;          /* WD sent or the child left */
    uint8_t connect_id[2];
    uint8_t client_slot;              /* the GBA's slot for this child, in its WA */

    /* Parent UNI frames waiting to go, one per tick. A command-less frame only carries
       state, so a newer one replaces it. */
    struct { uint8_t data[PIA_HOST_PARENT_BYTES]; uint8_t length; } parent[PIA_HOST_PARENT_SLOTS];
    int parent_head, parent_count;

    uint16_t packet_ids[2];
    int64_t next_net_ms, next_property_ms, next_update_ms, joined_ms;
    bool status_acked, property_acked;
    uint16_t update_acked;            /* the station list version it acknowledged, 0 none */

    /* The child's name record from its join, echoed in the station list. */
    uint8_t joiner_record[PIA_HOST_RECORD_BYTES];
    int joiner_record_len;
    uint8_t join_token[4];
    bool child_alive;                 /* the join reply arrived */
    int join_replies;
    int64_t next_join_reply_ms, next_probe_ms;
    struct { uint64_t stamp; int64_t sent_ms; bool used; } probes[PIA_HOST_PROBES];
    bool stream_opened, ack_owed, wt_sent, uni_sent;
    int wg_pending;                   /* a WG value to send, -1 none */
    int last_wt_tick;
    /* Child frames are delivered in order; early ones wait here. */
    int next_deliver;                 /* -1 until the child's first frame */
    struct { uint16_t seq; uint8_t data[PIA_HOST_HOLD_BYTES]; uint8_t length; bool used; } hold[PIA_HOST_HOLD_SLOTS];
    /* Sent WT frames by stamp, for RTT samples from WKs. */
    struct { uint32_t stamp; uint16_t seq; int64_t sent_ms; bool used, idle; } wt_ring[PIA_HOST_WT_RING];
    int wt_ring_head;
    int teardown;                     /* 0 none, 1 kind 9, 2 kind 13, 3 status with the slot marked, 4 with it cleared */
    int teardown_sent;
    int64_t next_teardown_ms;
    /* Pacing: each datagram from the child allows one data frame to it. child_stamp
       tracks the child's frame counter; skipped counts its gaps. */
    int credits;
    int64_t last_child_datagram_ms;
    uint32_t child_stamp;
    bool child_stamp_valid;
    int child_skipped;
    uint8_t silent_noted;             /* stations it reported unheard, by station slot */

    int received, sent, decrypt_failures, child_frames, parent_frames;
    int rx_bad_frame, rx_messages, rx_unzip_fail, resends;
    int parent_dropped, hold_dropped, wk_acks, shed;
} pia_host_station_t;

/* A station that left: the list versions that added and removed it, and its record. */
typedef struct
{
    uint8_t mac[6];
    uint16_t id, joined_seq, left_seq;
    uint8_t join_number, record_len;
    uint8_t record[PIA_HOST_RECORD_BYTES];
} pia_host_left_t;

typedef struct
{
    pia_crypto_t crypto;
    char ours[16];
    uint8_t our_mac[6];
    uint16_t our_id;                  /* random */
    pia_host_send_fn send;
    pia_host_child_fn deliver;
    pia_host_log_fn log;
    /* True for a parent frame a later one replaces (no key held, a direction); shed
       beyond PIA_HOST_SHED_AT queued. */
    bool (*sheddable)(const uint8_t *payload, size_t length);
    void *user;
    pia_host_scratch_t sc;
    pia_host_station_t st[PIA_HOST_STATIONS];
    /* The GBA's latest command-less frame, repeated to an idle child. */
    uint8_t idle[PIA_HOST_PARENT_BYTES];
    uint8_t idle_len;
    bool has_idle;

    int tick;
    uint32_t timestamp;               /* the frame counter stamped on WT frames */
    uint64_t nonce;
    uint32_t net_seq;                 /* the net status version */
    uint16_t list_seq;                /* the station list version, 0 before the first join */
    bool member_left;                 /* a station left this session, as the list marks it */
    int joins;
    pia_host_left_t left[PIA_HOST_LEFT];
    int left_count;
    int rx_wrong_source;

    int64_t now_ms;                   /* the latest tick's clock, the stream's timebase */
    uint8_t app_data[PIA_HOST_APP_DATA];
    int app_data_len;
    uint32_t property_seq;
    bool property_pending;            /* new application data not yet sent */
    uint16_t adapter_id;              /* the WA's first word, +3 per group */
    uint16_t wa_id;                   /* the one this group's WAs carry */
} pia_host_t;

void pia_host_init(pia_host_t *h, const uint8_t ssid[16], const uint8_t our_mac[6], const char *our_ip,
                   pia_host_send_fn send, pia_host_child_fn deliver, pia_host_log_fn log, void *user);
/* Start the Pia handshake with an authenticated LDN member. Returns its station slot, or
   -1 with every slot taken. */
int pia_host_station_joined(pia_host_t *h, const uint8_t mac[6], const char *ip, uint8_t index);
/* The LDN member with this index left the room. */
void pia_host_station_left(pia_host_t *h, uint8_t index);
void pia_host_receive(pia_host_t *h, const uint8_t *data, size_t length, const char *source);
/* Once per GBA frame (59.727 Hz). */
void pia_host_tick(pia_host_t *h, int64_t now_ms);
/* A parent payload from the GBA, for every station: 3-byte LLSF header and up to five
   14-byte slots. */
void pia_host_parent_frame(pia_host_t *h, const uint8_t *payload, size_t length);
/* A parent frame made for one station's child only. */
void pia_host_parent_frame_to(pia_host_t *h, int station, const uint8_t *payload, size_t length);
/* The station whose WC came first and is still unanswered, or -1. The bridge answers with
   pia_host_accept once the GBA has. */
int pia_host_connect_pending(const pia_host_t *h);
void pia_host_accept(pia_host_t *h, int station, bool accepted, uint8_t client_slot);
/* The GBA dropped this station's child: send WD. */
void pia_host_disconnect_child(pia_host_t *h, int station);
bool pia_host_session_up(const pia_host_t *h, int station);
/* Stations in the room, joined or joining. */
int pia_host_station_count(const pia_host_t *h);
/* Application data for the property update (0x50). */
void pia_host_set_app_data(pia_host_t *h, const uint8_t *data, int length);
