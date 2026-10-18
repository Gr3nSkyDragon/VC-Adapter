/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * ESP32-S3 UDS radio bridge for Azahar on Android. The phone talks to this board over the chip's
 * native USB Serial/JTAG port with the framing in uds_wire.h; the board is a raw 802.11 radio for
 * Nintendo 3DS local wireless (UDS).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bridge_mode.h"
#include "diag.h"
#include "display.h"
#include "gbwrap.h"
#include "keys.h"
#include "radio.h"
#include "uds_wire.h"

#define FW_MAJOR 1
#define FW_MINOR 7
#define PROTOCOL_VERSION UDS_WIRE_VERSION
#define RX_RING_BYTES (48 * 1024)
#define STATS_INTERVAL_US (5 * 1000 * 1000)
/* The Switch side's "LDN_" moves the board to Switch LDN mode only while no 3DS host is talking: before the first valid frame since
 * boot, and again once valid frames have stopped for this long. Opening the port does not reset the board, so without the second case
 * a board that had served a 3DS host stayed in this mode until it was power cycled. A 3DS session sends frames all the time (transfers,
 * Pico frames, radio frames), so its bytes are never mistaken for a Switch host's; GB-Link Switch LDN's clients retry about every
 * 12 s, long after this. */
#define MODE_WATCH_QUIET_US (2 * 1000 * 1000)

static RingbufHandle_t s_rx_ring;
static SemaphoreHandle_t s_write_lock;
/* The 3DS side's buffers come from the heap when it starts (app_main), so that a board in GB-Link Switch LDN's mode
 * (bridge_mode.h) does not carry them. */
static uint8_t *s_encoded;    /* UDS_WIRE_MAX_ENCODED */
static uint8_t *s_event_item; /* 1 + UDS_WIRE_MAX_PAYLOAD */
static volatile bool s_host_seen;

/* Serialized writer: the command task and the RX forwarder both send frames. */
static void send_frame(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t *payload,
                       size_t length)
{
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    size_t encoded = uds_wire_encode(type, seq, flags, payload, length, s_encoded, UDS_WIRE_MAX_ENCODED);
    size_t sent = 0;
    while (encoded && sent < encoded) {
        int written = usb_serial_jtag_write_bytes(s_encoded + sent, encoded - sent, pdMS_TO_TICKS(100));
        if (written <= 0) {
            /* The host is not reading (port closed). Drop the frame; the COBS delimiter that
             * starts the next one resynchronizes the receiver. */
            radio_note_usb_dropped();
            break;
        }
        sent += (size_t)written;
    }
    xSemaphoreGive(s_write_lock);
}

static void send_status(uint8_t request_type, uint8_t seq, int32_t result)
{
    uint8_t payload[5];
    payload[0] = request_type;
    for (int i = 0; i < 4; ++i) payload[1 + i] = (uint8_t)((uint32_t)result >> (8 * i));
    send_frame(UDS_EVT_STATUS, seq, 0, payload, sizeof(payload));
}

static void gbwrap_send(uint8_t type, uint8_t seq, const uint8_t *payload, size_t length)
{
    send_frame(type, seq, 0, payload, length);
}

/* Called from the Wi-Fi task: queue only. Ring items are {event type, payload...}. While the Game Boy wrapper runs, the frames are
 * its own and do not go to the host. */
static bool event_sink(uint8_t type, const uint8_t *payload, size_t length)
{
    if (gbwrap_active()) {
        return type == UDS_EVT_RX ? gbwrap_sink_rx(payload, length) : true;
    }
    uint8_t *item = s_event_item;
    if (length > UDS_WIRE_MAX_PAYLOAD) return false;
    item[0] = type;
    memcpy(item + 1, payload, length);
    return xRingbufferSend(s_rx_ring, item, 1 + length, 0) == pdTRUE;
}

/* diag.h's queue: a log line as a LOG frame, through the same ring and task as the radio's events, so whichever task logs never waits
 * on the USB port or its lock. A full ring drops the line. */
static void queue_log(const char *text, size_t length)
{
    uint8_t item[1 + 200];
    if (!s_rx_ring) return;
    if (length > sizeof(item) - 1) length = sizeof(item) - 1;
    item[0] = UDS_EVT_LOG;
    memcpy(item + 1, text, length);
    xRingbufferSend(s_rx_ring, item, 1 + length, 0);
}

