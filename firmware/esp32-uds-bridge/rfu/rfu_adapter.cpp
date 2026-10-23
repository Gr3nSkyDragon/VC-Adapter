// The GBA Wireless Adapter on this board's link port pins, for the Switch side (rfu_adapter.h). A port of GBLink-Firmware's
// RfuProtocolSection (src/sections/rfuProtocolSection.cpp) and its PIO link layer (src/layers/linkLayer_rfu.c), v2.2.7, to the
// ESP32-S3; the protocol itself is their RfuCore, unchanged (rfuProtocol.hpp). GPL-3.0, as both.
//
// The Pico meets the GBA's 2 MHz SIO32 clock with PIO. Here the second core does it in software: while the adapter is on, the
// link task owns that core with interrupts masked and follows SC through the dedicated GPIO instructions (one CPU cycle per
// read or write; the ordinary GPIO registers take ~400 ns over APB, more than half a 2 MHz bit). One task does everything the
// Pico splits between its PIO interrupt and its section thread, so RfuCore needs no lock; the other core (Wi-Fi, the Pia
// session) reaches it only through two byte rings.
//
// What the software cannot do is listen while it works: the Pico's state machine catches a word the GBA starts at any time.
// librfu helps. After every word of a command the GBA waits in a handshake (handshake_wait(1)/(0), librfu_intr.c) for the
// adapter's SI to answer its SO, and it is held there until this side mirrors again, so that is when the work is done:
// network frames, the core's tick. Only a command's first word arrives unannounced, and in the SIO32 ID exchange no word is
// announced; nothing else runs then. While the GBA waits for an event (comWaitEvent) it does not clock at all.

#include "rfu_adapter.h"

#include <atomic>
#include <cstring>
#include <new>

#include "sdkconfig.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "driver/gpio_filter.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"

#include "rfuProtocol.hpp"

// The link loop and its helpers in IRAM (CONFIG_UDS_RFU_GLUE_IRAM); listen and masterExchange always are.
#if CONFIG_UDS_RFU_GLUE_IRAM
#define GLUE_IRAM IRAM_ATTR
#else
#define GLUE_IRAM
#endif

#if !CONFIG_UDS_LINK_PORT
#error "CONFIG_UDS_RFU_ADAPTER drives the link port pins (CONFIG_UDS_LINK_PORT)"
#endif

// The shared VC-Adapter pinout, named from this board's side: SI takes the console's SO, SO feeds the console's SI.
#define PIN_SC CONFIG_UDS_LINK_PORT_SC_GPIO
#define PIN_RX CONFIG_UDS_LINK_PORT_SI_GPIO
#define PIN_TX CONFIG_UDS_LINK_PORT_SO_GPIO
#define PIN_SD CONFIG_UDS_LINK_PORT_SD_GPIO
#define PIN_LED CONFIG_UDS_RFU_STATUS_LED_GPIO

static constexpr uint32_t kMhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
static constexpr uint32_t us(uint32_t n) { return n * kMhz; }
static constexpr uint32_t ns(uint32_t n) { return n * kMhz / 1000; }

//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Wire timing (CPU cycles).
//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//

// SC is ignored for this long after each edge the GBA drives (linkLayer_rfu.c's [12]/[6] delays): both ends change their data
// line just after the falling edge, which couples onto SC, and a ringing edge crosses the threshold more than once. Half a
// 2 MHz bit is 238 ns.
static constexpr uint32_t kBlankFall = ns(100);
static constexpr uint32_t kBlankRise = ns(50);
// The last bit is held past the GBA's sample edge (the two trailing nops of the PIO program).
static constexpr uint32_t kHoldLast = ns(500);
// SC that stops mid-word: the word is lost (a glitch counted as a clock, or the GBA gave up). Half a 256 kHz bit is 1.9 us.
static constexpr uint32_t kMidWord = us(12);
// Adapter-master clock, as the Pico's (15 PIO cycles of 434 ns a bit, ~154 kHz): SC low with the SI bit, SO sampled 2.2 us in,
// SC high 3 us in, held high 3.5 us. The GBA as slave is edge-driven and takes any rate.
static constexpr uint32_t kMasterSample = ns(2170);
static constexpr uint32_t kMasterLow = ns(870);
static constexpr uint32_t kMasterHigh = ns(3470);

// SD carries the game's AgbRFU_SoftReset pulse, ~1.1 ms high. It counts once it has fallen again, so a line that idles high
// (the level shifter's pull-up) is not a reset. The adapter starts over 60 us after it (RFU_SD_SETTLE_US).
static constexpr uint32_t kSdPulseMin = us(400);
static constexpr uint32_t kSdPulseMax = us(3000);
static constexpr uint32_t kSdSettle = us(60);

// How long one wait for SC lasts before the loop looks at anything else, while the GBA is master and while it waits for an
// event.
static constexpr uint32_t kListenBudget = us(100);
static constexpr uint32_t kWaitListenBudget = us(20);

// rfuProtocolSection.cpp's: a GBA writing its save holds its serial interrupt off for a whole sector erase.
static constexpr uint32_t kGbaHandshakeUs = 60000;
static constexpr uint32_t kMaxRetriesPerWait = 400;

// With the GBA master and silent this long, it is not mid-burst: the loop may work then too.
static constexpr uint32_t kQuietMs = 50;
static constexpr uint32_t kQuietCc = us(kQuietMs * 1000);
// A GBA silent this long in the command phase has frozen or stopped using the adapter: the recorder is snapshotted.
static constexpr uint32_t kSilenceCc = us(1000000);
// Interrupts are let in for a moment at most this often, whenever the GBA cannot start a transfer meanwhile: in a handshake
// window, an event wait, a delivery's handshake waits, a quiet spell. The other core cannot touch flash until this core has
// answered its esp_ipc call (see breathe), so this bounds how long a flash read there waits.
static constexpr uint32_t kBreatheMs = 1;
// Inbound RFU1 bytes handled per pass (two 104-byte data frames). After a command-phase word the GBA sits in its serial
// interrupt meanwhile.
static constexpr size_t kWorkBytes = 208;
// What the Pico's interrupt has to stage a response in (RFU_ISR_BUDGET_US); a pause longer than this is counted.
static constexpr uint32_t kWindowBudgetUs = 30;
// And the least this side holds the GBA after a command-phase word before it answers the handshake. Answering within a few
// microseconds (the code in IRAM) made the GBA misread the next word, the acknowledgement it reads after a command's header,
// so that every command failed three times and the game never started its link; holding as the Pico does (its interrupt
// takes 5-20 us) and as the code from flash did (over 30 us on each command's first word) works. The cause on the GBA's side
// is not known yet.
static constexpr uint32_t kMinHoldUs = CONFIG_UDS_RFU_MIN_HOLD_US;

//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// Byte rings between the cores: one producer, one consumer each.
//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//

