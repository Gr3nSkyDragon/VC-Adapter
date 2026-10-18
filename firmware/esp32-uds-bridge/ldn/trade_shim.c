#include "trade_shim.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "ldn_wire.h"
/* Console protocol frames log lines in binary mode. The host test keeps plain printf. */
#define printf ldn_wire_printf
#else
#define RTC_NOINIT_ATTR
#endif

#define RFUCMD_READY_EXIT_STANDBY 0x6600
#define RFUCMD_SEND_BLOCK_INIT 0x8800
#define RFUCMD_SEND_BLOCK 0x8900
#define RFUCMD_SEND_BLOCK_REQ 0xa100
#define RFUCMD_SEND_HELD_KEYS 0xbe00
#define LINK_KEY_CODE_EMPTY 0x11
#define LINK_KEY_CODE_DPAD_RIGHT 0x15   /* 0x12-0x15: directions */
#define LINKCMD_CONFIRM_FINISH_TRADE 0xdcba

/* Post-trade standby rounds of a retail cartridge: trade_scene.c CB2_SaveAndEndTrade
   cases 1, 41, 5, 8, then trade.c CB2_CreateTradeMenu case 4. After the fifth answer
   the Switch parent waits in its sixth barrier. */
#define TRADE_SHIM_CHILD_ROUNDS 5
/* Quiet time after the fifth answer before injecting. Covers the parent processing its
   answer (1-2 frames) and a five-round parent sending its party request (~12 frames). */
#define TRADE_SHIM_SETTLE_MS 750
/* Fallback for other round counts: any post-trade answer followed by this much silence
   on both sides. Longer than the cartridge's save, the longest normal gap. */
#define TRADE_SHIM_FALLBACK_MS 8000
/* A waiting parent answers the extra round within 1-2 frames. */
#define TRADE_SHIM_ACK_MS 2000
#define TRADE_SHIM_MAX_ATTEMPTS 4
#define TRADE_SHIM_MAX_FAKES 16
/* A child that accepts a block request starts sending within 1-2 frames. Silence this
   long means it refused (Rfu_InitBlockSend returns FALSE while a block send or another
   command is pending). Measured as child silence so a slow accepted request is not
   repeated. */
#define TRADE_SHIM_REREQUEST_MS 700
#define TRADE_SHIM_MAX_REREQUESTS 3
/* Lead role: the Switch buffers its party and clears its block flags a few frames
   after its extra round is answered. Delay before releasing the GBA's held request and
   block. A hold with no extra round ends after the timeout. */
#define TRADE_SHIM_RELEASE_MS 150
#define TRADE_SHIM_HOLD_TIMEOUT_MS 5000
/* Lead role: repeat an unanswered request after this. Longer because the Switch's
   answering block can take a while to start. */
#define TRADE_SHIM_LEAD_REREQUEST_MS 1500
/* Fragments per block request type. sBlockRequests[] in link_rfu_2.c: 200, 200, 100,
   220, 40 bytes in 12-byte fragments. */
static const uint8_t kRequestFragments[] = {17, 17, 9, 19, 4};

/* ---- reset-surviving event log ---------------------------------------------- */

enum { EV_BOOT = 1, EV_RESET, EV_CONFIRM, EV_PARENT_STANDBY, EV_CHILD_STANDBY, EV_REQUEST, EV_INJECT,
       EV_ACK, EV_REANSWER, EV_REREQUEST, EV_NOTE, EV_HOST_BLOCK, EV_GIVE_UP, EV_ADAPTER, EV_BRIDGE, EV_TRACE,
       EV_SENT, EV_ECHO, EV_PIA };

#define LOG_MAGIC 0x53484d34u   /* "SHM4" */
#define LOG_SLOTS 400
/* History before the first stall since the last dump. Kept outside the ring so a long
   failure cannot overwrite its own start. */
#define INCIDENT_SLOTS 200
#define INCIDENT_HISTORY 150

typedef struct { uint32_t ms; uint8_t type, a; uint16_t b, c; } log_entry_t;

static RTC_NOINIT_ATTR struct
{
    uint32_t magic;
    uint16_t head, count;
    uint16_t boots;
    log_entry_t entries[LOG_SLOTS];
    uint16_t incident_count, incident_boot;
    log_entry_t incident[INCIDENT_SLOTS];
} rtc_log;

static bool incident_open;   /* new entries also go to the incident */
static int incident_reason;  /* stall that owns the record: 0 none, 1 pia, 2 echo */

static void log_event_at(uint32_t ms, uint8_t type, uint8_t a, uint16_t b, uint16_t c)
{
    if (rtc_log.magic != LOG_MAGIC) return;
    log_entry_t *e = &rtc_log.entries[(rtc_log.head + rtc_log.count) % LOG_SLOTS];
    if (rtc_log.count == LOG_SLOTS) rtc_log.head = (uint16_t)((rtc_log.head + 1) % LOG_SLOTS);
    else ++rtc_log.count;
    e->ms = ms; e->type = type; e->a = a; e->b = b; e->c = c;
    if (incident_open && rtc_log.incident_count < INCIDENT_SLOTS) rtc_log.incident[rtc_log.incident_count++] = *e;
}

static void log_event(int64_t now_ms, uint8_t type, uint8_t a, uint16_t b, uint16_t c)
{
    log_event_at((uint32_t)now_ms, type, a, b, c);
}

/* Starts an incident record unless an unread one exists. Copies recent history, then
   collects entries until incident_end. */
static bool incident_begin(void)
{
    if (rtc_log.magic != LOG_MAGIC || rtc_log.incident_count) return false;
    int n = rtc_log.count < INCIDENT_HISTORY ? rtc_log.count : INCIDENT_HISTORY;
    for (int i = 0; i < n; ++i)
        rtc_log.incident[i] = rtc_log.entries[(rtc_log.head + rtc_log.count - n + i) % LOG_SLOTS];
    rtc_log.incident_count = (uint16_t)n;
    rtc_log.incident_boot = rtc_log.boots;
    incident_open = true;
    return true;
}

static void incident_end(void) { incident_open = false; }

/* Stall cleared on its own: free the record. Entries stay in the ring. */
static void incident_resolved(int64_t now_ms, int reason)
{
    if (incident_reason != reason) return;
    incident_reason = 0;
    rtc_log.incident_count = 0;
    log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_STALL_RECOVERED, (uint16_t)reason, 0);
}

void trade_shim_boot(int reset_reason)
{
    if (rtc_log.magic != LOG_MAGIC || rtc_log.head >= LOG_SLOTS || rtc_log.count > LOG_SLOTS ||
        rtc_log.incident_count > INCIDENT_SLOTS)
    {
        memset(&rtc_log, 0, sizeof(rtc_log));
        rtc_log.magic = LOG_MAGIC;
    }
    ++rtc_log.boots;
    log_event(0, EV_BOOT, (uint8_t)reset_reason, rtc_log.boots, 0);
}

/* ---- state ------------------------------------------------------------------- */

