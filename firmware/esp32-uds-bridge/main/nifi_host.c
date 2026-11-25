/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
#include "nifi_host.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uds_wire.h"

#define MAX_PAYLOAD 512
#define POLL_HEADER 15 /* interval, reply_time, client_mask, duration, ack_mode, ack_delay, ack_count */
#define DONE_WAIT_US 3000
#define NOTIFY_TICK 0x01
#define NOTIFY_DONE 0x02

static const uint8_t kMpCmd[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x00};
static const uint8_t kMpAck[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x03};
static const uint8_t kMpReply[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x10};

typedef struct {
    uint32_t interval_us;
    uint16_t reply_time_us, client_mask, duration, ack_delay_us, ack_count;
    uint8_t ack_mode;
    uint8_t payload[MAX_PAYLOAD];
    size_t payload_length;
} poll_t;

static uds_event_sink_t s_sink;
static TaskHandle_t s_task;
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static poll_t s_poll, s_next; /* s_poll is the task's; s_next is copied in at its next tick */
static volatile bool s_next_pending;
static volatile bool s_active;
static uint16_t s_sequence;
/* Set by the Wi-Fi task, read by the polling task. */
static volatile int64_t s_cmd_done_us, s_ack_done_us;
static volatile uint32_t s_reply_us;
static volatile bool s_waiting_reply;
static uint8_t s_frame[24 + 4 + MAX_PAYLOAD];

static bool mac_equal(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }

/* A DS multiplayer frame from the host: data, From-DS, to `to`, from and BSSID the board's MAC. */
static size_t header(uint8_t *f, uint16_t frame_control, uint16_t duration, const uint8_t to[6])
{
    uint8_t own[6];
    radio_own_mac(own);
    put16(f, frame_control);
    put16(f + 2, duration);
    memcpy(f + 4, to, 6);
    memcpy(f + 10, own, 6);
    memcpy(f + 16, own, 6);
    put16(f + 22, (uint16_t)((s_sequence++ & 0x0FFF) << 4));
    return 24;
}

static void report(uint8_t kind, esp_err_t result, uint16_t sequence, int64_t queued, int64_t done, uint32_t reply, uint16_t fail)
{
    uint8_t event[18];
    event[0] = kind;
    event[1] = result == ESP_OK ? 0 : 1;
    put16(event + 2, sequence);
    put32(event + 4, (uint32_t)queued);
    put32(event + 8, (uint32_t)done);
    put32(event + 12, reply);
    put16(event + 16, fail);
    if (s_sink) s_sink(UDS_EVT_NIFI_TX, event, sizeof(event));
}

/* Waits for the transmit-done of the frame just queued (`done` is the variable its callback sets), at most DONE_WAIT_US. */
static int64_t wait_done(volatile int64_t *done, int64_t queued)
{
    while (!*done && esp_timer_get_time() - queued < DONE_WAIT_US) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, NOTIFY_DONE, &bits, 1);
    }
    return *done;
}

static void poll_once(void)
{
    const poll_t *p = &s_poll;
    /* The CMD. */
    size_t n = header(s_frame, 0x0228, p->duration, kMpCmd);
    const uint16_t cmd_sequence = (uint16_t)(s_sequence - 1);
    put16(s_frame + n, p->reply_time_us);
    put16(s_frame + n + 2, p->client_mask);
    memcpy(s_frame + n + 4, p->payload, p->payload_length);
    n += 4 + p->payload_length;
    s_cmd_done_us = 0;
    s_ack_done_us = 0;
    s_reply_us = 0;
    s_waiting_reply = true;
    ulTaskNotifyValueClear(NULL, NOTIFY_DONE);
    const int64_t cmd_queued = esp_timer_get_time();
    const esp_err_t cmd_result = radio_tx_nifi(s_frame, n);
    const int64_t cmd_done = cmd_result == ESP_OK ? wait_done(&s_cmd_done_us, cmd_queued) : 0;

    const uint8_t mode = p->ack_mode & ~UDS_NIFI_ACK_OPTIMISTIC;
    if (mode == UDS_NIFI_ACK_NONE) {
        s_waiting_reply = false;
        report(UDS_NIFI_TX_CMD, cmd_result, cmd_sequence, cmd_queued, cmd_done, s_reply_us, 0);
        return;
    }
    /* The ACK: after the delay, or (ACK_REPLY) as soon as a reply is heard. Busy-waiting: a tick is a millisecond. */
    const int64_t target = (cmd_done ? cmd_done : cmd_queued) + p->ack_delay_us;
    while (esp_timer_get_time() < target) {
        if (mode == UDS_NIFI_ACK_REPLY && s_reply_us) break;
    }
    const uint32_t reply = s_reply_us;
    s_waiting_reply = false;
    /* OPTIMISTIC: every client heard. An ACK early enough to land just after the reply slot goes before the board has heard the
     * reply (its capture callback comes a few hundred microseconds after), and a client told it was missed sends the same reply
     * again. */
    const uint16_t fail = (reply || (p->ack_mode & UDS_NIFI_ACK_OPTIMISTIC)) ? 0 : p->client_mask;
    n = header(s_frame, 0x0218, 0, kMpAck);
    const uint16_t ack_sequence = (uint16_t)(s_sequence - 1);
    put16(s_frame + n, p->ack_count);
    put16(s_frame + n + 2, fail);
    n += 4;
    const int64_t ack_queued = esp_timer_get_time();
    const esp_err_t ack_result = radio_tx_nifi(s_frame, n);
    const int64_t ack_done = ack_result == ESP_OK ? wait_done(&s_ack_done_us, ack_queued) : 0;
    report(UDS_NIFI_TX_CMD, cmd_result, cmd_sequence, cmd_queued, cmd_done, reply, 0);
    report(UDS_NIFI_TX_ACK, ack_result, ack_sequence, ack_queued, ack_done, reply, fail);
}