template <uint32_t N> struct ByteRing
{
    static_assert((N & (N - 1)) == 0, "power of two");
    uint8_t buf[N];
    std::atomic<uint32_t> head;  // written by the producer
    std::atomic<uint32_t> tail;  // written by the consumer

    // Producer: all or nothing, so a frame is never split by a full ring.
    bool put(const uint8_t* p, size_t n)
    {
        const uint32_t h = head.load(std::memory_order_relaxed);
        const uint32_t t = tail.load(std::memory_order_acquire);
        if (N - (h - t) < n) return false;
        for (size_t i = 0; i < n; ++i) buf[(h + i) & (N - 1)] = p[i];
        head.store(h + static_cast<uint32_t>(n), std::memory_order_release);
        return true;
    }

    // Consumer.
    size_t get(uint8_t* p, size_t n)
    {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        const uint32_t h = head.load(std::memory_order_acquire);
        if (n > h - t) n = h - t;
        for (size_t i = 0; i < n; ++i) p[i] = buf[(t + i) & (N - 1)];
        tail.store(t + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    // Consumer.
    void clear() { tail.store(head.load(std::memory_order_acquire), std::memory_order_release); }
};

// Inbound 2 KB, as the Pico's outbound ring: the Switch sends block transfers in bursts of up to 17 frames. Outbound 1 KB: the
// GBA sends a frame or two per game frame, and the other core empties the ring every millisecond.
static ByteRing<2048> s_fromNet;  // RFU1 stream from the bridge (other core) to the adapter
static ByteRing<1024> s_toNet;    // RFU1 frames from the adapter to the bridge

//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// State shared between the cores.
//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static std::atomic<bool> s_want;       // SetMode(rfuWireless) seen, not stopped since
static std::atomic<uint32_t> s_gen;    // bumped by every SetMode: the adapter starts over, as the Pico re-entering its mode
static volatile uint8_t s_roleLock;    // SetMode's variant byte
static TaskHandle_t s_task;

// Telemetry, written by the link task.
static volatile uint32_t s_lastNetRxMs;
static volatile uint32_t s_revRx0, s_revRx1;
static volatile uint8_t s_delivFail, s_delivSo;
static volatile uint8_t s_sdResets;
static volatile uint32_t s_slips;         // words lost mid-way, once a GBA has been found
static volatile uint32_t s_commandSlips;  // of those, while the GBA was in the command phase: each one a word the core missed
static volatile uint16_t s_outDrops;
static volatile bool s_onWire;
// How long the GBA was held after each command-phase word before this side answered its handshake: the worst since the last
// telemetry, and how many went past kWindowBudgetUs. The GBA's game loop stands still for all of it (librfu spins in its
// serial interrupt), so these say how much of the game's frame the link takes.
static volatile uint32_t s_windowMaxUs;
static volatile uint32_t s_windowsLate;

enum DelivFail : uint8_t { dfNone = 0, dfDanceSoHigh = 3, dfFinalSoLow = 4 };

//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// The link task (second core). Everything below until the bridge side runs there only.
//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//

// Built in place by resetCore. A plain static RfuCore would go in .data (its members have initializers), and this file's static
// data has to be in .bss, which the 3DS side takes for its heap (components/ldn_bridge/linker.lf).
alignas(rfuproto::RfuCore) static uint8_t s_coreSpace[sizeof(rfuproto::RfuCore)];
alignas(rfuproto::Rfu1StreamParser) static uint8_t s_parserSpace[sizeof(rfuproto::Rfu1StreamParser)];
#define g_core (*reinterpret_cast<rfuproto::RfuCore*>(s_coreSpace))
#define g_parser (*reinterpret_cast<rfuproto::Rfu1StreamParser*>(s_parserSpace))

// Dedicated GPIO channel masks.
static uint32_t s_scM, s_rxM, s_sdM, s_txM;

static uint32_t s_staged;
static uint32_t s_wordEndCc;    // when the last GBA-master word ended (listen)
static uint32_t s_wordStartCc;  // when its first clock edge came
static uint8_t s_slipBits;      // how many bits a word lost mid-way had brought (listen)
static uint32_t s_slipPasses;   // and how many times its wait had looked at SC before it fell (0: it was low on arrival)
static uint32_t s_slipAway;     // and how long the loop had been out of a wait before that one began, in cycles
static uint32_t s_listenExitCc; // when the last wait ended
static bool s_quiet;            // the GBA has been silent kQuietMs since its last word
static bool s_silenceSnapped;   // the recorder has been snapshotted for this silence

// Transfer recorder (diagnostics): the last GBA-master words, with what this side sent back, the core's state as each arrived
// and the gap since the word before. Snapshotted when the GBA soft-resets the adapter or restarts the ID exchange, and sent to
// the bridge as adapter trace frames (0x2D, the Pico's tag), which its log keeps (LDN_SHIM_LOG). Written by the link task
// only; a snapshot is handed to the other core through s_snapPending.
struct Xfer
{
    uint32_t rx, tx;
    uint16_t gapUs;
    uint8_t comstate;  // kXferSlip: a word lost mid-way (recordSlip)
};
static constexpr uint8_t kXferSlip = 15;  // no comstate has this value
static constexpr uint8_t kXferRing = 32;
static constexpr uint8_t kSnapXfers = 20;
static Xfer s_xfer[kXferRing];
static uint8_t s_xferHead;
static uint32_t s_prevWordEndCc;
static Xfer s_snap[kSnapXfers];
static uint8_t s_snapLen;
// 0: the GBA went silent for a second in the command phase (the game froze, or stopped using the adapter), 1: SD soft reset,
// 2: ID exchange restarted from the command stream, 3: command-phase slip.
static uint8_t s_snapReason;
static std::atomic<bool> s_snapPending;

static void IRAM_ATTR snapshot(uint8_t reason)
{
    if (s_snapPending.load(std::memory_order_acquire)) return;
    uint8_t n = 0;
    for (uint8_t i = kSnapXfers; i > 0; --i)
    {
        const Xfer& x = s_xfer[static_cast<uint8_t>(s_xferHead - i) & (kXferRing - 1)];
        if (x.rx == 0 && x.tx == 0 && x.gapUs == 0) continue;  // never written
        s_snap[n++] = x;
    }
    s_snapLen = n;
    s_snapReason = reason;
    s_snapPending.store(true, std::memory_order_release);
}
static uint32_t s_lastTickMs, s_lastBreatheMs;
static uint32_t s_retriesThisWait;
static UBaseType_t s_irqLevel;
// Volatile so listen stores them where they change (rarely), not on its way out of the wait, where every cycle delays the first
// bit.
static volatile bool s_sdHigh;
static volatile uint32_t s_sdRiseAt;

static inline uint32_t ccount() { return esp_cpu_get_cycle_count(); }
static inline uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
static inline uint32_t rd() { return dedic_gpio_cpu_ll_read_in(); }
static inline void tx(bool high) { dedic_gpio_cpu_ll_write_mask(s_txM, high ? s_txM : 0); }
static inline void IRAM_ATTR spin(uint32_t cycles)
{
    const uint32_t t0 = ccount();
    while (ccount() - t0 < cycles) { }
}

static uint16_t rand16(void*)
{
    // Device ids only need to differ between sessions (the Pico's randTrampoline).
    static uint32_t s = 0;
    if (s == 0) s = ccount() | 1;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<uint16_t>(s ^ ccount());
}

static void GLUE_IRAM emit(void*, const uint8_t* frame, size_t len)
{
    if (!s_toNet.put(frame, len)) s_outDrops = s_outDrops + 1;
}

static void GLUE_IRAM onFrame(void*, uint32_t ptype, uint32_t hdata, const uint8_t* payload, uint16_t payloadLen)
{
    const uint32_t now = nowMs();
    s_lastNetRxMs = now ? now : 1;
    g_core.applyNetPacket(ptype, hdata, payload, payloadLen, now);
}

static void resetCore()
{
    new (s_coreSpace) rfuproto::RfuCore();
    new (s_parserSpace) rfuproto::Rfu1StreamParser();
    g_core.emit = &emit;
    g_core.rand16 = &rand16;
    g_core.reset();
    g_core.roleLock = s_roleLock;  // after reset(), which must not clobber it
    g_parser.setCallback(&onFrame, nullptr);
    s_fromNet.clear();
    s_staged = 0;  // the RESET-state response, as rfuLink_enable primes it
    s_lastNetRxMs = 0;
    s_revRx0 = 0;
    s_revRx1 = 0;
    s_delivFail = dfNone;
    s_delivSo = 0;
    s_retriesThisWait = 0;
    s_sdResets = 0;
    s_slips = 0;
    s_commandSlips = 0;
    s_windowMaxUs = 0;
    s_windowsLate = 0;
    std::memset(s_xfer, 0, sizeof(s_xfer));
    s_xferHead = 0;
    s_prevWordEndCc = s_wordEndCc = ccount();
    s_quiet = false;
    s_silenceSnapped = true;  // nothing to show before the first word
    s_lastTickMs = s_lastBreatheMs = nowMs();
}

// The Pico's safeToApplyInbound: frames reach the core only between commands or while the GBA is slave. Until then they wait
// in the ring, in order (the Pico parks them).
static inline bool safeToApplyInbound(uint8_t comstate)
{
    return comstate == rfuproto::comWaitCmd || comstate == rfuproto::comWaitEvent ||
           comstate == rfuproto::comWaitResp;
}

// Network frames and the core's upkeep: in a handshake window, an event wait, or a quiet spell.
static void GLUE_IRAM work(uint32_t now, size_t budget)
{
    if (safeToApplyInbound(g_core.comstate))
    {
        uint8_t bytes[kWorkBytes];
        const size_t n = s_fromNet.get(bytes, budget < sizeof(bytes) ? budget : sizeof(bytes));
        if (n) g_parser.push(bytes, n);
    }
    if (now != s_lastTickMs)
    {
        s_lastTickMs = now;
        g_core.tick(now);
    }
}

// A moment with interrupts let in; this core takes none otherwise. Its tick waits for this, and so does anything the other core
// sends it with esp_ipc_call. That includes every flash access there (NVS reads among them: the Switch side reads its keys when
// a GBA connects): spi_flash's cache_utils.c parks this core with esp_ipc_call_nonblocking and busy-waits, scheduler stopped,
// until it has. Without these moments the other core froze for as long as the GBA kept talking, the first hardware test's
// failed connects. The parked core then spins in spi_flash_op_block_func until the flash access is over (the GBA waits in its
// handshake meanwhile).
static void GLUE_IRAM breathe(uint32_t now)
{
    if (now - s_lastBreatheMs < kBreatheMs) return;
    s_lastBreatheMs = now;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(s_irqLevel);
    spin(us(5));
    s_irqLevel = portSET_INTERRUPT_MASK_FROM_ISR();
}

enum Listen { kIdle, kWord, kSlip, kSdReset };

// GBA-master transfers (the Pico's rfu_slave32 program). Until SC falls, SI mirrors NOT SO, which answers librfu's
// handshake_wait(1)/(0) between words, and SD is watched for the soft-reset pulse. Then 32 bits, MSB first: both ends shift
// out on the falling edge and sample on the rising edge. SI is left low, as the PIO program's `set pins, 0` leaves it while
// the CPU stages the next response: the GBA's handshake_wait(1) holds it there until the next call mirrors.
static int IRAM_ATTR __attribute__((noinline)) listen(uint32_t word, uint32_t budget, uint32_t* out)
{
    const uint32_t start = ccount();
    const uint32_t away = start - s_listenExitCc;
    uint32_t passes = 0;
    uint32_t v;
    // SC is looked at between every step of the loop. Its first rise reaches this side late and briefly: through the level
    // shifter a rising GBA line climbs on the 10k pull-up, so SC reads high only for the last part of each half bit. Noticing
    // the first fall a loop late (~100 ns) and then blanking missed that rise in a hardware test, and with it bit 31 of a
    // search result word: the word slipped with 31 bits.
    for (;;)
    {
        v = rd();
        if (!(v & s_scM)) break;
        dedic_gpio_cpu_ll_write_mask(s_txM, (v & s_rxM) ? 0 : s_txM);
        if (!(rd() & s_scM)) break;
        if (s_sdM)
        {
            const bool sd = (v & s_sdM) != 0;
            if (sd != s_sdHigh)
            {
                s_sdHigh = sd;
                if (sd)
                    s_sdRiseAt = ccount();
                else
                {
                    const uint32_t held = ccount() - s_sdRiseAt;
                    if (held >= kSdPulseMin && held <= kSdPulseMax)
                    {
                        s_listenExitCc = ccount();
                        return kSdReset;
                    }
                }
            }
        }
        if (!(rd() & s_scM)) break;
        ++passes;
        if (ccount() - start > budget)
        {
            s_listenExitCc = ccount();
            return kIdle;
        }
    }

    // SI changes as soon as the GBA has sampled a bit, not on the next falling edge as a real adapter's does: through the level
    // shifter a rising SI climbs only on its 10k pull-up (~250-300 ns time constant), which half a 2 MHz bit does not cover, and
    // the GBA then reads some 0->1 steps late (librfu restarts the command). SC's own rise reaches this side ~150 ns after the
    // GBA latched SI, so the change cannot catch the bit it sampled, and the next has most of a bit to settle. Bit 31 goes out
    // on the first falling edge (the mirror has SI high until then).
    // rx starts with a marker bit above the word: 32 bits shift it out, and after a slip it shows how many arrived.
    uint32_t rx = 1;
    dedic_gpio_cpu_ll_write_mask(s_txM, (word >> 31) ? s_txM : 0);
    s_wordStartCc = ccount();
    for (int bit = 31;; --bit)
    {
        // No blank after the first fall: the wait above saw it late enough already.
        if (bit != 31) spin(kBlankFall);
        uint32_t t0 = ccount();
        // Every bit, bit 0 included, is read once SC's rise has reached this side: through the level shifter a rising SO
        // climbs on a pull-up and is not yet high just before the edge (reading bit 0 there turned 0x9966003D into ...3C).
        while (!((v = rd()) & s_scM))
            if (ccount() - t0 > kMidWord) goto slip;
        rx = (rx << 1) | ((v & s_rxM) ? 1u : 0u);
        if (bit == 0) break;
        dedic_gpio_cpu_ll_write_mask(s_txM, ((word >> (bit - 1)) & 1) ? s_txM : 0);
        spin(kBlankRise);
        t0 = ccount();
        while ((v = rd()) & s_scM)
            if (ccount() - t0 > kMidWord) goto slip;
    }
    spin(kHoldLast);
    dedic_gpio_cpu_ll_write_mask(s_txM, 0);
    s_wordEndCc = s_listenExitCc = ccount();
    s_quiet = false;
    s_silenceSnapped = false;
    *out = rx;
    return kWord;

slip:
    // Also what a GBA that is off looks like: through the level shifter every line then reads low.
    dedic_gpio_cpu_ll_write_mask(s_txM, 0);
    // Only once a GBA has been found: before that a console that is off reads as one long slip.
    if (g_core.dbgNintendo) s_slips = s_slips + 1;
    // For the recorder: the bits that did arrive, and how many.
    s_slipBits = static_cast<uint8_t>(31 - __builtin_clz(rx));
    *out = rx & ~(1u << s_slipBits);
    s_slipPasses = passes;
    s_slipAway = away;
    s_listenExitCc = ccount();
    // Not a word for the quiet spell: with no GBA clocking, the lines read low through the level shifter and every listen
    // slips. Counting those as words kept the loop from ever going quiet, so this core never let interrupts in and the other
    // core froze on its first flash access (a test build that no longer answered on USB).
    return kSlip;
}

// Adapter-master transfer (the Pico's rfu_master32 program): this side clocks one word into the GBA-as-slave and reads what it
// shifted out. SC goes through the GPIO registers; at ~154 kHz their latency does not matter.
static uint32_t IRAM_ATTR __attribute__((noinline)) masterExchange(uint32_t word)
{
    uint32_t rx = 0;
    for (int bit = 31; bit >= 0; --bit)
    {
        gpio_ll_set_level(&GPIO, PIN_SC, 0);
        dedic_gpio_cpu_ll_write_mask(s_txM, ((word >> bit) & 1) ? s_txM : 0);
        spin(kMasterSample);
        rx = (rx << 1) | ((rd() & s_rxM) ? 1u : 0u);
        spin(kMasterLow);
        gpio_ll_set_level(&GPIO, PIN_SC, 1);
        spin(kMasterHigh);
    }
    return rx;
}

// Takes SC from the GBA with no edge: the latch is already high, and an armed GBA-as-slave counts every edge as a bit.
static void GLUE_IRAM takeSc()
{
    gpio_ll_set_level(&GPIO, PIN_SC, 1);
    gpio_ll_output_enable(&GPIO, PIN_SC);
}

static void GLUE_IRAM releaseSc()
{
    gpio_ll_set_level(&GPIO, PIN_SC, 1);
    gpio_ll_output_disable(&GPIO, PIN_SC);
}

// The GBA waits in its handshake meanwhile, so a long wait breathes (a save in progress holds it up to tens of milliseconds).
static bool GLUE_IRAM waitSo(bool high, uint32_t timeoutUs)
{
    const uint32_t t0 = ccount();
    const uint32_t limit = us(timeoutUs);
    uint32_t lastBreath = t0;
    for (;;)
    {
        if (((rd() & s_rxM) != 0) == high) return true;
        const uint32_t now = ccount();
        if (now - t0 > limit) return false;
        if (now - lastBreath > us(1000))
        {
            lastBreath = now;
            breathe(nowMs());
        }
    }
}

static void GLUE_IRAM sdReset(uint32_t now)
{
    snapshot(1);
    g_core.onSdReset(now);
    s_sdResets = s_sdResets + 1;
    s_retriesThisWait = 0;
    spin(kSdSettle);
    s_staged = g_core.stagedTx;
}

// A word the GBA clocked as master. In IRAM, with the core's code it runs (components/ldn_bridge/linker.lf): the GBA waits in its
// handshake until this has staged the next response, and from flash each cache miss costs ~0.5 us; the other core's Wi-Fi code
// evicts this between commands, which held the GBA 60 us or more a command and halved its frame rate in the first room tests.
static void GLUE_IRAM onWord(uint32_t rx, uint32_t now)
{
    Xfer& x = s_xfer[s_xferHead++ & (kXferRing - 1)];
    x.rx = rx;
    x.tx = s_staged;  // what went out during this word
    x.comstate = g_core.comstate;
    const uint32_t gapUs = (s_wordStartCc - s_prevWordEndCc) / kMhz;
    x.gapUs = static_cast<uint16_t>(gapUs > 65535 ? 65535 : gapUs);
    s_prevWordEndCc = s_wordEndCc;
    const uint32_t loginRestarts = g_core.counters.loginRestarts;
    g_core.onSlaveTransfer(rx, now);
    if (g_core.counters.loginRestarts != loginRestarts) snapshot(2);
    s_staged = g_core.stagedTx;
}

// A word lost mid-way (listen's kSlip). In the command phase the GBA finished it anyway and goes on to the next, so the core,
// which never saw it, now answers one word behind. Recorded as a transfer with comstate kXferSlip: rx = the bits that came;
// tx = the bit count (bits 31-24), how many times the wait looked at SC before it fell (23-16; 0: SC was already low, the
// loop came back mid-word) and how long the loop had been away before that wait (15-0, cycles); gapUs = the comstate. The
// first one snapshots the recorder.
static void GLUE_IRAM recordSlip(uint32_t rx)
{
    const uint8_t cs = g_core.comstate;
    if (cs < rfuproto::comWaitCmd) return;  // the ID exchange starts over by itself
    // No bit at all: SC fell and stayed low, no word. The GBA does that around its soft reset (a hardware test counted tens of
    // thousands after each stop-mode command).
    if (s_slipBits == 0) return;
    s_commandSlips = s_commandSlips + 1;
    Xfer& x = s_xfer[s_xferHead++ & (kXferRing - 1)];
    x.rx = rx;
    x.tx = (static_cast<uint32_t>(s_slipBits) << 24) | ((s_slipPasses > 255 ? 255 : s_slipPasses) << 16) |
           (s_slipAway > 65535 ? 65535 : s_slipAway);
    x.comstate = kXferSlip;
    x.gapUs = cs;
    s_prevWordEndCc = ccount();
    snapshot(3);
}

// rfuProtocolSection.cpp's runDelivery: clock one wait-event frame into the GBA-as-slave word by word, with librfu's slave-ISR
// handshake between words (sio32intr_clock_slave). Word 1 needs none: the GBA armed itself right after the WAIT-class ACK.
static void GLUE_IRAM runDelivery()
{
    if (g_core.comstate != rfuproto::comWaitResp || g_core.eventWordCount() > 8)
    {
        s_retriesThisWait = 0;
        return;
    }
    uint32_t words[8];
    const uint8_t n = g_core.eventWordCount();
    for (uint8_t i = 0; i < n; i++) words[i] = g_core.eventWord(i);

    // The mirror serves the GBA's post-ACK handshake and arming first (the Pico waits 150 us with its slave program live). A
    // soft-resetting GBA can clock an ID word meanwhile, which takes the core back to the ID exchange.
    {
        uint32_t rx;
        const uint32_t t0 = ccount();
        for (;;)
        {
            const uint32_t spent = ccount() - t0;
            if (spent >= us(150)) break;
            const int r = listen(s_staged, us(150) - spent, &rx);
            if (r == kWord) onWord(rx, nowMs());
            else if (r == kSdReset) sdReset(nowMs());
            else if (r == kSlip) recordSlip(rx);
            if (g_core.comstate != rfuproto::comWaitResp) return;
            if (r == kIdle) break;
        }
    }

    tx(false);  // SI low: the inverted-ACK lead-in
    takeSc();

    // Word-1 inverted-ACK lead-in (afska wireless_adapter.md): the GBA may answer SI low with an SO-high pulse, usually not.
    if (waitSo(true, 120))
    {
        tx(true);
        waitSo(false, 4000);
        tx(false);
    }
    spin(us(45));

    bool ok = true;
    bool swapped = false;
    for (uint8_t i = 0; ok && i < n; i++)
    {
        const uint32_t rx = masterExchange(words[i]);
        s_revRx0 = s_revRx1;
        s_revRx1 = rx;
        g_core.counters.masterTransfers++;

        if (i > 0 && rx == 0x996601EE)
        {
            // The GBA rejected the event and staged an error response instead; it recovers through its timer and checkID.
            g_core.counters.errResponses++;
            ok = false;
            break;
        }

        tx(false);
        ok = waitSo(true, kGbaHandshakeUs);  // SO high: word received, being processed
        if (!ok)
        {
            s_delivFail = static_cast<uint8_t>(dfDanceSoHigh | (i << 4));
            s_delivSo = (rd() & s_rxM) ? 1 : 0;
            break;
        }
        tx(true);  // releases its handshake_wait(1)

        if (i + 1 < n)
        {
            // It re-arms with two SIOCNT writes and no level to watch (librfu_intr.c:311-328), within ~125 us.
            spin(us(250));
        }
        else
        {
            // Final word: the GBA drops SO and takes the clock back within tens of microseconds. SC is released at once.
            if (waitSo(false, kGbaHandshakeUs))
            {
                g_core.finishDelivery(nowMs());
                releaseSc();
                swapped = true;
            }
            else
            {
                ok = false;
                s_delivFail = static_cast<uint8_t>(dfFinalSoLow | (i << 4));
                s_delivSo = (rd() & s_rxM) ? 1 : 0;
            }
        }
    }

    if (swapped)
    {
        s_delivFail = dfNone;
        s_retriesThisWait = 0;
    }
    else
    {
        // The Pico retries a delivery whose first word never reached the wire; a software clock cannot fail that way.
        g_core.abortDelivery(nowMs());
        s_retriesThisWait = 0;
        releaseSc();
        tx(true);
    }
    s_staged = g_core.stagedTx;
}

// The GBA dropped to slave after a WAIT-class ACK (or is about to, once it has read the ACK): it clocks nothing until this side
// delivers the event, unless it soft-resets. Free time for the network and the core.
static void GLUE_IRAM serveWait()
{
    uint32_t rx;
    const int r = listen(s_staged, kWaitListenBudget, &rx);
    const uint32_t now = nowMs();
    if (r == kWord)
    {
        onWord(rx, now);
        return;
    }
    if (r == kSdReset)
    {
        sdReset(now);
        return;
    }
    if (r == kSlip) recordSlip(rx);
    work(now, kWorkBytes);
    breathe(now);
    if (g_core.comstate == rfuproto::comWaitResp || g_core.pollWaitEvent(now))
        runDelivery();
}

static void GLUE_IRAM __attribute__((noinline)) session(uint32_t gen)
{
    resetCore();
    s_sdHigh = s_sdM && (rd() & s_sdM);
    tx(true);
    releaseSc();
    s_onWire = true;

    bool holding = false;  // a command-phase word just ended: the GBA waits in its handshake
    s_irqLevel = portSET_INTERRUPT_MASK_FROM_ISR();
    while (s_want.load(std::memory_order_acquire) && s_gen.load(std::memory_order_acquire) == gen)
    {
        const uint8_t cs = g_core.comstate;
        if ((cs == rfuproto::comWaitEvent && g_core.dbgWaitAck()) || cs == rfuproto::comWaitResp)
        {
            holding = false;
            serveWait();
            continue;
        }

        if (holding)
        {
            const uint32_t heldUs = (ccount() - s_wordEndCc) / kMhz;
            if (heldUs > s_windowMaxUs) s_windowMaxUs = heldUs;
            if (heldUs > kWindowBudgetUs) s_windowsLate = s_windowsLate + 1;
            holding = false;
        }

        // Until the GBA goes quiet the wait runs to the quiet spell in one go: there is nothing to do between commands, and
        // every return to the loop is a moment a command's unannounced first word can start in. Hardware tests lost the first
        // bit of command headers (31 of 32 bits arrived), and with each a 130 ms librfu retry.
        uint32_t budget = kListenBudget;
        if (!s_quiet)
        {
            const uint32_t since = ccount() - s_wordEndCc;
            if (since < kQuietCc) budget = kQuietCc - since;
        }
        uint32_t rx;
        const int r = listen(s_staged, budget, &rx);
        if (r == kIdle || r == kSlip)
        {
            if (r == kSlip) recordSlip(rx);
            // Straight back to the wait for SC unless the GBA has gone quiet: a word that starts while this side is away is
            // lost, and with it the word count (the core answers one word behind from then on). Reading the timer here took a
            // microsecond or two each time a wait ran out, a gap a word could start in. A slip ends no quiet spell: with no
            // GBA clocking, every listen slips, and this core must still let interrupts in.
            if (!s_quiet)
            {
                if (ccount() - s_wordEndCc < kQuietCc) continue;
                s_quiet = true;
            }
            if (!s_silenceSnapped && g_core.comstate >= rfuproto::comWaitCmd && ccount() - s_wordEndCc >= kSilenceCc)
            {
                s_silenceSnapped = true;
                snapshot(0);
            }
            const uint32_t now = nowMs();
            work(now, kWorkBytes);
            breathe(now);
            continue;
        }
        const uint32_t now = nowMs();
        if (r == kWord)
        {
            onWord(rx, now);
            // The GBA is in its handshake now, except in the ID exchange (words come unannounced). It stands still until this
            // side answers (the hold counters measure how long).
            holding = g_core.comstate >= rfuproto::comWaitCmd;
            if (holding)
            {
                work(now, kWorkBytes);
                breathe(now);
                if (kMinHoldUs)
                    while (ccount() - s_wordEndCc < us(kMinHoldUs)) { }
            }
        }
        else
            sdReset(now);
    }
    portCLEAR_INTERRUPT_MASK_FROM_ISR(s_irqLevel);

    releaseSc();
    s_onWire = false;
}

static void linkTask(void*)
{
    // Dedicated GPIO channels belong to the core that creates them: this one.
    const int inPins[3] = {PIN_SC, PIN_RX, PIN_SD};
    const size_t inCount = PIN_SD >= 0 ? 3 : 2;
    gpio_config_t in = {};
    in.pin_bit_mask = (1ULL << PIN_SC) | (1ULL << PIN_RX);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;  // the SIO lines idle high
    ESP_ERROR_CHECK(gpio_config(&in));
#if PIN_SD >= 0
    in.pin_bit_mask = 1ULL << PIN_SD;
    in.pull_up_en = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&in));
#endif
    // SC is also driven, while this side clocks an event in: a plain GPIO output, latch high, enabled only then.
    gpio_ll_set_level(&GPIO, PIN_SC, 1);
    esp_rom_gpio_connect_out_signal(PIN_SC, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, PIN_SC, false, false);
    gpio_ll_output_disable(&GPIO, PIN_SC);

    // A ringing clock edge read as two is a lost word; the filter drops pulses under two IO MUX clocks. SO and SD carry the
    // same spikes, which GB-Link's ESP32 adapter filters too.
    const int filtered[3] = {PIN_SC, PIN_RX, PIN_SD};
    for (int i = 0; i < 3; ++i)
    {
        if (filtered[i] < 0) continue;
        gpio_pin_glitch_filter_config_t filterConfig = {};
        filterConfig.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT;
        filterConfig.gpio_num = static_cast<gpio_num_t>(filtered[i]);
        gpio_glitch_filter_handle_t filter = nullptr;
        if (gpio_new_pin_glitch_filter(&filterConfig, &filter) == ESP_OK) gpio_glitch_filter_enable(filter);
    }

    dedic_gpio_bundle_handle_t inBundle = nullptr, outBundle = nullptr;
    dedic_gpio_bundle_config_t inConfig = {};
    inConfig.gpio_array = inPins;
    inConfig.array_size = inCount;
    inConfig.flags.in_en = 1;
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&inConfig, &inBundle));
    const int outPins[1] = {PIN_TX};
    dedic_gpio_bundle_config_t outConfig = {};
    outConfig.gpio_array = outPins;
    outConfig.array_size = 1;
    outConfig.flags.out_en = 1;
    dedic_gpio_cpu_ll_write_all(~0u);  // SI high from the first moment it is driven
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&outConfig, &outBundle));
    gpio_ll_set_output_enable_ctrl(&GPIO, PIN_TX, false, false);
    gpio_ll_output_enable(&GPIO, PIN_TX);

    uint32_t inOffset = 0, outOffset = 0;
    dedic_gpio_get_in_offset(inBundle, &inOffset);
    dedic_gpio_get_out_offset(outBundle, &outOffset);
    s_scM = 1u << inOffset;
    s_rxM = 1u << (inOffset + 1);
    s_sdM = PIN_SD >= 0 ? 1u << (inOffset + 2) : 0;
    s_txM = 1u << outOffset;

    for (;;)
    {
        if (!s_want.load(std::memory_order_acquire))
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        session(s_gen.load(std::memory_order_acquire));
    }
}

