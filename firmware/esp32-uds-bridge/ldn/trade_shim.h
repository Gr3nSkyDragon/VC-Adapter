#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Link fixes between a retail FireRed/LeafGreen cartridge (the GBA) and the Switch
   release of the same game.

   After a trade both games run numbered READY_EXIT_STANDBY rounds around their saves.
   The child sends its round number, the parent answers with the same number, and a
   number that differs from the receiver's counter is ignored. The Switch release
   (pokefirered REVISION 0xA, trade_scene.c CB2_SaveAndEndTrade cases 43/44) runs one
   round more than a retail cartridge. Unfixed, both sides stop at "Communication
   standby".

   Joiner role (Switch = parent, GBA = child):
   - Injects the extra round for the child once per trade, after the cartridge's own
     rounds and a quiet period, then maps round numbers between the two sides.
   - Stamps consecutive mod-8 sequence tags on child commands for the parent's +1
     check. Covers the injected frame, untagged block resends and lost frames.
   - Re-sends an injected round the parent does not answer.
   - Re-sends a parent answer the child missed (child retrying a finished barrier).
     The Switch drops adapter frames around its own stalls.
   - Supplies missing block-fragment echoes before the echo the child acts on. A
     missing echo makes the child's link layer resend the fragment forever.
   - Repeats a block request the child refused while busy. The parent sends it once.

   Lead role (GBA = parent, Switch = child): answers the Switch's extra round, holds
   the GBA's party request and block until the Switch is ready, repeats refused
   requests.

   Events go to a log that survives a chip reset (opening the console resets it). */

void trade_shim_boot(int reset_reason);
void trade_shim_reset(void);

/* Child -> parent UNI frame (2-byte LLSF header + 14-byte command slot), rewritten in
   place. If the child is retrying a barrier the parent already answered, writes one or
   two parent UNI frames (73 bytes each) to `reply` and returns their total length. */
size_t trade_shim_child(uint8_t *payload, size_t length, int64_t now_ms, uint8_t *reply, size_t reply_capacity, bool *forward);

/* Stamps the next sequence tag on a command frame about to go to the parent. Call for
   every frame actually sent, in order. */
void trade_shim_stamp(uint8_t *payload, size_t length);

/* Periodic check: parent no longer echoing child commands. */
void trade_shim_poll(int64_t now_ms);

/* Reliable stream to the Switch stalled: state for the log. */
typedef struct
{
    uint16_t our_low, our_next, our_pending;
    uint16_t peer_low, peer_ack_next, receive_next;
    int64_t stalled_ms, peer_low_age_ms, peer_ack_age_ms, peer_ack_seen_age_ms;
    uint16_t out_of_order, credits, k_queued, k_inflight, out_queued;
    int overflow, idle_evicted;
} trade_shim_pia_t;
void trade_shim_pia_stall(int64_t now_ms, const trade_shim_pia_t *p);
/* Stream moving again after a reported stall. */
void trade_shim_pia_recovered(int64_t now_ms);

/* Parent -> child UNI frame (3-byte LLSF header + five 14-byte slots), rewritten in
   place. When the echo of the child's last block fragment arrives and earlier echoes
   are missing, writes the missing echo frames (73 bytes each) to `pre` and returns
   their total length. They must reach the child before this frame. */
size_t trade_shim_host(uint8_t *payload, size_t length, int64_t now_ms, uint8_t *pre, size_t pre_capacity);

/* The extra child round when due, or a re-send of an unanswered one. Writes a child
   UNI frame and returns its length, else 0. */
size_t trade_shim_inject(int64_t now_ms, uint8_t *out, size_t capacity);

/* Repeat of the parent's block request when the child has not started answering it.
   Writes a parent UNI frame (73 bytes) and returns its length, else 0. Polled with
   trade_shim_inject. */
size_t trade_shim_host_inject(int64_t now_ms, uint8_t *out, size_t capacity);

