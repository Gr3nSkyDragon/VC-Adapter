#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "transport.h"
#include "ldn_control.h"
#include "ldn_udp.h"
#include "ldn_session.h"
#include "ldn_wire.h"
#include "pico_link.h"
#include "ldn_keys.h"
#include "pia_bridge.h"
#include "trade_shim.h"
#include "esp_timer.h"
#define printf ldn_wire_printf
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MACARGS(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define FRAME_MAX 1514

typedef struct { uint16_t length; uint8_t bytes[FRAME_MAX]; } control_frame_t;
static esp_netif_t *s_netif;
static QueueHandle_t s_rx;
static uint8_t s_host[6], s_mac[6];
static atomic_bool s_connected;
static int64_t s_console_us;   /* last console RX time; nonzero = a host is attached */
static int s_pending_baud;
void ldn_control_apply_baud(void);
static atomic_uint s_dropped, s_rx_count, s_tx_ok, s_tx_failed, s_air_data;

static esp_err_t control_rx(void *buffer, uint16_t length, void *eb)
{
    const uint8_t *bytes = buffer;
    if (length >= 14 && bytes[12] == 0x88 && bytes[13] == 0xb7) {
        control_frame_t frame;
        if (length <= FRAME_MAX && memcmp(bytes + 6, s_host, 6) == 0) {
            frame.length = length;
            memcpy(frame.bytes, bytes, length);
            atomic_fetch_add(&s_rx_count, 1);
            if (xQueueSend(s_rx, &frame, 0) != pdTRUE) atomic_fetch_add(&s_dropped, 1);
        }
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    return esp_netif_receive(s_netif, buffer, length, eb);
}

static void tx_done(uint8_t ifidx, uint8_t *data, uint16_t *length, bool success)
{
    if (ifidx == WIFI_IF_STA) atomic_fetch_add(success ? &s_tx_ok : &s_tx_failed, 1);
}

void ldn_control_sniff(const unsigned char *frame, size_t length)
{
    if (length >= 28 && (frame[0] & 0x0c) == 8 && !memcmp(frame + 10, s_host, 6))
        atomic_fetch_add(&s_air_data, 1);
}

/* Why the chip last started, for the host: a brownout restart means its supply dipped. */
static const char *reset_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "other";
    }
}

/* Host frames reach the Pico verbatim; both ends use the same 'GB' framing. */
/* Builds the hex text so a line is one printf; in binary mode each printf is one frame. */
static const char *hex_bytes(const uint8_t *bytes, uint8_t count)
{
    static char text[3 * 24 + 1];
    static const char digits[] = "0123456789abcdef";
    size_t o = 0;
    for (uint8_t k = 0; k < count && o + 3 < sizeof(text); ++k) {
        text[o++] = ' '; text[o++] = digits[bytes[k] >> 4]; text[o++] = digits[bytes[k] & 15];
    }
    text[o] = 0;
    return text;
}

/* Kind 7 from the host. UART port: frame for the wired GB-Link. Host port: frame
   from the client's adapter, for the bridge. */
static void host_to_pico(const uint8_t *frame, size_t length)
{
    if (pico_link_port() == PICO_PORT_HOST) pico_link_feed_host(frame, length);
    else pico_link_write(frame, length);
}

/* Bridge-to-adapter frames go to the host as kind 6. */
static void drain_host_adapter(void)
{
    static uint8_t frame[5 + PICO_LINK_MAX_PAYLOAD];
    size_t length = 0;
    for (int i = 0; i < 32 && pico_link_poll_host_out(frame, sizeof(frame), &length); ++i)
        ldn_wire_send_rfu(frame, length);
}

void ldn_control_flush_adapter(void)
{
    if (pico_link_port() == PICO_PORT_HOST) drain_host_adapter();
    bridge_transport_flush();
}

void ldn_control_init(esp_netif_t *netif, const unsigned char host[6])
{
    s_netif = netif;
    memcpy(s_host, host, 6);
    ldn_udp_init(netif, host);
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_mac));
    s_rx = xQueueCreate(8, sizeof(control_frame_t));
    ESP_ERROR_CHECK(s_rx ? ESP_OK : ESP_ERR_NO_MEM);
    bridge_transport_init();
    ESP_ERROR_CHECK(esp_wifi_set_tx_done_cb(tx_done));
    printf("LDN_READY chip=%s transport=%s heap=%" PRIu32 "\n", CONFIG_IDF_TARGET, bridge_transport_name(),
           esp_get_free_heap_size());
    /* Started at boot: a host closing the USB port resets this chip, which would
       otherwise leave the GBA without an adapter. Harmless with no Pico attached. */
    ldn_wire_set_rfu_handler(host_to_pico);
    pico_link_start();
    trade_shim_boot((int)esp_reset_reason());
    /* Standalone by default, for the same USB-close reset. LDN_BRIDGE_STOP hands
       control back for the PC-relay path. */
    pia_bridge_start();
}