static struct
{
    uint16_t fakes[TRADE_SHIM_MAX_FAKES];   /* extra rounds, Switch numbering, ascending */
    int fake_count;
    int base;                 /* fakes evicted from the list, as an offset */
    uint8_t send_tag;         /* tag for the next frame sent to the parent */
    bool have_tag;            /* child has sent a command */
    bool post_trade;          /* from trade confirmation to the next block request */
    bool injected;            /* extra round supplied for this trade */
    int last_round;           /* newest parent standby answer, parent numbering, -1 none */
    int last_child_round;     /* newest forwarded child standby, parent numbering */
    int answers_since_confirm;
    int fake_round;           /* extra round, parent numbering, -1 none */
    bool fake_acked;
    int attempts;
    int64_t injected_at;
    int64_t quiet_since;
    int host_block_count;     /* fragment count of the parent's last BLOCK_INIT */
    int request_type;         /* parent's newest block request type, -1 none */
    bool request_served;      /* child has started answering it */
    int64_t request_at;       /* time of the request or its latest repeat */
    int request_attempts;
    int injections, reanswers, rerequests, trades, give_ups;
    uint16_t child_commands;  /* command frames sent to the parent, injected included */
    uint16_t echoes;          /* host frames echoing a child command */
    bool raw_tag_valid;
    uint8_t last_raw_tag;
    int tag_gaps;
    /* Child's block in flight. The parent echoes each accepted fragment once, in slot 1.
       A missing echo is a send failure for the child (its resends carry no tag). */
    struct { bool active; uint8_t count; uint32_t sent, echoed; uint8_t data[32][12]; } cb;
    /* Parent's block in flight. Joiner role: gap logging. Lead role: fragments kept for
       replay, since the Switch wipes a block that arrives early. */
    struct { bool active; uint8_t count, expect; uint32_t have; uint8_t data[32][12]; } pb;
    int replay_index, replay_count;   /* lead release: init, fragments, request */
    bool request_answering;           /* lead: Switch's answering block has begun */
    /* Lead hold: GBA block commands held from its last post-trade round until the
       Switch's extra round is answered. */
    bool hold;
    int64_t hold_since, release_at;
    int echoes_synthesized, resends_restamped;
    /* Child's newest command frame as received. */
    uint8_t last_frame[16];
    bool have_last_frame;
    int64_t last_command_ms;
    /* Recent command frames sent to the parent and their echoes, for the log. */
    struct { uint32_t ms; uint16_t command; uint8_t tag; } sent_ring[16];
    struct { uint32_t ms; uint16_t command; } echo_ring[16];
    int sent_head, echo_head, sent_count, echo_count;
    int64_t clock_ms, last_echo_ms;
    int sent_since_echo;
    bool echo_stall_reported;
    uint32_t request_frags;   /* fragment indices sent since the request */
    int duplicates;
} s;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void trade_shim_reset(void)
{
    memset(&s, 0, sizeof(s));
    s.last_round = -1;
    s.fake_round = -1;
    s.request_type = -1;
    log_event(0, EV_RESET, 0, 0, 0);
}

static int64_t note_last_ms[64];

void trade_shim_note(int64_t now_ms, uint8_t kind, uint16_t b, uint16_t c)
{
    /* Recur while the Switch's stream is disturbed. One per form per 10 s. */
    if (kind == TRADE_SHIM_NOTE_HOST_SILENCE || kind == TRADE_SHIM_NOTE_SWITCH_CLOCK_SKIP)
    {
        int key = (kind * 2 + (c != 0 && kind == TRADE_SHIM_NOTE_HOST_SILENCE)) % 64;
        if (note_last_ms[key] && now_ms - note_last_ms[key] < 10000) return;
        note_last_ms[key] = now_ms;
    }
    log_event(now_ms, EV_NOTE, kind, b, c);
}

/* ---- loss counters ------------------------------------------------------------ */

/* Full snapshot every TELEMETRY_PERIOD_MS. In between, only lines whose watched counters
   moved, at most every CHANGE_LOG_MS, so a long failure cannot flush its own start. */
#define TELEMETRY_PERIOD_MS 60000
#define CHANGE_LOG_MS 10000

static struct
{
    /* Pico, from tags 0x1D / 0x2E / 0x1E */
    uint16_t park_drops, park_idle_drops, uart_overflows, fifo_drops, aborts, retries;
    uint8_t park_high, fifo_high, sd_resets, deliv_fail, comstate, sheds;
    uint16_t err_responses, login_restarts;
    uint8_t ev_data, ev_rtx, ev_timeo, ev_disc, link_pwr_zero, wipe15, wipe16, wipe17;
    uint8_t last_cmd, last_plen, ring[8];
    uint16_t commands, out_ring_drops;
    uint16_t gba_restarts;
    uint8_t last_trace_seq;
    uint16_t trace_snapshots;
    bool seen;
    /* bridge */
    uint16_t reordered, hold_dropped, overflow, queued_high, bad_frames, decrypt_failures, unzip_failures;
    uint16_t host_skips, host_long_skips;   /* host frames with adapter clock jump of 2 / 3+ */
    uint16_t rx_body_max, unzip_last_error;
    int64_t last_logged, last_change_logged;
} t;

typedef struct { uint8_t type, a; uint16_t b, c; uint8_t watch; } counter_line_t;   /* watch: 1 = b, 2 = c */
#define COUNTER_LINES 18

static uint8_t sat8(uint16_t v) { return v > 255 ? 255 : (uint8_t)v; }

static int counter_lines(counter_line_t *l)
{
    int n = 0;
#define LINE(type_, a_, b_, c_, watch_) \
    (l[n] = (counter_line_t){ (uint8_t)(type_), (uint8_t)(a_), (uint16_t)(b_), (uint16_t)(c_), (uint8_t)(watch_) }, ++n)
    LINE(EV_ADAPTER, 0x10, t.park_drops, t.uart_overflows, 3);
    LINE(EV_ADAPTER, 0x20, t.fifo_drops, t.fifo_high | (t.park_high << 8), 1);
    LINE(EV_ADAPTER, 0x30, t.aborts, t.retries, 3);
    LINE(EV_ADAPTER, 0x40, t.sd_resets | (t.deliv_fail << 8), sat8(t.err_responses) | (sat8(t.login_restarts) << 8), 3);
    LINE(EV_ADAPTER, 0x50, t.park_idle_drops, t.comstate, 0);
    LINE(EV_ADAPTER, 0x60, t.ev_timeo | (t.ev_disc << 8), t.ev_data | (t.ev_rtx << 8), 1);
    LINE(EV_ADAPTER, 0x70, t.link_pwr_zero | (t.wipe15 << 8), t.wipe16 | (t.wipe17 << 8), 3);
    LINE(EV_ADAPTER, 0x80, t.ring[0] | (t.ring[1] << 8), t.ring[2] | (t.ring[3] << 8), 0);
    LINE(EV_ADAPTER, 0x90, t.ring[4] | (t.ring[5] << 8), t.ring[6] | (t.ring[7] << 8), 0);
    LINE(EV_ADAPTER, 0xa0, t.last_cmd | (t.last_plen << 8), t.commands, 0);
    LINE(EV_ADAPTER, 0xb0, t.out_ring_drops, t.queued_high, 1);
    LINE(EV_ADAPTER, 0xc0, t.gba_restarts, t.trace_snapshots, 3);
    LINE(EV_BRIDGE, 0, t.hold_dropped, t.reordered, 3);
    LINE(EV_BRIDGE, 2, t.bad_frames, t.decrypt_failures, 3);
    LINE(EV_BRIDGE, 4, t.host_skips, t.host_long_skips, 0);
    LINE(EV_BRIDGE, 6, s.child_commands, s.echoes, 0);
    LINE(EV_BRIDGE, 8, t.rx_body_max, t.unzip_last_error, 3);
    LINE(EV_BRIDGE, 10, t.overflow, t.unzip_failures, 3);
#undef LINE
    return n;
}

static counter_line_t logged_lines[COUNTER_LINES];
static int logged_count;

static bool line_moved(const counter_line_t *now, const counter_line_t *then)
{
    return ((now->watch & 1) && now->b != then->b) || ((now->watch & 2) && now->c != then->c);
}

/* periodic: every line. Otherwise moved watched lines plus the unwatched context lines
   (adapter state, command ring, clock skips, command/echo counts). */
static void log_counters(int64_t now_ms, bool periodic)
{
    counter_line_t current[COUNTER_LINES];
    int n = counter_lines(current);
    for (int i = 0; i < n; ++i)
    {
        bool moved = i >= logged_count || line_moved(&current[i], &logged_lines[i]);
        if (periodic || current[i].watch == 0 || moved)
            log_event(now_ms, current[i].type, (uint8_t)(current[i].a | (periodic ? 0 : 1)), current[i].b, current[i].c);
    }
    memcpy(logged_lines, current, (size_t)n * sizeof(current[0]));
    logged_count = n;
}

static void consider_logging(int64_t now_ms)
{
    if (!t.seen) return;
    if (now_ms - t.last_logged >= TELEMETRY_PERIOD_MS)
    {
        t.last_logged = now_ms;
        log_counters(now_ms, true);
        return;
    }
    if (now_ms - t.last_change_logged < CHANGE_LOG_MS) return;
    counter_line_t current[COUNTER_LINES];
    int n = counter_lines(current);
    bool moved = n != logged_count;
    for (int i = 0; i < n && !moved; ++i) moved = line_moved(&current[i], &logged_lines[i]);
    if (!moved) return;
    t.last_change_logged = now_ms;
    log_counters(now_ms, false);
}