//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//
// The bridge side (first core): 'GB' frames in and out, the Pico's half-second telemetry, the status LED.
//-////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////-//

static constexpr uint8_t kSync0 = 0x47, kSync1 = 0x42;  // 'G' 'B'
static constexpr uint8_t kChannelCommand = 0x00, kChannelData = 0x01, kChannelStatus = 0x02;
static constexpr uint16_t kMaxPayload = 128;
static constexpr uint8_t kModeRfuWireless = 0x07;
static constexpr uint16_t kStatusAwaitMode = 0xFF02;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Bridge to adapter, under s_lock.
static struct
{
    uint8_t state;
    uint8_t channel;
    uint16_t length;
    uint16_t seen;
    uint8_t payload[kMaxPayload];
} s_in;
static uint16_t s_statusQueue[4];
static uint8_t s_statusCount;
static uint32_t s_fromNetDrops;

// Adapter to bridge: rfu_adapter_read's caller only.
static uint8_t s_out[512];
static size_t s_outLen;
static uint8_t s_chunk[64];
static size_t s_chunkLen;
static uint32_t s_lastTelemetryMs;

static void onBridgeFrame()
{
    if (s_in.channel == kChannelCommand && s_in.length >= 2 && s_in.payload[0] == 0x00)
    {
        // SetMode. Into wireless mode, again or not, the adapter starts over and says so; any other mode takes it off the wire.
        if (s_in.payload[1] == kModeRfuWireless)
        {
            s_roleLock = s_in.length >= 3 ? s_in.payload[2] : 0;
            s_gen.fetch_add(1, std::memory_order_acq_rel);
            s_want.store(true, std::memory_order_release);
            if (s_statusCount < sizeof(s_statusQueue) / sizeof(s_statusQueue[0]))
                s_statusQueue[s_statusCount++] = kStatusAwaitMode;
        }
        else
            s_want.store(false, std::memory_order_release);
    }
    else if (s_in.channel == kChannelData && s_in.length)
    {
        if (!s_fromNet.put(s_in.payload, s_in.length)) ++s_fromNetDrops;
    }
    // Other commands (the Pico's BOOTSEL reboot among them) have no meaning here.
}