/* Lead role: the GBA leads and the Switch is its child. The extra post-trade round is
   the Switch's, and the GBA never answers it.
   Child frame (16 bytes from the Switch's adapter) toward the GBA, rewritten in place
   with its own sequence tag. The extra round, or a barrier the GBA already left, is
   answered with a parent frame (73 bytes) in `reply`. `forward` stays true. */
size_t trade_shim_lead_child(uint8_t *payload, size_t length, int64_t now_ms, uint8_t *reply, size_t reply_capacity, bool *forward);
/* Parent frame from the GBA toward the Switch: standby numbers mapped in place, the
   GBA's block kept for a repeat. */
void trade_shim_lead_parent(uint8_t *payload, size_t length, int64_t now_ms);
/* Once per tick: next parent frame (73 bytes) of the GBA's held or unanswered party
   request and its block, else 0. */
size_t trade_shim_lead_inject(int64_t now_ms, uint8_t *out, size_t capacity);
/* Parent frame the next one supersedes (lead role). */
bool trade_shim_parent_sheddable(const uint8_t *payload, size_t length);
/* Adapter's own report: client queue high-water mark, frames dropped from it,
   superseded reports shed, bytes lost on the UART. */
void trade_shim_adapter_counts(uint8_t *queue_high, uint16_t *queue_drops, uint8_t *sheds, uint16_t *uart_overflows);
/* Child frame the next one supersedes: no command, or a held-keys report of no key or
   a direction. The link sheds these while a backlog stands. */
bool trade_shim_sheddable(const uint8_t *payload, size_t length);

/* Bridge context for the log. */
enum { TRADE_SHIM_NOTE_BRIDGE_START = 1, TRADE_SHIM_NOTE_CHILD_CONNECT, TRADE_SHIM_NOTE_CHILD_DISCONNECT,
       TRADE_SHIM_NOTE_HOST_DISCONNECT, TRADE_SHIM_NOTE_BRIDGE_STOP, TRADE_SHIM_NOTE_HOST_SILENCE,
       TRADE_SHIM_NOTE_PARENT_DISCONNECT_CMD, TRADE_SHIM_NOTE_CHILD_TAG_GAP, TRADE_SHIM_NOTE_SWITCH_CLOCK_SKIP,
       TRADE_SHIM_NOTE_ECHO_SYNTHESIZED, TRADE_SHIM_NOTE_PARENT_FRAGMENT_GAP, TRADE_SHIM_NOTE_ECHO_GAP,
       TRADE_SHIM_NOTE_RESTAMPED_RESEND, TRADE_SHIM_NOTE_ADAPTER_TRACE, TRADE_SHIM_NOTE_UNZIP_FAILURE,
       TRADE_SHIM_NOTE_ECHO_STALL, TRADE_SHIM_NOTE_PIA_STALL, TRADE_SHIM_NOTE_STALL_RECOVERED,
       TRADE_SHIM_NOTE_NET_REQUEST, TRADE_SHIM_NOTE_NET_UNACKNOWLEDGED };
void trade_shim_note(int64_t now_ms, uint8_t kind, uint16_t b, uint16_t c);

/* Adapter telemetry frame from the Pico (data-channel payload under 64 bytes).
   Changed loss counters are logged. */
void trade_shim_adapter(int64_t now_ms, const uint8_t *frame, size_t length);

/* Bridge loss counters, sampled every poll. Logged on change and periodically. */
void trade_shim_bridge_counters(int64_t now_ms, uint16_t reordered, uint16_t hold_dropped, uint16_t overflow, uint16_t queued,
                                uint16_t bad_frames, uint16_t decrypt_failures, uint16_t unzip_failures,
                                uint16_t host_skips, uint16_t host_long_skips,
                                uint16_t rx_body_max, uint16_t unzip_last_error);

void trade_shim_status(char *out, size_t capacity);
/* Log dump, a few lines per call so the bridge is not held up: begin, then step until
   it returns false. A pending incident record comes first and is cleared once the
   whole log has been emitted. */
void trade_shim_dump_begin(void);
bool trade_shim_dump_step(void (*emit)(const char *line), int max_lines);