void trade_shim_adapter(int64_t now_ms, const uint8_t *f, size_t n)
{
    if (n == 25 && f[0] == 0x1d)
    {
        t.ev_data = f[1]; t.ev_rtx = f[2]; t.ev_timeo = f[3]; t.ev_disc = f[4];
        t.link_pwr_zero = f[9];
        t.wipe15 = f[15]; t.wipe16 = f[16]; t.wipe17 = f[17];
        t.sd_resets = f[14];
        if (f[18] > t.park_drops) t.park_drops = f[18];   /* 0x1E has the wider copy */
        if (f[19] > t.fifo_drops) t.fifo_drops = f[19];
        if (f[20] > t.fifo_high) t.fifo_high = f[20];
        t.sheds = f[22];
    }
    else if (n == 62 && f[0] == 0x2e)
    {
        t.comstate = f[1];
        t.last_cmd = f[5]; t.last_plen = f[6];
        memset(t.ring, 0, sizeof(t.ring));
        for (int i = 0; i < 8 && i < f[7]; ++i) t.ring[i] = f[8 + i];
        t.commands = rd16(f + 28);
        uint32_t aborts = rd16(f + 32) | ((uint32_t)rd16(f + 34) << 16);
        uint32_t restarts = rd16(f + 36) | ((uint32_t)rd16(f + 38) << 16);
        uint32_t err = rd16(f + 40) | ((uint32_t)rd16(f + 42) << 16);
        uint32_t retries = rd16(f + 58) | ((uint32_t)rd16(f + 60) << 16);
        t.aborts = aborts > 65535 ? 65535 : (uint16_t)aborts;
        t.login_restarts = restarts > 65535 ? 65535 : (uint16_t)restarts;
        t.err_responses = err > 65535 ? 65535 : (uint16_t)err;
        t.retries = retries > 65535 ? 65535 : (uint16_t)retries;
        t.deliv_fail = f[56];
    }
    else if (n >= 15 && f[0] == 0x1e)
    {
        if (n >= 17) t.out_ring_drops = rd16(f + 15);
        if (n >= 19) t.gba_restarts = rd16(f + 17);
        t.uart_overflows = rd16(f + 1);
        if (f[3] > t.park_high) t.park_high = f[3];
        t.park_drops = rd16(f + 4);
        t.fifo_drops = rd16(f + 6);
        if (f[8] > t.fifo_high) t.fifo_high = f[8];
        t.aborts = rd16(f + 9);
        t.retries = rd16(f + 11);
        t.park_idle_drops = rd16(f + 13);
    }
    else if (n >= 4 && f[0] == 0x2d && f[1] != t.last_trace_seq)
    {
        /* Adapter exchange trace captured when the game reset it mid-link: commands,
           events and restarts before a game-side link error. */
        t.last_trace_seq = f[1];
        ++t.trace_snapshots;
        uint8_t count = f[2];
        log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_ADAPTER_TRACE, f[1], (uint16_t)((count << 8) | f[3]));
        for (uint8_t i = 0; i < count && 4 + 4 * (size_t)i + 4 <= n; ++i)
        {
            const uint8_t *e = f + 4 + 4 * i;
            log_event(now_ms, EV_TRACE, e[0], e[1], rd16(e + 2));
        }
        printf("LDN_BRIDGE adapter was reset by the GBA mid-link; its exchange trace is in LDN_SHIM_LOG\n");
    }
    else return;
    if (!t.seen) { t.seen = true; t.last_logged = now_ms; log_counters(now_ms, true); return; }
    consider_logging(now_ms);
}

void trade_shim_bridge_counters(int64_t now_ms, uint16_t reordered, uint16_t hold_dropped, uint16_t overflow, uint16_t queued,
                                uint16_t bad_frames, uint16_t decrypt_failures, uint16_t unzip_failures,
                                uint16_t host_skips, uint16_t host_long_skips,
                                uint16_t rx_body_max, uint16_t unzip_last_error)
{
    t.rx_body_max = rx_body_max;
    t.unzip_last_error = unzip_last_error;
    t.host_skips = host_skips;
    t.host_long_skips = host_long_skips;
    t.reordered = reordered;
    t.hold_dropped = hold_dropped;
    t.overflow = overflow;
    t.bad_frames = bad_frames;
    t.decrypt_failures = decrypt_failures;
    t.unzip_failures = unzip_failures;
    if (queued > t.queued_high) t.queued_high = queued;
    consider_logging(now_ms);
}

/* Round-number mapping. The Switch runs the extra rounds, so its numbers are ahead.
   Fakes use Switch numbering. Cartridge round b maps to b plus every extra round at or
   below the result. Joiner role: Switch = parent. Lead role: Switch = child. */
static int to_ahead(int b)
{
    int a = b + s.base;
    for (int i = 0; i < s.fake_count; ++i) if (s.fakes[i] <= a) ++a;
    return a;
}
static int to_behind(int a)
{
    int b = a - s.base;
    for (int i = 0; i < s.fake_count; ++i) if (s.fakes[i] < a) --b;
    return b;
}
static bool is_fake(int p)
{
    for (int i = 0; i < s.fake_count; ++i) if (s.fakes[i] == p) return true;
    return false;
}
static void add_fake(int p)
{
    if (s.fake_count == TRADE_SHIM_MAX_FAKES)
    {
        /* Rounds only increase: fold the oldest fake into the offset. */
        memmove(s.fakes, s.fakes + 1, sizeof(s.fakes) - sizeof(s.fakes[0]));
        --s.fake_count;
        ++s.base;
    }
    s.fakes[s.fake_count++] = (uint16_t)p;
}

/* Parent UNI frame: 3-byte LLSF header (one child slot, UNI state, 70 data bytes) and
   five 14-byte slots. */
static size_t build_host_frame(uint8_t *out, uint16_t cmd0, uint16_t v0, uint16_t cmd1, uint16_t v1)
{
    memset(out, 0, 73);
    out[0] = 0x46; out[1] = 0x00; out[2] = 0x05;
    wr16(out + 3, cmd0); wr16(out + 5, v0);
    wr16(out + 17, cmd1); wr16(out + 19, v1);
    return 73;
}

static size_t build_child_frame(uint8_t *out, uint8_t tag, uint16_t round)
{
    memset(out, 0, 16);
    out[0] = 0x0e; out[1] = 0x10;                    /* child UNI frame, 14-byte slot */
    out[2] = (uint8_t)(tag << 5);
    out[3] = RFUCMD_READY_EXIT_STANDBY >> 8;
    wr16(out + 4, round);
    return 16;
}