static void feedBridgeByte(uint8_t b)
{
    switch (s_in.state)
    {
    case 0: if (b == kSync0) s_in.state = 1; return;
    case 1: s_in.state = (b == kSync1) ? 2 : (b == kSync0 ? 1 : 0); return;
    case 2: s_in.channel = b; s_in.state = 3; return;
    case 3: s_in.length = b; s_in.state = 4; return;
    case 4:
        s_in.length |= static_cast<uint16_t>(b) << 8;
        s_in.seen = 0;
        if (s_in.length > kMaxPayload) { s_in.state = 0; return; }
        if (s_in.length == 0) { onBridgeFrame(); s_in.state = 0; return; }
        s_in.state = 5;
        return;
    default:
        s_in.payload[s_in.seen++] = b;
        if (s_in.seen >= s_in.length)
        {
            onBridgeFrame();
            s_in.state = 0;
        }
        return;
    }
}

static bool queueFrame(uint8_t channel, const uint8_t* payload, size_t length)
{
    if (sizeof(s_out) - s_outLen < length + 5) return false;
    uint8_t* o = s_out + s_outLen;
    o[0] = kSync0;
    o[1] = kSync1;
    o[2] = channel;
    o[3] = static_cast<uint8_t>(length);
    o[4] = static_cast<uint8_t>(length >> 8);
    std::memcpy(o + 5, payload, length);
    s_outLen += length + 5;
    return true;
}