static void host_task(void *unused)
{
    (void)unused;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, NOTIFY_TICK, &bits, portMAX_DELAY);
        if (!(bits & NOTIFY_TICK)) continue;
        if (s_next_pending) {
            taskENTER_CRITICAL(&s_lock);
            s_poll = s_next;
            s_next_pending = false;
            taskEXIT_CRITICAL(&s_lock);
        }
        if (s_active && radio_running()) poll_once();
    }
}

static void tick(void *unused)
{
    (void)unused;
    if (s_task) xTaskNotify(s_task, NOTIFY_TICK, eSetBits);
}

esp_err_t nifi_host_init(uds_event_sink_t sink)
{
    s_sink = sink;
    const esp_timer_create_args_t timer_args = {.callback = tick, .name = "nifi_poll"};
    esp_err_t result = esp_timer_create(&timer_args, &s_timer);
    if (result != ESP_OK) return result;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    const BaseType_t core = 1;
#else
    const BaseType_t core = 0;
#endif
    if (xTaskCreatePinnedToCore(host_task, "nifi_host", 3072, NULL, configMAX_PRIORITIES - 2, &s_task, core) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t nifi_host_poll(const uint8_t *p, size_t length)
{
    if (length < POLL_HEADER || length - POLL_HEADER > MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    const uint32_t interval = le32(p);
    esp_timer_stop(s_timer);
    if (!interval) {
        s_active = false;
        return ESP_OK;
    }
    if (interval < 2000 || (p[10] & ~UDS_NIFI_ACK_OPTIMISTIC) > UDS_NIFI_ACK_REPLY) return ESP_ERR_INVALID_ARG;
    if (!radio_running()) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_lock);
    s_next.interval_us = interval;
    s_next.reply_time_us = le16(p + 4);
    s_next.client_mask = le16(p + 6);
    s_next.duration = le16(p + 8);
    s_next.ack_mode = p[10];
    s_next.ack_delay_us = le16(p + 11);
    s_next.ack_count = le16(p + 13);
    s_next.payload_length = length - POLL_HEADER;
    memcpy(s_next.payload, p + POLL_HEADER, s_next.payload_length);
    s_next_pending = true;
    taskEXIT_CRITICAL(&s_lock);
    s_active = true;
    return esp_timer_start_periodic(s_timer, interval);
}

void nifi_host_stop(void)
{
    if (s_timer) esp_timer_stop(s_timer);
    s_active = false;
}

void nifi_host_tx_done(const uint8_t *frame, size_t length, int64_t now_us)
{
    if (length < 10) return;
    if (mac_equal(frame + 4, kMpCmd)) s_cmd_done_us = now_us;
    else if (mac_equal(frame + 4, kMpAck)) s_ack_done_us = now_us;
    else return;
    if (s_task) xTaskNotify(s_task, NOTIFY_DONE, eSetBits);
}

void nifi_host_rx(const uint8_t *frame, size_t length, uint32_t callback_us)
{
    if (!s_waiting_reply || s_reply_us || length < 22) return;
    if ((frame[0] & 0x0C) != 0x08) return; /* data */
    uint8_t own[6];
    radio_own_mac(own);
    if (mac_equal(frame + 16, kMpReply) && mac_equal(frame + 4, own)) s_reply_us = callback_us ? callback_us : 1;
}