size_t trade_shim_child(uint8_t *payload, size_t length, int64_t now_ms, uint8_t *reply, size_t reply_capacity, bool *forward)
{
    if (forward) *forward = true;
    s.clock_ms = now_ms;
    if (length < 16) return 0;
    if (((rd16(payload) >> 10) & 15) != 4) return 0;  /* not a UNI frame */
    uint8_t *slot = payload + 2;
    if (slot[1] == 0) return 0;                        /* idle: no command, no tag */
    s.quiet_since = now_ms;
    uint16_t command = (uint16_t)((slot[1] << 8) | (slot[0] & 0x1f));
    uint8_t raw_tag = (uint8_t)(slot[0] >> 5);
    uint8_t index = (uint8_t)(slot[0] & 0x1f);
    bool fragment = (command & 0xff00) == RFUCMD_SEND_BLOCK;
    s.last_command_ms = now_ms;
    if (s.have_last_frame && memcmp(payload, s.last_frame, 16) == 0)
    {
        /* Identical frame twice: the adapter repeated one GBA transfer (librfu re-runs an
           exchange whose ack it misread). Real commands always advance the tag. Dropped
           so the parent does not act twice. */
        if (forward) *forward = false;
        ++s.duplicates;
        return 0;
    }
    memcpy(s.last_frame, payload, 16);
    s.have_last_frame = true;
    /* Tags are replaced at send time (trade_shim_stamp). The parent accepts only previous
       tag + 1 and drops the child after five misses in a row. Lost frames and block
       resends break that (HandleSendFailure queues fragments untagged, as tag 0). Raw
       tags are checked here for the log only. */
    bool resend = false;
    if (s.raw_tag_valid && raw_tag != ((s.last_raw_tag + 1) & 7))
    {
        resend = raw_tag == 0 && fragment && s.cb.active && index + 1 < s.cb.count;
        if (resend)
        {
            ++s.resends_restamped;
            log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_RESTAMPED_RESEND, index, 0);
        }
        else
        {
            ++s.tag_gaps;
            log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_CHILD_TAG_GAP, s.last_raw_tag, raw_tag);
        }
    }
    if (!resend)                                       /* a resend sits between two tagged frames */
    {
        s.last_raw_tag = raw_tag;
        s.raw_tag_valid = true;
    }
    s.have_tag = true;
    if (command == RFUCMD_SEND_BLOCK_INIT)
    {
        uint16_t count = rd16(slot + 2);
        if (!s.cb.active || count != s.cb.count || s.cb.sent == 0)
        {
            s.cb.active = count >= 1 && count <= 32;
            s.cb.count = (uint8_t)count;
            s.cb.sent = s.cb.echoed = 0;
        }
    }
    else if (fragment && s.cb.active && index < s.cb.count)
    {
        memcpy(s.cb.data[index], slot + 2, 12);
        s.cb.sent |= 1u << index;
    }
    if (s.request_type >= 0 && !s.request_served)
    {
        /* Served by a block of the requested size: its INIT, or all fragments if every
           INIT was lost. Excludes the child's own 20-byte trade menu blocks and resends
           of an earlier block. */
        size_t type = (size_t)s.request_type;
        if (type >= sizeof(kRequestFragments)) s.request_served = true;
        else if (command == RFUCMD_SEND_BLOCK_INIT) s.request_served = rd16(slot + 2) == kRequestFragments[type];
        else if (fragment && index < 32)
        {
            uint32_t all = (1u << kRequestFragments[type]) - 1;
            s.request_frags |= 1u << index;
            s.request_served = (s.request_frags & all) == all;
        }
    }
    if (command != RFUCMD_READY_EXIT_STANDBY) return 0;

    int c = rd16(slot + 2), p = to_ahead(c);
    wr16(slot + 2, (uint16_t)p);
    if (p > s.last_child_round) s.last_child_round = p;
    log_event(now_ms, EV_CHILD_STANDBY, raw_tag, (uint16_t)c, (uint16_t)p);

    /* Retry of a barrier the parent already left: its answer was lost and will not be
       re-sent. Answer for the parent. A late real copy is ignored by the child, whose
       counter has moved on. */
    if (p > s.last_round || reply_capacity < 146) return 0;
    size_t n = build_host_frame(reply, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c);
    ++s.reanswers;
    log_event(now_ms, EV_REANSWER, 0, (uint16_t)c, (uint16_t)p);
    printf("LDN_BRIDGE GBA is retrying standby round %d that the Switch already answered (as %d), answering for it\n", c, p);
    /* A party request that arrived during that barrier was refused by the child's link
       layer. The parent does not repeat it. */
    if (s.request_type >= 0 && !s.request_served && s.request_attempts < TRADE_SHIM_MAX_REREQUESTS)
    {
        n += build_host_frame(reply + n, RFUCMD_SEND_BLOCK_REQ, (uint16_t)s.request_type, 0, 0);
        ++s.request_attempts;
        ++s.rerequests;
        s.request_at = now_ms;
        log_event(now_ms, EV_REREQUEST, 0, (uint16_t)s.request_type, 0);
        printf("LDN_BRIDGE repeating the Switch's party request %d to the GBA\n", s.request_type);
    }
    return n;
}

size_t trade_shim_host(uint8_t *payload, size_t length, int64_t now_ms, uint8_t *pre, size_t pre_capacity)
{
    size_t pre_len = 0;
    s.clock_ms = now_ms;
    if (length < 3) return 0;
    uint32_t header = payload[0] | (payload[1] << 8) | ((uint32_t)payload[2] << 16);
    if (((header >> 14) & 15) != 4) return 0;           /* not a UNI frame */
    size_t size = header & 0x7f;
    if (size < 28 || 3 + size > length) return 0;
    for (int i = 0; i < 2; ++i)                       /* slot 0: parent, slot 1: echo of this child */
    {
        uint8_t *slot = payload + 3 + 14 * i;
        uint16_t command = rd16(slot), value = rd16(slot + 2);
        if (command == 0) continue;
        s.quiet_since = now_ms;
        if (i == 1)
        {
            ++s.echoes;
            s.echo_ring[s.echo_head].ms = (uint32_t)now_ms;
            s.echo_ring[s.echo_head].command = command;
            s.echo_head = (s.echo_head + 1) % 16;
            if (s.echo_count < 16) ++s.echo_count;
            s.last_echo_ms = now_ms;
            s.sent_since_echo = 0;
            if (s.echo_stall_reported) incident_resolved(now_ms, 2);
            s.echo_stall_reported = false;
        }
        if (i == 1 && (command & 0xff00) == RFUCMD_SEND_BLOCK && s.cb.active)
        {
            uint8_t index = (uint8_t)(command & 0x1f);
            if (index < s.cb.count) s.cb.echoed |= 1u << index;
            if (index + 1 == s.cb.count)
            {
                /* The child acts on this echo. Unechoed earlier fragments would count as
                   send failures, so supply their echoes first. */
                uint32_t below = (1u << index) - 1;
                uint32_t missing = s.cb.sent & ~s.cb.echoed & below;
                if (missing) log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_ECHO_GAP, (uint16_t)missing, s.cb.count);
                for (uint8_t k = 0; k < index && pre_len + 73 <= pre_capacity; ++k)
                {
                    if (!(missing & (1u << k))) continue;
                    uint8_t *f = pre + pre_len;
                    memset(f, 0, 73);
                    memcpy(f, payload, 3);                     /* LLSF header of the real frame */
                    f[17] = k; f[18] = RFUCMD_SEND_BLOCK >> 8; /* slot 1: echo, tag stripped */
                    memcpy(f + 19, s.cb.data[k], 12);
                    pre_len += 73;
                    s.cb.echoed |= 1u << k;
                    ++s.echoes_synthesized;
                    log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_ECHO_SYNTHESIZED, k, s.cb.count);
                    printf("LDN_BRIDGE echo of the GBA's fragment %u never came back from the Switch, supplying it\n", k);
                }
            }
        }
        if (i == 0 && (command & 0xff00) == RFUCMD_SEND_BLOCK && s.pb.active)
        {
            /* Logging only. The parent sends each fragment once, so a gap is a fragment
               the child never receives. */
            uint8_t index = (uint8_t)(command & 0x1f);
            if (index != s.pb.expect && index + 1 != s.pb.expect)
                log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_PARENT_FRAGMENT_GAP, s.pb.expect, index);
            if (index >= s.pb.expect) s.pb.expect = (uint8_t)(index + 1);
        }
        if (i == 0)
        {
            if (command == RFUCMD_SEND_BLOCK_INIT)
            {
                if (value != s.host_block_count) log_event(now_ms, EV_HOST_BLOCK, 0, value, 0);
                s.host_block_count = value;
                if (!s.pb.active || (uint8_t)value != s.pb.count || s.pb.expect == s.pb.count)
                { s.pb.active = value >= 1 && value <= 32; s.pb.count = (uint8_t)value; s.pb.expect = 0; }
            }

            else if (command == RFUCMD_SEND_BLOCK && s.host_block_count == 2 && value == LINKCMD_CONFIRM_FINISH_TRADE)
            {
                s.post_trade = true;
                s.injected = false;
                s.answers_since_confirm = 0;
                s.fake_round = -1;
                s.fake_acked = false;
                s.attempts = 0;
                ++s.trades;
                log_event(now_ms, EV_CONFIRM, 0, (uint16_t)s.trades, (uint16_t)(s.last_round + 1));
                printf("LDN_BRIDGE trade %d confirmed, watching the post-trade standby rounds\n", s.trades);
            }
            else if (command == 0xed00 || command == 0xee00)
            {
                log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_PARENT_DISCONNECT_CMD, command, value);
                printf("LDN_BRIDGE Switch sent a disconnect command %04x\n", command);
            }
            else if (command == RFUCMD_SEND_BLOCK_REQ)
            {
                s.request_type = value;
                s.request_served = false;
                s.request_at = now_ms;
                s.request_attempts = 0;
                s.request_frags = 0;
                log_event(now_ms, EV_REQUEST, s.post_trade ? 1 : 0, value, 0);
                if (s.post_trade)
                {
                    s.post_trade = false;
                    s.fake_acked = true;              /* parent has moved on */
                    printf("LDN_BRIDGE Switch is requesting party data again\n");
                }
            }
        }
        if (command != RFUCMD_READY_EXIT_STANDBY) continue;
        if (is_fake(value))
        {
            /* Parent's half of an injected round. Hidden from the child: a matching
               number would pre-arm its next barrier. */
            if (i == 0 && value == s.fake_round && !s.fake_acked)
            {
                s.fake_acked = true;
                log_event(now_ms, EV_ACK, 0, value, 0);
                printf("LDN_BRIDGE Switch answered the supplied round %d\n", value);
            }
            log_event(now_ms, EV_PARENT_STANDBY, (uint8_t)i, value, 0xffff);
            memset(slot, 0, 4);
            continue;
        }
        if (i == 0 && (int)value > s.last_round)
        {
            s.last_round = value;
            if (s.post_trade) ++s.answers_since_confirm;
        }
        uint16_t child_value = (uint16_t)to_behind(value);
        wr16(slot + 2, child_value);
        log_event(now_ms, EV_PARENT_STANDBY, (uint8_t)i, value, child_value);
    }
    return pre_len;
}

