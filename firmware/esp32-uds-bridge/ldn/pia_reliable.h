#pragma once
#include "pia_bin.h"

/* Pia reliable stream: selective repeat, 128-bit ack bitmap, RTO from recent round
   trips, fast retransmit on a repeated gap report. The send window and its frame size are
   the owner's: the joiner's stream to the host carries large frames, a hosted station's
   stream only small ones. */

#define PIA_RELIABLE_SLOTS 64            /* the joiner's window */
#define PIA_RELIABLE_PAYLOAD 256
#define PIA_RELIABLE_OOO 64
#define PIA_RTT_SAMPLES 7

typedef struct
{
    double time;
    uint8_t *data;
    int resends;
    uint16_t length;
    uint16_t seq;
    uint8_t flags;
    bool acked;
    bool used;
} pia_pending_t;

typedef struct
{
    uint16_t seq;
    uint8_t flags;
    const uint8_t *data;
    uint16_t length;
} pia_packet_t;

typedef struct
{
    uint16_t next, low, receive_next;
    pia_pending_t *pending;
    uint16_t slots, payload;
    int pending_count;
    uint16_t out_of_order[PIA_RELIABLE_OOO];
    int ooo_count;
    double samples[PIA_RTT_SAMPLES];
    int sample_count;
    int gap, gap_count;
    bool receive_started;
} pia_reliable_t;

/* `slots` (a power of two) entries in `pending`, each with `payload` bytes of `data`. */
void pia_reliable_init(pia_reliable_t *r, pia_pending_t *pending, uint8_t *data, uint16_t slots, uint16_t payload);
/* The frame waiting with this sequence, or NULL. */
const pia_pending_t *pia_reliable_entry(const pia_reliable_t *r, uint16_t seq);
double pia_reliable_rto(const pia_reliable_t *r);
void pia_reliable_add_rtt(pia_reliable_t *r, double ms);
uint16_t pia_reliable_send_low(const pia_reliable_t *r);
bool pia_reliable_has_gap(const pia_reliable_t *r);
int pia_reliable_pending(const pia_reliable_t *r);

/* Queue a frame for transmission. Returns its sequence, or -1 when the window is full. */
int pia_reliable_queue(pia_reliable_t *r, const uint8_t *data, size_t len, uint8_t flags, double time);
void pia_reliable_acknowledge(pia_reliable_t *r, const uint8_t *data, size_t len, double time);
int pia_reliable_retransmit(pia_reliable_t *r, double time, int limit, pia_packet_t *out, int max);
void pia_reliable_receive(pia_reliable_t *r, uint16_t seq, uint16_t advertised_low);
void pia_reliable_ack(const pia_reliable_t *r, uint8_t out[20]);
/* Frame header: flags, length, sequence, send-window base. */
int pia_reliable_wrap(const pia_reliable_t *r, const pia_packet_t *p, uint8_t *out, size_t cap);