static void rx_forward_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t length = 0;
        uint8_t *item = xRingbufferReceive(s_rx_ring, &length, pdMS_TO_TICKS(50));
        if (item) {
            send_frame(item[0], 0, 0, item + 1, length - 1);
            vRingbufferReturnItem(s_rx_ring, item);
        }
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_US / 1000));
        uds_stats_t stats;
        radio_get_stats(&stats);
        uint8_t payload[36];
        const uint32_t values[9] = {stats.rx_seen,  stats.rx_forwarded, stats.rx_dropped,
                                    stats.tx_ok,    stats.tx_failed,    stats.beacons_sent,
                                    stats.usb_dropped, stats.tx_unicast, stats.acks_rx};
        for (int i = 0; i < 9; ++i)
            for (int b = 0; b < 4; ++b) payload[i * 4 + b] = (uint8_t)(values[i] >> (8 * b));
        send_frame(UDS_EVT_STATS, 0, 0, payload, sizeof(payload));
    }
}

/* The screen's snapshot (display.h): the wrapper when it runs, otherwise the raw radio, otherwise idle. */
static void display_status(scene_status_t *status)
{
    status->fw_major = FW_MAJOR;
    status->fw_minor = FW_MINOR;
    status->host_seen = s_host_seen;
    gbwrap_status_t wrapper;
    gbwrap_get_status(&wrapper);
    if (wrapper.active) {
        status->mode = SCENE_WRAPPER;
        status->gen = wrapper.gen;
        status->stage = wrapper.stage;
        status->channel = wrapper.channel;
        status->rx = wrapper.frames_received;
        status->tx = wrapper.frames_sent;
        memcpy(status->title, wrapper.title, sizeof(status->title));
    } else if (radio_running()) {
        uds_stats_t stats;
        radio_get_stats(&stats);
        status->mode = SCENE_RADIO;
        status->channel = radio_channel();
        status->rx = stats.rx_forwarded;
        status->tx = stats.tx_ok;
    }
}