size_t trade_shim_inject(int64_t now_ms, uint8_t *out, size_t capacity)
{
    if (capacity < 16 || !s.post_trade || !s.have_tag) return 0;
    int64_t quiet = now_ms - s.quiet_since;
    if (!s.injected)
    {
        bool due = (s.answers_since_confirm >= TRADE_SHIM_CHILD_ROUNDS && quiet >= TRADE_SHIM_SETTLE_MS)
                || (s.answers_since_confirm >= 1 && quiet >= TRADE_SHIM_FALLBACK_MS);
        if (!due) return 0;
        int next = s.last_round > s.last_child_round ? s.last_round : s.last_child_round;
        s.fake_round = next + 1;
        add_fake(s.fake_round);
        s.injected = true;
        s.fake_acked = false;
        s.attempts = 1;
        s.injected_at = now_ms;
        s.quiet_since = now_ms;
        ++s.injections;
        log_event(now_ms, EV_INJECT, 0, (uint16_t)s.fake_round, (uint16_t)s.answers_since_confirm);
        printf("LDN_BRIDGE supplied standby round %d on the GBA's behalf after %d answered rounds\n",
               s.fake_round, s.answers_since_confirm);
        return build_child_frame(out, 0, (uint16_t)s.fake_round);
    }
    if (s.fake_acked || now_ms - s.injected_at < TRADE_SHIM_ACK_MS) return 0;
    if (s.attempts >= TRADE_SHIM_MAX_ATTEMPTS)
    {
        if (s.give_ups == s.injections - 1) { ++s.give_ups; log_event(now_ms, EV_GIVE_UP, 0, (uint16_t)s.fake_round, 0); }
        return 0;
    }
    /* Re-send the round. If the first copy was seen and the answer is just late, this
       copy arrives after the round and is ignored as a stale number. */
    ++s.attempts;
    s.injected_at = now_ms;
    log_event(now_ms, EV_INJECT, 0, (uint16_t)s.fake_round, (uint16_t)(0x100 | s.attempts));
    printf("LDN_BRIDGE no answer to the supplied round %d, sending it again (attempt %d)\n", s.fake_round, s.attempts);
    return build_child_frame(out, 0, (uint16_t)s.fake_round);
}

size_t trade_shim_host_inject(int64_t now_ms, uint8_t *out, size_t capacity)
{
    if (capacity < 73 || s.request_type < 0 || s.request_served) return 0;
    if (s.request_attempts >= TRADE_SHIM_MAX_REREQUESTS || now_ms - s.request_at < TRADE_SHIM_REREQUEST_MS
        || now_ms - s.last_command_ms < TRADE_SHIM_REREQUEST_MS) return 0;
    /* The child's link layer refused the request (previous block send still open, e.g.
       after a lost echo, or another command pending). The parent asks once. A repeat
       while still busy is refused the same way. */
    ++s.request_attempts;
    ++s.rerequests;
    s.request_at = now_ms;
    log_event(now_ms, EV_REREQUEST, (uint8_t)s.request_attempts, (uint16_t)s.request_type, 0);
    printf("LDN_BRIDGE GBA did not answer the Switch's block request %d, repeating it (attempt %d)\n",
           s.request_type, s.request_attempts);
    return build_host_frame(out, RFUCMD_SEND_BLOCK_REQ, (uint16_t)s.request_type, 0, 0);
}

/* ---- lead role: GBA is parent, Switch is child ------------------------------------ */

size_t trade_shim_lead_child(uint8_t *payload, size_t length, int64_t now_ms, uint8_t *reply, size_t reply_capacity, bool *forward)
{
    if (forward) *forward = true;
    s.clock_ms = now_ms;
    if (length < 16) return 0;
    if (((rd16(payload) >> 10) & 15) != 4) return 0;  /* not a UNI frame */
    uint8_t *slot = payload + 2;
    if (slot[1] == 0) return 0;                        /* idle: no command */
    s.quiet_since = s.last_command_ms = now_ms;
    s.have_tag = true;
    uint16_t command = (uint16_t)((slot[1] << 8) | (slot[0] & 0x1f));
    uint8_t index = (uint8_t)(slot[0] & 0x1f);
    bool fragment = (command & 0xff00) == RFUCMD_SEND_BLOCK;
    if (s.request_type >= 0 && !s.request_served)
    {
        size_t type = (size_t)s.request_type;
        if (command == RFUCMD_SEND_BLOCK_INIT || fragment) s.request_answering = true;
        if (type >= sizeof(kRequestFragments)) s.request_served = true;
        else if (command == RFUCMD_SEND_BLOCK_INIT) s.request_served = rd16(slot + 2) == kRequestFragments[type];
        else if (fragment && index < 32)
        {
            uint32_t all = (1u << kRequestFragments[type]) - 1;
            s.request_frags |= 1u << index;
            s.request_served = (s.request_frags & all) == all;
        }
    }
    if (command != RFUCMD_READY_EXIT_STANDBY || reply_capacity < 146) return 0;

    /* Switch frames keep their own tags, which the GBA requires consecutive, so none is
       dropped. A frame the GBA must ignore gets round number 0xffff. */
    int c = rd16(slot + 2);
    if (is_fake(c))
    {
        /* Extra round retried: the answer has not reached the Switch yet. */
        wr16(slot + 2, 0xffff);
        log_event(now_ms, EV_CHILD_STANDBY, 1, (uint16_t)c, 0xffff);
        return build_host_frame(reply, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c);
    }
    int p = to_behind(c);
    log_event(now_ms, EV_CHILD_STANDBY, 0, (uint16_t)c, (uint16_t)p);
    size_t n = 0;
    if (p > s.last_round)
    {
        /* Past the GBA's newest barrier. After the cartridge's post-trade rounds this is
           the Switch's extra round, which the GBA never answers. Answered here and
           forwarded as 0xffff, since the GBA's barrier of that number is still ahead. */
        wr16(slot + 2, (uint16_t)p);
        if (!s.post_trade || s.injected || s.answers_since_confirm < TRADE_SHIM_CHILD_ROUNDS) return 0;
        add_fake(c);
        s.fake_round = c;
        s.injected = true;
        s.post_trade = false;
        ++s.injections;
        if (s.hold) s.release_at = now_ms + TRADE_SHIM_RELEASE_MS;
        wr16(slot + 2, 0xffff);
        log_event(now_ms, EV_INJECT, 1, (uint16_t)c, (uint16_t)s.answers_since_confirm);
        printf("LDN_BRIDGE answered the Switch's extra standby round %d for the GBA after %d rounds\n", c, s.answers_since_confirm);
        n = build_host_frame(reply, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c);
    }
    else
    {
        /* Retry of a barrier the GBA already left: its answer was lost. Answer for it. */
        wr16(slot + 2, (uint16_t)p);
        ++s.reanswers;
        log_event(now_ms, EV_REANSWER, 1, (uint16_t)c, (uint16_t)p);
        printf("LDN_BRIDGE Switch is retrying standby round %d that the GBA already answered (as %d), answering for it\n", c, p);
        n = build_host_frame(reply, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c, RFUCMD_READY_EXIT_STANDBY, (uint16_t)c);
    }
    /* A party request refused during that barrier is repeated by
       trade_shim_lead_inject. The GBA does not repeat it. */
    return n;
}