void ldn_control_link(bool connected)
{
    if (connected) ESP_ERROR_CHECK(esp_wifi_internal_reg_rxcb(WIFI_IF_STA, control_rx));
    atomic_store(&s_connected, connected);
}

void ldn_control_target(const unsigned char host[6])
{
    memcpy(s_host, host, 6);
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_mac));
    ldn_udp_init(s_netif, host);
    xQueueReset(s_rx);
    atomic_store(&s_connected, false);
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void emit_line(const char *line) { printf("%s\n", line); }
static bool s_dumping_log;

static void command(const char *line)
{
    if (!strcmp(line, "LDN_BINARY") || !strcmp(line, "LDN_HELLO")) { ldn_wire_enable(); return; }
    if (!strncmp(line, "LDN_BEGIN ", 10)) {
        char *end; unsigned long id = strtoul(line + 10, &end, 16);
        if (*end || !id || strlen(line + 10) != 8) { printf("LDN_ERROR INVALID_SESSION\n"); return; }
        /* Reset the radio only if the host owns it; the bridge's room must survive a
           client attaching. */
        if (!pia_bridge_running()) ldn_session_stop();
        ldn_wire_session((uint32_t)id); printf("LDN_BEGUN\n"); return;
    }
    /* Radio commands for a host that runs the session itself. */
    if (pia_bridge_running() && (!strncmp(line, "LDN_SCAN ", 9) || !strncmp(line, "LDN_CONFIG ", 11) ||
                                 !strcmp(line, "LDN_STOP"))) {
        printf("LDN_ERROR BRIDGE_OWNS_RADIO\n"); return;
    }
    if (!strncmp(line, "LDN_SCAN ", 9)) {
        unsigned channel; char extra;
        int result = sscanf(line + 9, "%u %c", &channel, &extra) == 1 ? ldn_session_scan(channel) : ESP_ERR_INVALID_ARG;
        printf("LDN_SCAN_RESULT %d\n", result); return;
    }
    if (!strncmp(line, "LDN_CONFIG ", 11)) {
        unsigned channel; char ssid[33], bssid[18], key[33], extra;
        int result = sscanf(line + 11, "%u %32s %17s %32s %c", &channel, ssid, bssid, key, &extra) == 4
            ? ldn_session_configure(ssid, bssid, key, channel) : ESP_ERR_INVALID_ARG;
        memset(key, 0, sizeof(key));
        printf("LDN_CONFIG_RESULT %d\n", result); return;
    }
    if (!strcmp(line, "LDN_STOP")) { ldn_session_stop(); printf("LDN_STOPPED\n"); return; }
    if (!strcmp(line, "LDN_QUIET")) { printf("LDN_QUIET_OK\n"); return; }
    /* Pico link commands: start, stop and counters only. */
    if (!strcmp(line, "LDN_PICO_START")) {
        ldn_wire_set_rfu_handler(host_to_pico);
        pico_link_start();
        printf("LDN_PICO_STARTED\n"); return;
    }
    /* Key provisioning. Values go straight to NVS and are never echoed, logged or
       returned; only which names are present is visible. */
    if (!strncmp(line, "LDN_KEY ", 8)) {
        char name[40]; char hex[40];
        if (sscanf(line + 8, "%39s %39s", name, hex) != 2 || strlen(hex) != 32) {
            printf("LDN_KEY_BAD format\n"); return;
        }
        uint8_t value[16];
        for (int i = 0; i < 16; ++i) {
            int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) { printf("LDN_KEY_BAD hex\n"); return; }
            value[i] = (uint8_t)(hi << 4 | lo);
        }
        const bool ok = ldn_keys_store(name, value);
        memset(value, 0, sizeof(value));
        memset(hex, 0, sizeof(hex));
        printf(ok ? "LDN_KEY_OK %s\n" : "LDN_KEY_BAD name\n", name);
        return;
    }
    if (!strcmp(line, "LDN_KEYS")) {
        bool kek = false, gen = false, m00 = false, m12 = false;
        ldn_keys_status(&kek, &gen, &m00, &m12);
        printf("LDN_KEYS kek=%d gen=%d master00=%d master12=%d protocol1=%d protocol3=%d\n",
               kek, gen, m00, m12, ldn_keys_supports(1), ldn_keys_supports(3));
        return;
    }
    if (!strcmp(line, "LDN_KEYS_ERASE")) { ldn_keys_erase(); printf("LDN_KEYS_ERASED\n"); return; }
    if (!strcmp(line, "LDN_BRIDGE_START")) { pia_bridge_start(); printf("LDN_BRIDGE_STARTED\n"); return; }
    if (!strcmp(line, "LDN_BRIDGE_STOP")) { pia_bridge_stop(); printf("LDN_BRIDGE_STOPPED\n"); return; }
    if (!strcmp(line, "LDN_SHIM_LOG")) { trade_shim_dump_begin(); s_dumping_log = true; return; }
    if (!strcmp(line, "LDN_BRIDGE_STATUS")) {
        static char report[512];
        pia_bridge_status(report, sizeof(report));
        printf("LDN_BRIDGE_STATUS %s\n", report);
        return;
    }
    if (!strcmp(line, "LDN_PIA_TEST")) {
        extern int pia_selftest(char *out, size_t cap);
        static char report[192];
        pia_selftest(report, sizeof(report));
        printf("LDN_PIA_TEST %s\n", report);
        return;
    }
    if (!strcmp(line, "LDN_PICO_BOOTSEL")) { pico_link_bootsel(); printf("LDN_PICO_BOOTSEL_SENT\n"); return; }
    if (!strcmp(line, "LDN_PICO_DATA")) {
        for (uint8_t slot = 0; slot < 4; ++slot) {
            uint8_t frame[24];
            const uint8_t n = pico_link_frame_log(slot, frame, sizeof(frame));
            if (!n) continue;
            printf("LDN_PICO_DATA slot=%u%s\n", slot, hex_bytes(frame, n));
        }
        return;
    }
    if (!strcmp(line, "LDN_PICO_STATUS")) {
        uint16_t codes[8];
        const uint8_t n = pico_link_status_log(codes, 8);
        printf("LDN_PICO_STATUS count=%u", n);
        for (uint8_t i = 0; i < n; ++i) printf(" %04x", codes[i]);
        printf("\n");
        return;
    }
    if (!strcmp(line, "LDN_PICO_SWAP")) {
        pico_link_swap();
        printf("LDN_PICO_SWAPPED tx=%d rx=%d\n",
               pico_link_swapped() ? PICO_LINK_PIN_RX : PICO_LINK_PIN_TX,
               pico_link_swapped() ? PICO_LINK_PIN_TX : PICO_LINK_PIN_RX);
        return;
    }
    if (!strcmp(line, "LDN_PICO_PROBE")) {
        bool tx = false, rx = false;
        pico_link_probe(&tx, &rx);
        printf("LDN_PICO_PROBE pin%d_driven=%d pin%d_driven=%d\n",
               PICO_LINK_PIN_TX, tx ? 1 : 0, PICO_LINK_PIN_RX, rx ? 1 : 0);
        return;
    }
    if (!strcmp(line, "LDN_PICO_MODE")) { pico_link_set_mode(); printf("LDN_PICO_MODE_SENT\n"); return; }
    if (!strcmp(line, "LDN_PICO_STOP")) { pico_link_stop(); printf("LDN_PICO_STOPPED\n"); return; }
    if (!strcmp(line, "LDN_PICO_STATS")) {
        uint32_t rx = 0, tx = 0, bytes = 0, resync = 0, dropped = 0;
        pico_link_stats(&rx, &tx, &bytes, &resync, &dropped);
        printf("LDN_PICO_STATS running=%d rx_frames=%" PRIu32 " tx_frames=%" PRIu32
               " rx_bytes=%" PRIu32 " resync=%" PRIu32 " dropped=%" PRIu32 "\n",
               pico_link_running() ? 1 : 0, rx, tx, bytes, resync, dropped);
        printf("LDN_PICO_MODE set_mode_sent=%" PRIu32 " await_mode=%d wrong_cable=%d\n",
               pico_link_mode_sent(), pico_link_await_mode() ? 1 : 0, pico_link_wrong_cable() ? 1 : 0);
        printf("LDN_PICO_GBA active=%d\n", pico_link_gba_active() ? 1 : 0);
        uint8_t first[21];
        const uint8_t n = pico_link_first_frame(first, sizeof(first));
        if (n) {
            printf("LDN_PICO_FIRST%s\n", hex_bytes(first, n));
        }
        return;
    }
    if (!strcmp(line, "LDN_BAUD 921600") || !strcmp(line, "LDN_BAUD 115200")) {
        /* Applied after this request's DONE has left at the old rate. */
        s_pending_baud = atoi(line + 9);
        printf("LDN_BAUD_READY %s\n", line + 9); return;
    }
    /* Adapter port: GB-Link on the UART, or the host. Host needs binary mode (kinds 6/7). */
    if (!strcmp(line, "LDN_ADAPTER host") || !strcmp(line, "LDN_ADAPTER uart")) {
        const bool host = line[12] == 'h';
        if (host && !ldn_wire_active()) { printf("LDN_ERROR NOT_BINARY\n"); return; }
        if (!pico_link_set_port(host ? PICO_PORT_HOST : PICO_PORT_UART)) { printf("LDN_ERROR NO_MEMORY\n"); return; }
        printf("LDN_ADAPTER %s\n", host ? "host" : "uart");
        return;
    }
    if (!strcmp(line, "LDN_ADAPTER")) {
        printf("LDN_ADAPTER %s\n", pico_link_port() == PICO_PORT_HOST ? "host" : "uart");
        return;
    }
    /* Firmware identity, with no side effects (unlike LDN_HELLO). */
    if (!strcmp(line, "LDN_INFO")) {
        printf("LDN_INFO frlg-ldn-bridge %s chip=%s transport=%s\n", BRIDGE_VERSION, CONFIG_IDF_TARGET,
               bridge_transport_name());
        printf("LDN_BOOT reset=%s\n", reset_name(esp_reset_reason()));
        return;
    }
    if (!strcmp(line, "LDN_RF")) {
        int count, average, low, high;
        ldn_probe_rssi_window(&count, &average, &low, &high);
        printf("LDN_RF frames=%d avg=%d min=%d max=%d last=%d\n", count, average, low, high, ldn_probe_last_rssi());
        return;
    }
    if (!strncmp(line, "LDN_ANTENNA ", 12)) {
        const bool external = line[12] == '1';
        const bool took = bridge_board_antenna(external);
        printf("LDN_ANTENNA %s %s rssi=%d\n", external ? "external" : "onboard", took ? "set" : "unsupported",
               ldn_probe_last_rssi());
        return;
    }
    if (ldn_udp_command(line, atomic_load(&s_connected))) return;
    if (!strcmp(line, "LDN_STATUS")) {
        printf("LDN_LINK %u " MACSTR "\n", atomic_load(&s_connected), MACARGS(s_mac));
        printf("LDN_STATS tx_ok=%u tx_failed=%u ethernet_rx=%u air_rx=%u dropped=%u usb_dropped=%" PRIu32 " heap=%" PRIu32
               " min_heap=%" PRIu32 "\n",
            atomic_load(&s_tx_ok), atomic_load(&s_tx_failed), atomic_load(&s_rx_count), atomic_load(&s_air_data),
            atomic_load(&s_dropped), bridge_transport_dropped(), esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        return;
    }
    if (strncmp(line, "LDN_TX ", 7)) { printf("LDN_ERROR UNKNOWN_COMMAND\n"); return; }
    static uint8_t body[FRAME_MAX];
    const char *hex = line + 7;
    size_t n = strlen(hex);
    int result = ESP_ERR_INVALID_ARG;
    if (n < 10 || n > 3000 || n % 2) goto done;
    for (size_t i = 0; i < n / 2; ++i) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) goto done;
        body[i] = (uint8_t)((hi << 4) | lo);
    }
    result = ldn_control_tx_action(body, n / 2);