// rfuProtocolSection.cpp's pumpOutbound: RFU1 frames in 64-byte chunks, the last zero padded (the parser skips padding).
static void pumpOutbound()
{
    for (;;)
    {
        if (s_chunkLen < sizeof(s_chunk)) s_chunkLen += s_toNet.get(s_chunk + s_chunkLen, sizeof(s_chunk) - s_chunkLen);
        if (s_chunkLen == 0) return;
        if (s_chunkLen < sizeof(s_chunk)) std::memset(s_chunk + s_chunkLen, 0, sizeof(s_chunk) - s_chunkLen);
        if (!queueFrame(kChannelData, s_chunk, sizeof(s_chunk))) return;  // kept for the next call
        const bool full = s_chunkLen == sizeof(s_chunk);
        s_chunkLen = 0;
        if (!full) return;
    }
}

static void put32(uint8_t* o, uint32_t v)
{
    o[0] = static_cast<uint8_t>(v);
    o[1] = static_cast<uint8_t>(v >> 8);
    o[2] = static_cast<uint8_t>(v >> 16);
    o[3] = static_cast<uint8_t>(v >> 24);
}

// rfuProtocolSection.cpp's reportDiagnostics, the frames the bridge reads (pico_link: 0x0E; trade_shim: 0x2E, 0x1D, 0x1E).
// The core is read from this core while the link task changes it; a torn counter in a log is the worst of it.
static void queueTelemetry()
{
    uint8_t d[16] = {0x0E, static_cast<uint8_t>(g_core.dbgAnyRx ? 1 : 0), static_cast<uint8_t>(g_core.dbgNintendo ? 1 : 0),
                     static_cast<uint8_t>(g_core.comstate)};
    put32(d + 4, g_core.dbgFirstRx);
    put32(d + 8, g_core.dbgLastRx);
    put32(d + 12, g_core.counters.slaveTransfers);
    queueFrame(kChannelData, d, sizeof(d));

    uint8_t f[62] = {};
    f[0] = 0x2E;
    f[1] = static_cast<uint8_t>(g_core.comstate);
    f[2] = static_cast<uint8_t>(g_core.state);
    f[3] = (g_core.comstate == rfuproto::comWaitResp) ? 1 : 0;
    f[4] = static_cast<uint8_t>((g_core.dbgAnyRx ? 0x01 : 0) | (g_core.dbgNintendo ? 0x02 : 0) |
                                (g_core.dbgWaitAck() ? 0x04 : 0));
    f[5] = g_core.dbgLastCmd();
    f[6] = g_core.dbgLastPlen();
    const uint8_t cnt = g_core.dbgCmdRingCount;
    const uint8_t head = g_core.dbgCmdRingHead;
    f[7] = cnt;
    for (uint8_t i = 0; i < 8; i++) f[8 + i] = (i < cnt) ? g_core.dbgCmdRing[static_cast<uint8_t>(head - cnt + i) & 7] : 0;
    const uint32_t c32[8] = {
        g_core.counters.slaveTransfers, g_core.counters.masterTransfers, g_core.counters.waitEventTransfers,
        g_core.counters.commands,       g_core.counters.deliveryAborts,  g_core.counters.loginRestarts,
        g_core.counters.errResponses,   g_core.dbgLastRx,
    };
    for (int i = 0; i < 8; i++) put32(f + 16 + i * 4, c32[i]);
    put32(f + 48, s_revRx0);
    put32(f + 52, s_revRx1);
    f[56] = s_delivFail;
    f[57] = s_delivSo;
    put32(f + 58, g_core.counters.deliveryRetries);
    queueFrame(kChannelData, f, sizeof(f));

    const uint32_t lev = g_core.dbgLastEvent;
    const uint32_t lpw = g_core.dbgLastLinkPwr;
    const auto sat4 = [](uint8_t v) -> uint8_t { return v > 15 ? 15 : v; };
    const uint8_t fifoHigh = g_core.dbgFifoHigh;
    g_core.dbgFifoHigh = 0;
    const uint32_t queueFull = g_core.counters.rxDropQueueFull;
    uint8_t ev[25] = {
        0x1D,
        g_core.dbgEvData, g_core.dbgEvRtx, g_core.dbgEvTimeo, g_core.dbgEvDisc,
        0, 0, 0, 0,
        g_core.dbgLinkPwrZero,
        0, 0, 0, 0,
        s_sdResets,
        static_cast<uint8_t>((sat4(g_core.dbgWipeEvict) << 4) | sat4(g_core.dbgWipeNetDisc)),
        static_cast<uint8_t>((sat4(g_core.dbgWipeHostStart) << 4) | sat4(g_core.dbgWipeCmdDisc)),
        static_cast<uint8_t>((sat4(g_core.dbgWipeReset) << 4) | g_core.dbgSlotOccupancy()),
        0,  // the Pico's parked-queue losses: frames wait in the ring here, and a full ring loses them on the bridge side
        static_cast<uint8_t>(queueFull > 255 ? 255 : queueFull),
        fifoHigh,
        static_cast<uint8_t>((sat4(g_core.dbgFlowHints) << 4) | sat4(g_core.dbgFlowHolds)),
        g_core.dbgSheds,
        g_core.dbgIdleRetx,
        static_cast<uint8_t>(g_core.counters.commands),
    };
    put32(ev + 5, lev);
    put32(ev + 10, lpw);
    queueFrame(kChannelData, ev, sizeof(ev));

    const auto sat16 = [](uint32_t v) -> uint16_t { return v > 65535 ? 65535 : static_cast<uint16_t>(v); };
    const uint16_t inDrops = sat16(s_fromNetDrops);
    const uint16_t qf = sat16(queueFull), ab = sat16(g_core.counters.deliveryAborts);
    const uint16_t rt = sat16(g_core.counters.deliveryRetries), rs = sat16(g_core.counters.commandRestarts);
    const uint16_t outDrops = s_outDrops;
    const uint16_t slips = sat16(s_commandSlips);
    const uint32_t windowMax4 = (s_windowMaxUs + 3) / 4;
    s_windowMaxUs = 0;  // per report
    const uint16_t late = sat16(s_windowsLate);
    const uint8_t tr[19] = {
        0x1E,
        static_cast<uint8_t>(inDrops), static_cast<uint8_t>(inDrops >> 8),  // the Pico's UART overflows: bridge frames lost here
        // The Pico's parked-queue high-water (park_high in the bridge's log, which keeps the highest): the longest the GBA was
        // held after a word since the last report, in 4 us units.
        static_cast<uint8_t>(windowMax4 > 255 ? 255 : windowMax4),
        // The Pico's parked-frame losses (none here: frames wait in the ring): command-phase words lost mid-way instead, each
        // one a word the core never saw (see recordSlip). The bridge's log calls them park_drops.
        static_cast<uint8_t>(slips), static_cast<uint8_t>(slips >> 8),
        static_cast<uint8_t>(qf), static_cast<uint8_t>(qf >> 8),
        fifoHigh,
        static_cast<uint8_t>(ab), static_cast<uint8_t>(ab >> 8),
        static_cast<uint8_t>(rt), static_cast<uint8_t>(rt >> 8),
        // The Pico's idle-frame drops (park_idle_drops): how many holds went past kWindowBudgetUs.
        static_cast<uint8_t>(late), static_cast<uint8_t>(late >> 8),
        static_cast<uint8_t>(outDrops), static_cast<uint8_t>(outDrops >> 8),
        static_cast<uint8_t>(rs), static_cast<uint8_t>(rs >> 8),
    };
    queueFrame(kChannelData, tr, sizeof(tr));
}