void trade_shim_lead_parent(uint8_t *payload, size_t length, int64_t now_ms)
{
    s.clock_ms = now_ms;
    if (length < 3) return;
    uint32_t header = payload[0] | (payload[1] << 8) | ((uint32_t)payload[2] << 16);
    if (((header >> 14) & 15) != 4) return;             /* not a UNI frame */
    size_t size = header & 0x7f;
    if (size < 28 || 3 + size > length) return;
    for (int i = 0; i < 2; ++i)                       /* slot 0: GBA, slot 1: echo of the Switch */
    {
        uint8_t *slot = payload + 3 + 14 * i;
        uint16_t command = rd16(slot), value = rd16(slot + 2);
        if (command == 0) continue;
        s.quiet_since = now_ms;
        if (i == 0 && (command & 0xff00) == RFUCMD_SEND_BLOCK && s.pb.active)
        {
            uint8_t index = (uint8_t)(command & 0x1f);
            if (index < s.pb.count) { memcpy(s.pb.data[index], slot + 2, 12); s.pb.have |= 1u << index; }
        }
        if (i == 0)
        {
            if (command == RFUCMD_SEND_BLOCK_INIT)
            {
                if (value != s.host_block_count) log_event(now_ms, EV_HOST_BLOCK, 0, value, 0);
                s.host_block_count = value;
                s.pb.active = value >= 1 && value <= 32;
                s.pb.count = (uint8_t)value;
                s.pb.have = 0;
            }
            if (s.hold && (command == RFUCMD_SEND_BLOCK_REQ || command == RFUCMD_SEND_BLOCK_INIT || (command & 0xff00) == RFUCMD_SEND_BLOCK))
            {
                /* GBA past its last barrier, Switch still in its extra one. A request now
                   would arrive before the Switch buffers its party and the block would be
                   cleared. Held here, released by trade_shim_lead_inject. */
                if (command == RFUCMD_SEND_BLOCK_REQ)
                {
                    s.request_type = value;
                    s.request_served = s.request_answering = false;
                    s.request_attempts = 0;
                    s.request_frags = 0;
                    log_event(now_ms, EV_REQUEST, 2, value, 0);
                }
                memset(slot, 0, 14);
                continue;
            }
            else if (command == RFUCMD_SEND_BLOCK && s.host_block_count == 2 && value == LINKCMD_CONFIRM_FINISH_TRADE)
            {
                s.post_trade = true;
                s.injected = false;
                s.answers_since_confirm = 0;
                s.fake_round = -1;
                s.hold = false;
                s.release_at = 0;
                ++s.trades;
                log_event(now_ms, EV_CONFIRM, 1, (uint16_t)s.trades, (uint16_t)(s.last_round + 1));
                printf("LDN_BRIDGE trade %d confirmed, watching the post-trade standby rounds\n", s.trades);
            }
            else if (command == RFUCMD_SEND_BLOCK_REQ)
            {
                s.request_type = value;
                s.request_served = s.request_answering = false;
                s.request_at = now_ms;
                s.request_attempts = 0;
                s.request_frags = 0;
                log_event(now_ms, EV_REQUEST, s.post_trade ? 1 : 0, value, 0);
            }
        }
        if (command != RFUCMD_READY_EXIT_STANDBY) continue;
        if (i == 0 && (int)value > s.last_round)
        {
            s.last_round = value;
            if (s.post_trade && ++s.answers_since_confirm == TRADE_SHIM_CHILD_ROUNDS && !s.hold)
            {
                s.hold = true;
                s.hold_since = now_ms;
                s.release_at = 0;
            }
        }
        uint16_t child_value = (uint16_t)to_ahead(value);
        wr16(slot + 2, child_value);
        log_event(now_ms, EV_PARENT_STANDBY, (uint8_t)(2 + i), value, child_value);
    }
}

/* Replay order: GBA's block first, request last. The Switch answers the request at once
   with its own block, whose echoes would otherwise crowd out the replayed fragments. */
static size_t lead_release_frame(uint8_t *out)
{
    int k = s.replay_index++;
    if (k == s.replay_count - 1) return build_host_frame(out, RFUCMD_SEND_BLOCK_REQ, (uint16_t)s.request_type, 0, 0);
    if (k == 0) return build_host_frame(out, RFUCMD_SEND_BLOCK_INIT, s.pb.count, 0x80, 0);
    uint8_t index = (uint8_t)(k - 1);
    memset(out, 0, 73);
    out[0] = 0x46; out[1] = 0x00; out[2] = 0x05;
    wr16(out + 3, (uint16_t)(RFUCMD_SEND_BLOCK | index));
    memcpy(out + 5, s.pb.data[index], 12);
    return 73;
}

static bool parent_block_whole(void)
{
    return s.pb.active && s.pb.count > 0 && s.pb.have == (s.pb.count == 32 ? 0xffffffffu : (1u << s.pb.count) - 1);
}

size_t trade_shim_lead_inject(int64_t now_ms, uint8_t *out, size_t capacity)
{
    if (capacity < 73) return 0;
    if (s.replay_index < s.replay_count)
    {
        size_t n = lead_release_frame(out);
        if (s.replay_index == s.replay_count && s.hold) { s.hold = false; s.request_at = now_ms; }
        return n;
    }
    if (s.hold)
    {
        /* Release after release_at, or after the timeout if no extra round came. Block
           and request if the block is whole, else the request alone. */
        const bool timeout = now_ms - s.hold_since > TRADE_SHIM_HOLD_TIMEOUT_MS;
        if (s.request_type < 0) { if (timeout) s.hold = false; return 0; }
        const bool due = (s.release_at && now_ms >= s.release_at) || timeout;
        if (!due) return 0;
        if (!parent_block_whole() && !timeout && now_ms < s.release_at + 500) return 0;
        s.replay_count = parent_block_whole() ? 2 + s.pb.count : 1;
        s.replay_index = 0;
        s.release_at = 0;
        log_event(now_ms, EV_REREQUEST, 0x20, (uint16_t)s.request_type, (uint16_t)s.replay_count);
        printf("LDN_BRIDGE releasing the GBA's %sparty request %d to the Switch\n",
               s.replay_count > 1 ? "block and " : "", s.request_type);
        size_t n = lead_release_frame(out);
        if (s.replay_index == s.replay_count) { s.hold = false; s.request_at = now_ms; }
        return n;
    }
    if (s.request_type < 0 || s.request_served || s.request_answering) return 0;
    if (s.request_attempts >= TRADE_SHIM_MAX_REREQUESTS || now_ms - s.request_at < TRADE_SHIM_LEAD_REREQUEST_MS
        || now_ms - s.last_command_ms < TRADE_SHIM_REREQUEST_MS) return 0;
    /* Request refused by the Switch's link layer (barrier still open). The GBA asks once.
       Request only: the block already reached the Switch, and a second copy would cut
       into the answer. */
    ++s.request_attempts;
    ++s.rerequests;
    s.request_at = now_ms;
    s.replay_count = 1;
    s.replay_index = 0;
    log_event(now_ms, EV_REREQUEST, (uint8_t)(0x10 | s.request_attempts), (uint16_t)s.request_type, 1);
    printf("LDN_BRIDGE Switch has not answered the GBA's party request %d, repeating it (attempt %d)\n",
           s.request_type, s.request_attempts);
    return lead_release_frame(out);
}

void trade_shim_adapter_counts(uint8_t *queue_high, uint16_t *queue_drops, uint8_t *sheds, uint16_t *uart_overflows)
{
    *queue_high = t.fifo_high;
    *queue_drops = t.fifo_drops;
    *sheds = t.sheds;
    *uart_overflows = t.uart_overflows;
}

/* Lead role: own slot and echo slot each empty, no key or a direction; no other slots
   in use. */
