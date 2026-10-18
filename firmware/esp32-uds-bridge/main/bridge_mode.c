/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
#include "bridge_mode.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "nvs.h"
#include "uds_wire.h"
#if UDS_HAVE_LDN
#include "esp_heap_caps_init.h"
#endif

#define MODE_NAMESPACE "bridge"
#define MODE_KEY "mode"

bool bridge_mode_has_ldn(void)
{
#if UDS_HAVE_LDN
    return true;
#else
    return false;
#endif
}

/* Crashes in a row (panic or watchdog resets, not the restarts the firmware asks for), kept across resets but not power-up. */
#define CRASH_MAGIC 0x6D6F6465u
#define CRASHES_BEFORE_FALLBACK 3
static RTC_NOINIT_ATTR uint32_t s_crash_magic;
static RTC_NOINIT_ATTR uint32_t s_crashes;

static void store_mode(bridge_mode_t mode)
{
    nvs_handle_t handle;
    if (nvs_open(MODE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, MODE_KEY, (uint8_t)mode);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bridge_mode_t bridge_mode_get(void)
{
    if (!bridge_mode_has_ldn()) return BRIDGE_MODE_UDS;
    uint8_t value = BRIDGE_MODE_LDN; /* the default: GB-Link Switch LDN's side */
    nvs_handle_t handle;
    if (nvs_open(MODE_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, MODE_KEY, &value);
        nvs_close(handle);
    }
    bridge_mode_t mode = value == BRIDGE_MODE_UDS ? BRIDGE_MODE_UDS : BRIDGE_MODE_LDN;

    /* A mode that keeps crashing (out of memory, say) would otherwise hold the board in a reset loop that no host can talk it out
     * of: after a few crashes in a row, the other mode is stored and booted. */
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
    if (s_crash_magic != CRASH_MAGIC || !crashed) s_crashes = 0;
    s_crash_magic = CRASH_MAGIC;
    if (crashed && ++s_crashes >= CRASHES_BEFORE_FALLBACK) {
        s_crashes = 0;
        mode = mode == BRIDGE_MODE_LDN ? BRIDGE_MODE_UDS : BRIDGE_MODE_LDN;
        store_mode(mode);
    }
    return mode;
}

void bridge_mode_switch(bridge_mode_t mode)
{
    store_mode(mode);
    esp_restart();
}

#if UDS_HAVE_LDN
/* components/ldn_bridge/linker.lf: GB-Link Switch LDN's .bss, between these two. */
extern uint8_t _ldn_bridge_bss_start[];
extern uint8_t _ldn_bridge_bss_end[];
#endif

size_t bridge_mode_reclaim_ldn_ram(esp_err_t *result)
{
    if (result) *result = ESP_ERR_NOT_SUPPORTED; /* no Switch side in this build */
#if UDS_HAVE_LDN
    /* The markers sit wherever the linker put them, not necessarily aligned (the start once followed a 40-byte string at ...c86), and
     * the heap allocator refuses a block whose start is not aligned (tlsf_create), which heap_caps_add_region reports as
     * ESP_ERR_INVALID_SIZE: 3DS mode then had no memory to start Wi-Fi. Rounding the start up and the end down keeps the block inside
     * the Switch side's own data and gives away at most a few bytes. */
    const intptr_t align = 16;
    const intptr_t start = ((intptr_t)_ldn_bridge_bss_start + align - 1) & ~(align - 1);
    const intptr_t end = (intptr_t)_ldn_bridge_bss_end & ~(align - 1);
    if (end <= start) {
        if (result) *result = ESP_ERR_INVALID_STATE; /* the markers hold nothing: linker.lf placed no data between them */
        return 0;
    }
    const esp_err_t added = heap_caps_add_region(start, end);
    if (result) *result = added;
    if (added == ESP_OK) return (size_t)(end - start);
#endif
    return 0;
}

void bridge_mode_watch_uds(const uint8_t *bytes, size_t length)
{
    /* Every GB-Link Switch LDN console command starts with "LDN_", mGBA's Switch backend opens with "\nLDN_BINARY\n". A 3DS host's
     * first frame (HELLO) has no payload, so the four letters cannot come from it. */
    static const char kMark[] = "LDN_";
    static size_t matched;
    if (!bridge_mode_has_ldn()) return;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == (uint8_t)kMark[matched]) {
            if (++matched == sizeof(kMark) - 1) bridge_mode_switch(BRIDGE_MODE_LDN);
        } else {
            matched = bytes[i] == (uint8_t)kMark[0] ? 1 : 0;
        }
    }
}

#if UDS_HAVE_LDN
/* Whether `encoded` (the bytes between two 0x00 delimiters) is this framing's HELLO: no payload, so ten raw bytes. Only that
 * frame is looked for, so this needs a few bytes of state instead of a whole decoder (uds_wire.h), which Switch LDN mode has
 * no RAM to spare for. */
static bool is_hello(const uint8_t *encoded, size_t length)
{
    uint8_t raw[UDS_WIRE_HEADER + 4];
    size_t out = 0, in = 0;
    while (in < length) { /* COBS */
        const uint8_t code = encoded[in++];
        for (uint8_t k = 1; k < code; ++k) {
            if (in >= length || out >= sizeof(raw)) return false;
            raw[out++] = encoded[in++];
        }
        if (code != 0xFF && in < length) {
            if (out >= sizeof(raw)) return false;
            raw[out++] = 0;
        }
    }
    if (out != sizeof(raw) || raw[0] != UDS_WIRE_VERSION || raw[1] != UDS_CMD_HELLO || raw[4] || raw[5]) return false;
    const uint32_t crc = (uint32_t)raw[6] | ((uint32_t)raw[7] << 8) | ((uint32_t)raw[8] << 16) | ((uint32_t)raw[9] << 24);
    return uds_crc32(raw, UDS_WIRE_HEADER) == crc;
}

/* Switch LDN mode (ldn/ldn_control.c, before its console is in binary mode): a valid HELLO frame of this firmware's own framing
 * comes from a 3DS host. Its CRC32 only checks out under this framing's header, so GB-Link Switch LDN's console text and frames
 * never look like one. */
void bridge_usb_sniff(const uint8_t *bytes, size_t length)
{
    static uint8_t encoded[16]; /* a HELLO is 11 bytes encoded */
    static size_t used;
    static bool overflow;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0) {
            if (used < sizeof(encoded)) encoded[used++] = bytes[i];
            else overflow = true;
            continue;
        }
        if (used && !overflow && is_hello(encoded, used)) bridge_mode_switch(BRIDGE_MODE_UDS);
        used = 0;
        overflow = false;
    }
}
#endif