// A transfer snapshot as trace frames the bridge logs entry by entry, as "+<n>ms ? <xx>": three entries a transfer, four
// transfers a frame. Entry 1: xx = rx bits 31-24, n = rx bits 23-8. Entry 2: xx = rx bits 7-0, n = what this side sent, bits
// 31-16. Entry 3: xx = the core's comstate (high nibble) and the gap since the previous word in 32 us steps (low nibble, 15 =
// 480 us or more), n = what this side sent, bits 15-0. A word lost mid-way (recordSlip) has comstate 15 and the real comstate
// in the low nibble; rx is the bits that came, entry 2's n = the bit count * 256 + the wait's looks at SC before it fell, and
// entry 3's n = how long the loop had been away before that wait, in cycles. Frame byte 3 (the note's low byte): reason << 6 |
// first transfer.
// At most one snapshot every few seconds, so a reset loop does not fill the log.
static uint8_t s_traceSeq;
static uint32_t s_lastSnapSentMs;

static void queueSnapshot(uint32_t now)
{
    if (!s_snapPending.load(std::memory_order_acquire)) return;
    if (s_lastSnapSentMs && now - s_lastSnapSentMs < 4000) return;
    const size_t frames = (s_snapLen + 3) / 4;
    if (sizeof(s_out) - s_outLen < frames * (5 + 4 + 12 * 4)) return;  // all of it at once, or next call
    for (uint8_t first = 0; first < s_snapLen; first += 4)
    {
        uint8_t f[4 + 12 * 4] = {};
        const uint8_t count = static_cast<uint8_t>((s_snapLen - first < 4 ? s_snapLen - first : 4) * 3);
        f[0] = 0x2D;
        f[1] = ++s_traceSeq;
        f[2] = count;
        f[3] = static_cast<uint8_t>((s_snapReason << 6) | first);
        for (uint8_t i = 0; i < count / 3; ++i)
        {
            const Xfer& x = s_snap[first + i];
            uint8_t* e = f + 4 + i * 12;
            const uint32_t gap = x.comstate == kXferSlip ? x.gapUs : x.gapUs / 32;  // a slip's comstate (recordSlip)
            e[0] = 0; e[1] = static_cast<uint8_t>(x.rx >> 24); e[2] = static_cast<uint8_t>(x.rx >> 8); e[3] = static_cast<uint8_t>(x.rx >> 16);
            e[4] = 0; e[5] = static_cast<uint8_t>(x.rx); e[6] = static_cast<uint8_t>(x.tx >> 16); e[7] = static_cast<uint8_t>(x.tx >> 24);
            e[8] = 0; e[9] = static_cast<uint8_t>((x.comstate << 4) | (gap > 15 ? 15 : gap));
            e[10] = static_cast<uint8_t>(x.tx); e[11] = static_cast<uint8_t>(x.tx >> 8);
        }
        queueFrame(kChannelData, f, 4 + count * 4);
    }
    s_lastSnapSentMs = now ? now : 1;
    s_snapPending.store(false, std::memory_order_release);
}