bool trade_shim_parent_sheddable(const uint8_t *payload, size_t length)
{
    if (length < 3 + 14 * 2) return false;
    uint32_t header = payload[0] | (payload[1] << 8) | ((uint32_t)payload[2] << 16);
    if (((header >> 14) & 15) != 4) return false;
    for (size_t o = 3; o + 14 <= length; o += 14)
    {
        const uint8_t *slot = payload + o;
        if (slot[1] == 0 && slot[0] == 0) continue;
        if (o >= 3 + 14 * 2 || slot[1] != (RFUCMD_SEND_HELD_KEYS >> 8)) return false;
        const uint8_t key = slot[2];
        if (!(key == 0 || (key >= LINK_KEY_CODE_EMPTY && key <= LINK_KEY_CODE_DPAD_RIGHT))) return false;
    }
    return true;
}

bool trade_shim_sheddable(const uint8_t *payload, size_t length)
{
    if (length < 6 || ((rd16(payload) >> 10) & 15) != 4) return false;
    if (payload[3] == 0) return true;   /* no command */
    if (payload[3] != (RFUCMD_SEND_HELD_KEYS >> 8)) return false;
    /* No key or a direction: superseded by the next report. Buttons (A, READY,
       EXIT_ROOM) are one-shot and must arrive. */
    const uint8_t key = payload[4];
    return key == 0 || (key >= LINK_KEY_CODE_EMPTY && key <= LINK_KEY_CODE_DPAD_RIGHT);
}

void trade_shim_stamp(uint8_t *payload, size_t length)
{
    if (length < 4 || ((rd16(payload) >> 10) & 15) != 4 || payload[3] == 0) return;   /* idle frames carry no tag */
    payload[2] = (uint8_t)((s.send_tag << 5) | (payload[2] & 0x1f));
    s.sent_ring[s.sent_head].ms = (uint32_t)s.clock_ms;
    s.sent_ring[s.sent_head].command = (uint16_t)((payload[3] << 8) | (payload[2] & 0x1f));
    s.sent_ring[s.sent_head].tag = s.send_tag;
    s.sent_head = (s.sent_head + 1) % 16;
    if (s.sent_count < 16) ++s.sent_count;
    s.send_tag = (uint8_t)((s.send_tag + 1) & 7);
    ++s.child_commands;
    ++s.sent_since_echo;
}

static void log_rings(void)
{
    for (int i = 0; i < s.sent_count; ++i)
    {
        int at = (s.sent_head + 16 - s.sent_count + i) % 16;
        log_event_at(s.sent_ring[at].ms, EV_SENT, s.sent_ring[at].tag, s.sent_ring[at].command, 0);
    }
    for (int i = 0; i < s.echo_count; ++i)
    {
        int at = (s.echo_head + 16 - s.echo_count + i) % 16;
        log_event_at(s.echo_ring[at].ms, EV_ECHO, 0, s.echo_ring[at].command, 0);
    }
}

static uint16_t clamp16(int64_t v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }

void trade_shim_poll(int64_t now_ms)
{
    s.clock_ms = now_ms;
    /* Commands sent but none echoed: the parent stopped accepting child frames (rejected
       tag sequence or its own link error). */
    if (!s.echo_stall_reported && s.sent_since_echo >= 6 && s.last_echo_ms && now_ms - s.last_echo_ms >= 2000)
    {
        s.echo_stall_reported = true;
        bool fresh = incident_begin();
        if (fresh) incident_reason = 2;
        log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_ECHO_STALL, (uint16_t)s.sent_since_echo, clamp16(now_ms - s.last_echo_ms));
        log_rings();
        if (fresh) incident_end();
        printf("LDN_BRIDGE Switch stopped echoing the GBA's commands (%d unechoed)\n", s.sent_since_echo);
    }
}

void trade_shim_pia_recovered(int64_t now_ms) { incident_resolved(now_ms, 1); }

void trade_shim_pia_stall(int64_t now_ms, const trade_shim_pia_t *p)
{
    bool fresh = incident_begin();
    if (fresh) incident_reason = 1;
    log_event(now_ms, EV_NOTE, TRADE_SHIM_NOTE_PIA_STALL, p->our_pending, clamp16(p->stalled_ms));
    log_event(now_ms, EV_PIA, 1, p->our_low, p->our_next);
    log_event(now_ms, EV_PIA, 2, p->peer_low, clamp16(p->peer_low_age_ms));
    log_event(now_ms, EV_PIA, 3, p->peer_ack_next, clamp16(p->peer_ack_age_ms));
    log_event(now_ms, EV_PIA, 4, clamp16(p->peer_ack_seen_age_ms), p->receive_next);
    log_event(now_ms, EV_PIA, 5, p->out_of_order, p->credits);
    log_event(now_ms, EV_PIA, 6, p->k_queued, p->k_inflight);
    log_event(now_ms, EV_PIA, 7, p->out_queued, clamp16(p->overflow));
    log_event(now_ms, EV_PIA, 8, clamp16(p->idle_evicted), s.send_tag);
    log_rings();
    if (fresh) incident_end();
    printf("LDN_BRIDGE Switch has not acknowledged our frames for %d ms\n", (int)p->stalled_ms);
}

void trade_shim_status(char *out, size_t capacity)
{
    snprintf(out, capacity, "shim_trades=%d shim_injected=%d shim_reanswers=%d shim_rerequests=%d shim_offset=%d shim_post_trade=%d shim_last_round=%d shim_answers=%d shim_acked=%d shim_echoes=%d shim_restamps=%d shim_tag_gaps=%d shim_commands=%u shim_echoed=%u shim_dups=%d shim_unechoed=%d",
             s.trades, s.injections, s.reanswers, s.rerequests, s.base + s.fake_count, s.post_trade ? 1 : 0, s.last_round,
             s.answers_since_confirm, s.fake_acked ? 1 : 0, s.echoes_synthesized, s.resends_restamped, s.tag_gaps,
             s.child_commands, s.echoes, s.duplicates, s.sent_since_echo);
}

static const char *const kEventNames[] = {
    "?", "BOOT", "RESET", "CONFIRM", "PARENT_STANDBY", "CHILD_STANDBY", "REQUEST", "INJECT",
    "ACK", "REANSWER", "REREQUEST", "NOTE", "HOST_BLOCK", "GIVE_UP", "ADAPTER", "BRIDGE", "TRACE",
    "SENT", "ECHO", "PIA" };
static const char *const kNoteNames[] = { "?", "bridge start", "child connect", "child disconnect", "host disconnect", "bridge stop", "host silence ms", "parent disconnect cmd", "child tag gap prev/now", "switch clock skip frames/ms", "echo synthesized idx/count", "parent fragment gap expect/got", "echo gap mask/count", "restamped resend idx/tag", "adapter trace seq count<<8|restarts", "unzip failure error/wire bytes", "echo stall unechoed/ms", "pia stall pending/ms", "stall recovered pia=1/echo=2", "switch net request type/seq", "switch net request NOT acknowledged type/copies" };
static const char *const kTraceKinds[] = { "?", "cmd", "event", "restart_in_state", "delivery", "sd_reset_in_state", "login" };