done:
    printf("LDN_TX_RESULT %d\n", result);
}

int ldn_control_tx_action(const uint8_t *body, size_t length)
{
    static const uint8_t prefix[] = {0x00, 0x22, 0xaa, 0x01, 0x02};
    if (length < 5 || length + 14 > FRAME_MAX || memcmp(body, prefix, sizeof(prefix))) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_connected)) return ESP_ERR_INVALID_STATE;
    static uint8_t frame[FRAME_MAX];
    memcpy(frame, s_host, 6);
    memcpy(frame + 6, s_mac, 6);
    frame[12] = 0x88; frame[13] = 0xb7;
    memcpy(frame + 14, body, length);
    return esp_wifi_internal_tx(WIFI_IF_STA, frame, 14 + length);
}

bool ldn_control_connected(void) { return atomic_load(&s_connected); }

int64_t ldn_control_console_age_ms(void)
{
    return s_console_us ? (esp_timer_get_time() - s_console_us) / 1000 : INT64_MAX;
}
void ldn_control_mac(uint8_t out[6]) { memcpy(out, s_mac, 6); }

static void (*s_action_handler)(const uint8_t source[6], const uint8_t *body, size_t length);
void ldn_control_set_action_handler(void (*handler)(const uint8_t source[6], const uint8_t *body, size_t length))
{
    s_action_handler = handler;
}