// The status LED, for a board with no PC to read the telemetry. Off: the adapter is off the wire. A blip every two seconds: on
// the wire, nothing heard. Slow blink: the GBA clocks it but no ID word came through (wiring, bit order). Fast blink: ID words,
// the exchange has not finished. On: the GBA found the adapter; off for a moment every second while the Switch side sends.
static void updateLed(uint32_t now)
{
#if PIN_LED >= 0
    bool lit;
    if (!s_onWire)
        lit = false;
    else if (g_core.comstate >= rfuproto::comWaitCmd)
        lit = !(s_lastNetRxMs && now - s_lastNetRxMs < 1000 && now % 1000 < 100);
    else if (g_core.dbgNintendo)
        lit = now % 250 < 125;
    else if (g_core.dbgAnyRx)
        lit = now % 1000 < 500;
    else
        lit = now % 2000 < 60;
    gpio_set_level(static_cast<gpio_num_t>(PIN_LED), lit ? 0 : 1);  // the XIAO's user LED is active low
#else
    (void)now;
#endif
}

extern "C" void rfu_adapter_start(void)
{
    if (s_task) return;
#if PIN_LED >= 0
    gpio_config_t led = {};
    led.pin_bit_mask = 1ULL << PIN_LED;
    led.mode = GPIO_MODE_OUTPUT;
    gpio_config(&led);
    gpio_set_level(static_cast<gpio_num_t>(PIN_LED), 1);
#endif
    // Above everything that could be scheduled on the second core, which then never leaves this task.
    xTaskCreatePinnedToCore(linkTask, "rfu_link", 4096, nullptr, configMAX_PRIORITIES - 1, &s_task, 1);
}