static void format_entry(const log_entry_t *e, char *line, size_t cap)
{
    const char *name = e->type < sizeof(kEventNames) / sizeof(kEventNames[0]) ? kEventNames[e->type] : "?";
    double t = e->ms / 1000.0;
    switch (e->type)
    {
    case EV_BOOT: snprintf(line, cap, "  %9.3f %s reset_reason=%u boot=%u", t, name, e->a, e->b); break;
    case EV_CONFIRM: snprintf(line, cap, "  %9.3f %s trade=%u parent_count=%u", t, name, e->b, e->c); break;
    case EV_PARENT_STANDBY:
        if (e->c == 0xffff) snprintf(line, cap, "  %9.3f %s slot=%u round=%u suppressed", t, name, e->a, e->b);
        else snprintf(line, cap, "  %9.3f %s slot=%u round=%u to_gba=%u", t, name, e->a, e->b, e->c);
        break;
    case EV_CHILD_STANDBY: snprintf(line, cap, "  %9.3f %s round=%u to_switch=%u raw_tag=%u", t, name, e->b, e->c, e->a); break;
    case EV_REQUEST: snprintf(line, cap, "  %9.3f %s type=%u%s", t, name, e->b, e->a ? " (post-trade watch ends)" : ""); break;
    case EV_INJECT: snprintf(line, cap, "  %9.3f %s round=%u %s%u", t, name, e->b, (e->c & 0x100) ? "attempt=" : "after_answers=", e->c & 0xff); break;
    case EV_SENT: snprintf(line, cap, "  %9.3f %s tag=%u command=%04x", t, name, e->a, e->b); break;
    case EV_ECHO: snprintf(line, cap, "  %9.3f %s command=%04x", t, name, e->b); break;
    case EV_PIA:
        switch (e->a)
        {
        case 1: snprintf(line, cap, "  %9.3f %s our_low=%04x our_next=%04x", t, name, e->b, e->c); break;
        case 2: snprintf(line, cap, "  %9.3f %s switch_low=%04x unchanged_ms=%u", t, name, e->b, e->c); break;
        case 3: snprintf(line, cap, "  %9.3f %s switch_ack_next=%04x unchanged_ms=%u", t, name, e->b, e->c); break;
        case 4: snprintf(line, cap, "  %9.3f %s switch_ack_seen_ms_ago=%u our_receive_next=%04x", t, name, e->b, e->c); break;
        case 5: snprintf(line, cap, "  %9.3f %s out_of_order=%u credits=%u", t, name, e->b, e->c); break;
        case 6: snprintf(line, cap, "  %9.3f %s k_queued=%u k_inflight=%u", t, name, e->b, e->c); break;
        case 7: snprintf(line, cap, "  %9.3f %s out_queued=%u overflow=%u", t, name, e->b, e->c); break;
        default: snprintf(line, cap, "  %9.3f %s idle_evicted=%u next_tag=%u", t, name, e->b, e->c); break;
        }
        break;
    case EV_ACK: case EV_GIVE_UP: snprintf(line, cap, "  %9.3f %s round=%u", t, name, e->b); break;
    case EV_REANSWER: snprintf(line, cap, "  %9.3f %s gba_round=%u switch_round=%u", t, name, e->b, e->c); break;
    case EV_REREQUEST:
        if (e->a) snprintf(line, cap, "  %9.3f %s type=%u attempt=%u", t, name, e->b, e->a);
        else snprintf(line, cap, "  %9.3f %s type=%u (with a re-answer)", t, name, e->b);
        break;
    case EV_NOTE: snprintf(line, cap, "  %9.3f %s %s %u %u", t, name,
                           e->a < sizeof(kNoteNames) / sizeof(kNoteNames[0]) ? kNoteNames[e->a] : "?", e->b, e->c); break;
    case EV_HOST_BLOCK: snprintf(line, cap, "  %9.3f %s fragments=%u", t, name, e->b); break;
    case EV_ADAPTER:
    {
        const char *why = (e->a & 1) ? "changed" : "periodic";
        switch (e->a & 0xf0)
        {
        case 0x10: snprintf(line, cap, "  %9.3f %s %s park_drops=%u uart_overflows=%u", t, name, why, e->b, e->c); break;
        case 0x20: snprintf(line, cap, "  %9.3f %s %s fifo_drops=%u fifo_high=%u park_high=%u", t, name, why, e->b, e->c & 0xff, e->c >> 8); break;
        case 0x30: snprintf(line, cap, "  %9.3f %s %s delivery_aborts=%u delivery_retries=%u", t, name, why, e->b, e->c); break;
        case 0x40: snprintf(line, cap, "  %9.3f %s %s sd_resets=%u deliv_fail=%02x err_responses=%u login_restarts=%u", t, name, why, e->b & 0xff, e->b >> 8, e->c & 0xff, e->c >> 8); break;
        case 0x50: snprintf(line, cap, "  %9.3f %s %s park_idle_drops=%u comstate=%u", t, name, why, e->b, e->c); break;
        case 0x60: snprintf(line, cap, "  %9.3f %s %s events timeo=%u disc=%u data=%u rtx=%u", t, name, why, e->b & 0xff, e->b >> 8, e->c & 0xff, e->c >> 8); break;
        case 0x70: snprintf(line, cap, "  %9.3f %s %s link_pwr_zero=%u wipes evict/netdisc=%02x hoststart/cmddisc=%02x reset/occupancy=%02x", t, name, why, e->b & 0xff, e->b >> 8, e->c & 0xff, e->c >> 8); break;
        case 0x80: snprintf(line, cap, "  %9.3f %s %s gba_cmd_ring[0..3]=%02x %02x %02x %02x", t, name, why, e->b & 0xff, e->b >> 8, e->c & 0xff, e->c >> 8); break;
        case 0x90: snprintf(line, cap, "  %9.3f %s %s gba_cmd_ring[4..7]=%02x %02x %02x %02x", t, name, why, e->b & 0xff, e->b >> 8, e->c & 0xff, e->c >> 8); break;
        case 0xa0: snprintf(line, cap, "  %9.3f %s %s last_cmd=%02x plen=%u commands=%u", t, name, why, e->b & 0xff, e->b >> 8, e->c); break;
        case 0xc0: snprintf(line, cap, "  %9.3f %s %s gba_restarts=%u trace_snapshots=%u", t, name, why, e->b, e->c); break;
        default:   snprintf(line, cap, "  %9.3f %s %s out_ring_drops=%u bridge_queue_high=%u", t, name, why, e->b, e->c); break;
        }
        break;
    }
    case EV_TRACE:
        snprintf(line, cap, "  %9.3f %s +%ums %s %02x", t, name, e->c,
                 e->a < sizeof(kTraceKinds) / sizeof(kTraceKinds[0]) ? kTraceKinds[e->a] : "?", e->b);
        break;
    case EV_BRIDGE:
    {
        const char *why = (e->a & 1) ? "changed" : "periodic";
        switch (e->a & 0xfe)
        {
        case 2: snprintf(line, cap, "  %9.3f %s %s bad_frames=%u decrypt_failures=%u", t, name, why, e->b, e->c); break;
        case 4: snprintf(line, cap, "  %9.3f %s %s host_clock_skips single=%u multi=%u", t, name, why, e->b, e->c); break;
        case 6: snprintf(line, cap, "  %9.3f %s %s child_commands=%u echoed=%u", t, name, why, e->b, e->c); break;
        case 8: snprintf(line, cap, "  %9.3f %s %s rx_body_max=%u unzip_last_error=%u", t, name, why, e->b, e->c); break;
        case 10: snprintf(line, cap, "  %9.3f %s %s outbound_overflow=%u unzip_failures=%u", t, name, why, e->b, e->c); break;
        default: snprintf(line, cap, "  %9.3f %s %s hold_dropped=%u reordered=%u", t, name, why, e->b, e->c); break;
        }
        break;
    }
    default: snprintf(line, cap, "  %9.3f %s %u %u %u", t, name, e->a, e->b, e->c); break;
    }
}

static struct { int phase, index, head, count; } dump;

void trade_shim_dump_begin(void) { dump.phase = 0; dump.index = 0; }

bool trade_shim_dump_step(void (*emit)(const char *line), int max_lines)
{
    char line[128];
    for (; max_lines > 0; --max_lines)
    {
        switch (dump.phase)
        {
        case 0:
            if (rtc_log.magic != LOG_MAGIC) { emit("LDN_SHIM_LOG empty"); dump.phase = 4; return false; }
            if (rtc_log.incident_count)
            {
                snprintf(line, sizeof(line), "LDN_SHIM_INCIDENT entries=%u boot=%u", rtc_log.incident_count, rtc_log.incident_boot);
                emit(line);
                dump.phase = 1;
            }
            else dump.phase = 2;
            dump.index = 0;
            break;
        case 1:
            if (dump.index >= rtc_log.incident_count) { dump.phase = 2; dump.index = 0; break; }
            format_entry(&rtc_log.incident[dump.index++], line, sizeof(line));
            emit(line);
            break;
        case 2:
            snprintf(line, sizeof(line), "LDN_SHIM_LOG entries=%u boots=%u", rtc_log.count, rtc_log.boots);
            emit(line);
            dump.head = rtc_log.head;
            dump.count = rtc_log.count;
            dump.index = 0;
            dump.phase = 3;
            break;
        case 3:
            if (dump.index >= dump.count)
            {
                rtc_log.incident_count = 0;   /* read: free for the next stall */
                incident_reason = 0;
                emit("LDN_SHIM_LOG end");
                dump.phase = 4;
                return false;
            }
            format_entry(&rtc_log.entries[(dump.head + dump.index++) % LOG_SLOTS], line, sizeof(line));
            emit(line);
            break;
        default:
            return false;
        }
    }
    return true;
}