/* VC-Adapter merge: the merged firmware's 3DS side (main/bridge_mode.c) looks at everything the host sends, in text and
   binary mode alike, for a 3DS host's HELLO (Azahar, mGBA's 3DS side, uds-esp32-setup). No frame of this firmware's own can
   be one (a HELLO decodes to 10 bytes under the 3DS framing's CRC32; these frames are 16 bytes or more), and a HELLO on the
   port means this firmware's host has let go of it. A no-op otherwise. */
__attribute__((weak)) void bridge_usb_sniff(const uint8_t *bytes, size_t length)
{
    (void)bytes;
    (void)length;
}

void ldn_control_poll(void)
{
    /* A few lines per pass so the dump does not block the bridge loop. */
    if (s_dumping_log && !trade_shim_dump_step(emit_line, 8)) s_dumping_log = false;
    /* Only this task sends adapter frames to the host: the wire layer uses static
       buffers and the Pico link task runs on another core. */
    if (pico_link_port() == PICO_PORT_HOST) drain_host_adapter();
    else if (ldn_wire_active() && !pia_bridge_running()) {
        /* Drop frames queued before the host attached; a stale WrongCable would
           abort the session. */
        static bool flushed;
        if (!flushed) { pico_link_reset_inbound(); flushed = true; }
        static uint8_t pico_frame[5 + PICO_LINK_MAX_PAYLOAD];
        size_t pico_len = 0;
        for (int i = 0; i < 32 && pico_link_poll_inbound(pico_frame, sizeof(pico_frame), &pico_len); ++i)
            ldn_wire_send_rfu(pico_frame, pico_len);
    }

    static bool previous;
    bool connected = atomic_load(&s_connected);
    if (previous != connected) {
        printf("LDN_LINK %u " MACSTR "\n", connected, MACARGS(s_mac)); previous = connected;
    }
    static control_frame_t frame;
    static char hex[FRAME_MAX * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    while (xQueueReceive(s_rx, &frame, 0) == pdTRUE) {
        size_t n = frame.length - 14;
        for (size_t i = 0; i < n; ++i) { hex[2*i] = digits[frame.bytes[14+i] >> 4]; hex[2*i+1] = digits[frame.bytes[14+i] & 15]; }
        hex[2*n] = 0;
        if (s_action_handler) s_action_handler(frame.bytes + 6, frame.bytes + 14, n);
        else printf("LDN_RX " MACSTR " %s\n", MACARGS(frame.bytes + 6), hex);
    }
    static char line[3016];
    static size_t used;
    static bool overflow;
    /* Per-poll read size. Polled every 1 ms, well above console throughput; the
       rest waits in the transport ring. */
    static uint8_t bytes[2048];
    int n = bridge_transport_read(bytes, sizeof(bytes));
    if (n > 0) s_console_us = esp_timer_get_time();
    /* VC-Adapter merge: also while the binary wire is active. A host that set the wire up stays "connected" after it closes the
       port, until the board restarts, and a 3DS host that came next could not move the board to its mode. */
    if (n > 0) bridge_usb_sniff(bytes, (size_t)n);
    for (int i = 0; i < n; ++i) {
        if (ldn_wire_active()) { ldn_wire_feed(bytes[i], command); continue; }
        if (bytes[i] == '\r') continue;
        if (bytes[i] == '\n') {
            line[used] = 0;
            if (!overflow) command(line); else printf("LDN_ERROR LINE_TOO_LONG\n");
            ldn_control_apply_baud();
            used = 0; overflow = false;
        } else if (used < sizeof(line) - 1) line[used++] = bytes[i];
        else overflow = true;
    }
    ldn_udp_poll(connected);
}

void ldn_control_apply_baud(void)
{
    if (!s_pending_baud) return;
    const int baud = s_pending_baud;
    s_pending_baud = 0;
    bridge_transport_set_baud(baud);
}