extern "C" void rfu_adapter_stop(void)
{
    s_want.store(false, std::memory_order_release);
}

extern "C" void rfu_adapter_write(const uint8_t* bytes, size_t length)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < length; ++i) feedBridgeByte(bytes[i]);
    portEXIT_CRITICAL(&s_lock);
}

extern "C" size_t rfu_adapter_read(uint8_t* out, size_t capacity)
{
    const uint32_t now = nowMs();

    uint16_t status[4];
    uint8_t statusCount;
    portENTER_CRITICAL(&s_lock);
    statusCount = s_statusCount;
    std::memcpy(status, s_statusQueue, sizeof(status));
    s_statusCount = 0;
    portEXIT_CRITICAL(&s_lock);
    for (uint8_t i = 0; i < statusCount; ++i)
    {
        const uint8_t code[2] = {static_cast<uint8_t>(status[i]), static_cast<uint8_t>(status[i] >> 8)};
        queueFrame(kChannelStatus, code, sizeof(code));
    }

    pumpOutbound();
    queueSnapshot(now);
    if (s_onWire && now - s_lastTelemetryMs >= 500)
    {
        s_lastTelemetryMs = now;
        queueTelemetry();
    }
    updateLed(now);

    const size_t n = s_outLen < capacity ? s_outLen : capacity;
    std::memcpy(out, s_out, n);
    std::memmove(s_out, s_out + n, s_outLen - n);
    s_outLen -= n;
    return n;
}