static void handle_command(uds_wire_frame_t *frame)
{
    const uint8_t *p = frame->payload;
    const size_t n = frame->length;
    esp_err_t result = ESP_OK;

    switch (frame->type) {
    case UDS_CMD_GB_START:
    case UDS_CMD_GB_XFER:
    case UDS_CMD_GB_STOP:
    case UDS_CMD_GB_PICO:
        gbwrap_request(frame->type, frame->seq, p, n); /* the wrapper task answers */
        return;
    case UDS_CMD_SET_KEY:
        result = n == 17 ? keys_store(p[0], p + 1) : ESP_ERR_INVALID_SIZE;
        memset((uint8_t *)frame->payload, 0, n); /* the key is not kept anywhere but in the key store (the payload is the decoder's) */
        send_status(frame->type, frame->seq, (int32_t)result);
        return;
    case UDS_CMD_KEY_STATUS: {
        /* Empty: the 3DS key, as before. One byte: that slot (a Switch key's, say). */
        const uint8_t slot = n >= 1 ? p[0] : UDS_KEY_SLOT_DATA;
        uint8_t payload[2] = {slot, keys_present(slot)};
        send_frame(UDS_EVT_KEY_INFO, frame->seq, 0, payload, sizeof(payload));
        return;
    }
    case UDS_CMD_ERASE_KEYS:
        send_status(frame->type, frame->seq, (int32_t)keys_erase());
        return;
    case UDS_CMD_STOP:
        if (gbwrap_active()) {
            gbwrap_request(frame->type, frame->seq, p, n);
            return;
        }
        break; /* the radio's own STOP, below */
    case UDS_CMD_START:
    case UDS_CMD_SET_CHANNEL:
    case UDS_CMD_TX_FRAME:
    case UDS_CMD_SET_BEACON:
    case UDS_CMD_SET_WATCH:
        if (gbwrap_active()) {
            /* The radio belongs to the Game Boy wrapper until GB_STOP. */
            if (frame->type != UDS_CMD_TX_FRAME) send_status(frame->type, frame->seq, ESP_ERR_INVALID_STATE);
            return;
        }
        break;
    default:
        break;
    }

    switch (frame->type) {
    case UDS_CMD_HELLO: {
        s_host_seen = true;
        gbwrap_host_hello(); /* a wrapper the board started on its own (a GB-Link on the UART) gives the board to the host */
        display_wake();
        diag_report("hello"); /* a host that opened the port after boot gets the memory figures too */
        uint8_t payload[9] = {PROTOCOL_VERSION, FW_MAJOR, FW_MINOR};
        esp_efuse_mac_get_default(payload + 3);
        send_frame(UDS_EVT_HELLO_ACK, frame->seq, 0, payload, sizeof(payload));
        return;
    }
    case UDS_CMD_START:
        /* {channel, mac[6]} plus an optional flags byte: bit 0 = decoy hardware MAC. */
        result = (n == 7 || n == 8) ? radio_start(p[0], p + 1, n == 8 && (p[7] & 1))
                                    : ESP_ERR_INVALID_SIZE;
        break;
    case UDS_CMD_STOP:
        result = radio_stop();
        break;
    case UDS_CMD_SET_CHANNEL:
        result = n == 1 ? radio_set_channel(p[0]) : ESP_ERR_INVALID_SIZE;
        break;
    case UDS_CMD_TX_FRAME:
        /* {tx_flags, rate500k, mpdu...}. Fire-and-forget: no Status, so the host can stream
         * frames; failures show up as tx_failed in the periodic stats. The driver decides from
         * the address whether a frame expects an ACK, so UDS_TX_NO_ACK is informational. */
        if (n >= 2 + 10) (void)radio_tx(p + 2, n - 2, p[1]);
        return;
    case UDS_CMD_SET_BEACON:
        result = radio_set_beacon(p, n);
        break;
    case UDS_CMD_SET_WATCH:
        if (n == 6) radio_set_watch(p); else result = ESP_ERR_INVALID_SIZE;
        break;
    case UDS_CMD_PING:
        send_frame(UDS_EVT_PONG, frame->seq, 0, p, n);
        return;
    default:
        result = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    send_status(frame->type, frame->seq, (int32_t)result);
}

#if UDS_HAVE_LDN
void ldn_app_main(void); /* ldn/ldn_probe.c: GB-Link Switch LDN's bridge; does not return */
#endif

void app_main(void)
{
    keys_init(); /* NVS, which also holds the boot mode */
#if UDS_HAVE_LDN
    if (bridge_mode_get() == BRIDGE_MODE_LDN) {
        ldn_app_main();
        return;
    }
    /* The Switch side's static data is idle in this mode: it becomes heap for this side's buffers and Wi-Fi. */
    esp_err_t reclaim_result;
    const size_t reclaimed = bridge_mode_reclaim_ldn_ram(&reclaim_result);
    diag_set_reclaimed(reclaimed, reclaim_result);
#endif
    s_write_lock = xSemaphoreCreateMutex();
    s_rx_ring = xRingbufferCreate(RX_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    s_encoded = malloc(UDS_WIRE_MAX_ENCODED);
    s_event_item = malloc(1 + UDS_WIRE_MAX_PAYLOAD);
    uds_wire_decoder_t *decoder = malloc(sizeof(*decoder));
    configASSERT(s_encoded && s_event_item && decoder);

    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_config.rx_buffer_size = 16384;
    usb_config.tx_buffer_size = 16384;
    usb_serial_jtag_driver_install(&usb_config);

    /* The ROM boot banner and anything else that reached the port ends at a newline; a bare
     * delimiter first makes the host discard it as an idle/garbage frame. */
    const uint8_t delimiter = 0;
    usb_serial_jtag_write_bytes(&delimiter, 1, pdMS_TO_TICKS(50));

    /* From here ESP-IDF's log (Wi-Fi's included) reaches the host as LOG frames, never as text on the port (diag.h). */
    diag_start(queue_log);
    diag_report("boot");

    radio_init(event_sink);
    gbwrap_init(gbwrap_send);
    display_start(display_status); /* optional: without a screen this does nothing */

    xTaskCreate(rx_forward_task, "rx_fwd", 4096, NULL, 6, NULL);
    xTaskCreate(stats_task, "stats", 3072, NULL, 2, NULL);

    uds_wire_decoder_init(decoder);
    uint8_t chunk[256];
    int64_t last_frame_us = 0; /* when the last valid 3DS host frame came; 0: none since boot */
    for (;;) {
        int count = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (count > 0 && (!last_frame_us || esp_timer_get_time() - last_frame_us >= MODE_WATCH_QUIET_US)) {
            bridge_mode_watch_uds(chunk, (size_t)count);
        }
        for (int i = 0; i < count; ++i) {
            uds_wire_frame_t frame;
            if (uds_wire_feed(decoder, chunk[i], &frame)) {
                last_frame_us = esp_timer_get_time();
                handle_command(&frame);
            }
        }
    }
}
