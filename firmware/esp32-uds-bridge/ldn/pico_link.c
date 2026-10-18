#include "pico_link.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "soc/uart_reg.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PICO_LINK_UART UART_NUM_1
#define SYNC0 0x47 /* 'G' */
#define SYNC1 0x42 /* 'B' */
#define HEADER_LENGTH 5
/* In wireless mode the adapter reports twice a second to whichever side spoke last.
   Silence this long means it left the mode for this board (restart, late power-up,
   or now answering a USB host). */
#define ADAPTER_SILENCE_MS 3000

typedef struct
{
    uint16_t length;
    uint8_t bytes[HEADER_LENGTH + PICO_LINK_MAX_PAYLOAD];
} pico_frame_t;

typedef enum { RX_SYNC0, RX_SYNC1, RX_CHANNEL, RX_LEN_LO, RX_LEN_HI, RX_PAYLOAD } rx_state_t;

static TaskHandle_t s_task;
static QueueHandle_t s_inbound;
static QueueHandle_t s_host_out;
static volatile pico_port_t s_port = PICO_PORT_UART;
static volatile bool s_running;

static rx_state_t s_state = RX_SYNC0;
static pico_frame_t s_partial;
static uint16_t s_payloadLength;
static uint16_t s_payloadSeen;

static uint32_t s_rxFrames, s_txFrames, s_rxBytes, s_resync, s_dropped;
static uint32_t s_modeSent;
static bool s_swapped;
static volatile bool s_awaitMode;
static volatile bool s_wrongCable;
static volatile bool s_gbaActive;
static volatile TickType_t s_lastFrame;
static uint8_t s_firstFrame[HEADER_LENGTH + 16];
static uint8_t s_firstFrameLength;
static uint16_t s_statusLog[8];
static uint8_t s_statusCount;
#define FRAME_LOG_SLOTS 4
#define FRAME_LOG_BYTES 24
static uint8_t s_frameLog[FRAME_LOG_SLOTS][FRAME_LOG_BYTES];
static uint8_t s_frameLogLength[FRAME_LOG_SLOTS];
static uint8_t s_frameLogCount;

static void frame_complete(void)
{
    ++s_rxFrames;
    s_lastFrame = xTaskGetTickCount();
    if (s_firstFrameLength == 0)
    {
        const uint16_t copy = s_partial.length < sizeof(s_firstFrame) ? s_partial.length : sizeof(s_firstFrame);
        memcpy(s_firstFrame, s_partial.bytes, copy);
        s_firstFrameLength = (uint8_t)copy;
    }
    if (s_partial.bytes[2] == PICO_LINK_CHANNEL_STATUS && s_partial.length >= HEADER_LENGTH + 2)
    {
        const uint16_t code = (uint16_t)(s_partial.bytes[HEADER_LENGTH] |
                                         ((uint16_t)s_partial.bytes[HEADER_LENGTH + 1] << 8));
        const uint8_t slots = (uint8_t)(sizeof(s_statusLog) / sizeof(s_statusLog[0]));
        s_statusLog[s_statusCount % slots] = code;
        ++s_statusCount;
        if (code == 0xFF02) { s_awaitMode = true; s_wrongCable = false; }
        else if (code == 0xFF0D) { s_wrongCable = true; s_awaitMode = false; }
    }
    if (s_partial.bytes[2] == PICO_LINK_CHANNEL_DATA)
    {
        if (s_partial.length >= HEADER_LENGTH + 4 && s_partial.bytes[HEADER_LENGTH] == 0x0E)
            s_gbaActive = s_partial.bytes[HEADER_LENGTH + 1] != 0;   /* dbgAnyRx */

        const uint8_t slot = s_frameLogCount % FRAME_LOG_SLOTS;
        const uint8_t copy = s_partial.length < FRAME_LOG_BYTES ? (uint8_t)s_partial.length : FRAME_LOG_BYTES;
        memcpy(s_frameLog[slot], s_partial.bytes, copy);
        s_frameLogLength[slot] = copy;
        ++s_frameLogCount;
    }
    if (!s_inbound) return;
    /* Queue full: drop the oldest frame, keep the newest. */
    if (xQueueSend(s_inbound, &s_partial, 0) != pdTRUE)
    {
        pico_frame_t discard;
        if (xQueueReceive(s_inbound, &discard, 0) == pdTRUE) ++s_dropped;
        if (xQueueSend(s_inbound, &s_partial, 0) != pdTRUE) ++s_dropped;
    }
}

static void feed(uint8_t byte)
{
    ++s_rxBytes;
    switch (s_state)
    {
    case RX_SYNC0:
        if (byte == SYNC0) { s_partial.bytes[0] = byte; s_state = RX_SYNC1; }
        else ++s_resync;
        return;
    case RX_SYNC1:
        if (byte == SYNC1) { s_partial.bytes[1] = byte; s_state = RX_CHANNEL; }
        else { ++s_resync; s_state = (byte == SYNC0) ? RX_SYNC1 : RX_SYNC0; }
        return;
    case RX_CHANNEL:
        s_partial.bytes[2] = byte; s_state = RX_LEN_LO; return;
    case RX_LEN_LO:
        s_partial.bytes[3] = byte; s_payloadLength = byte; s_state = RX_LEN_HI; return;
    case RX_LEN_HI:
        s_partial.bytes[4] = byte;
        s_payloadLength |= (uint16_t)byte << 8;
        if (s_payloadLength > PICO_LINK_MAX_PAYLOAD) { ++s_resync; s_state = RX_SYNC0; return; }
        s_payloadSeen = 0;
        if (s_payloadLength == 0) { s_partial.length = HEADER_LENGTH; frame_complete(); s_state = RX_SYNC0; return; }
        s_state = RX_PAYLOAD;
        return;
    case RX_PAYLOAD:
        s_partial.bytes[HEADER_LENGTH + s_payloadSeen++] = byte;
        if (s_payloadSeen >= s_payloadLength)
        {
            s_partial.length = (uint16_t)(HEADER_LENGTH + s_payloadLength);
            frame_complete();
            s_state = RX_SYNC0;
        }
        return;
    }
}

/* The Pico idles in no mode and replies on whichever side spoke last, so this end
   must speak first. Retried because boot order is not fixed. */
static void send_set_mode(void)
{
    static const uint8_t set_mode_rfu[] = {0x00, 0x07, 0x00};
    if (pico_link_send(PICO_LINK_CHANNEL_COMMAND, set_mode_rfu, sizeof(set_mode_rfu))) ++s_modeSent;
}

/* Frames queued each way. The original ESP32 hosts a room with little heap to spare, and
   a Wi-Fi driver short of heap loses frames to the Switch; a few frames is all a queue
   holds in practice. */
#if CONFIG_IDF_TARGET_ESP32
#define FRAME_QUEUE 32
#else
#define FRAME_QUEUE 48
#endif

static bool s_uart_installed;

/* Installed from the link task so the ISR runs on that core, away from Wi-Fi.
   RX threshold 32 of the 128-byte FIFO: at 921600 baud a near-full threshold leaves
   <0.1 ms, which a busy Wi-Fi core overruns. While the page carries the adapter's frames
   (the host port) the UART is idle and its driver's buffers go back to the heap. */
static bool install_driver(void)
{
    if (s_uart_installed) return true;
    const uart_config_t config = {
        .baud_rate = PICO_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(PICO_LINK_UART, 4096, 4096, 0, NULL, 0) != ESP_OK) return false;
    s_uart_installed = true;
    ESP_ERROR_CHECK(uart_param_config(PICO_LINK_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(PICO_LINK_UART,
                                 s_swapped ? PICO_LINK_PIN_RX : PICO_LINK_PIN_TX,
                                 s_swapped ? PICO_LINK_PIN_TX : PICO_LINK_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    const uart_intr_config_t interrupts = {
        .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M | UART_RXFIFO_TOUT_INT_ENA_M,
        .rxfifo_full_thresh = 32,
        .rx_timeout_thresh = 2,
    };
    ESP_ERROR_CHECK(uart_intr_config(PICO_LINK_UART, &interrupts));
    return true;
}

static void remove_driver(void)
{
    if (!s_uart_installed) return;
    uart_driver_delete(PICO_LINK_UART);
    s_uart_installed = false;
}

static void link_task(void *arg)
{
    SemaphoreHandle_t ready = arg;
    if (s_port != PICO_PORT_HOST) ESP_ERROR_CHECK(install_driver() ? ESP_OK : ESP_ERR_NO_MEM);
    xSemaphoreGive(ready);
    uint8_t buffer[256];
    /* Delay the first SetMode. A Pico already in wireless mode sends telemetry, and
       re-entering the mode would reset an adapter the GBA already found. */
    TickType_t nextMode = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
    while (s_running)
    {
        /* Host port: frames are fed by the console task. */
        if (s_port == PICO_PORT_HOST)
        {
            remove_driver();
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        else if (!install_driver())
            vTaskDelay(pdMS_TO_TICKS(100));   /* retried until the heap has room */
        else
        {
            const int read = uart_read_bytes(PICO_LINK_UART, buffer, sizeof(buffer), pdMS_TO_TICKS(2));
            for (int i = 0; i < read; ++i) feed(buffer[i]);

            /* Silent adapter has left the mode: clear state and resume SetMode, so
               power-up order does not matter. Checked after the read so frames queued
               during a task stall still count. */
            if ((s_awaitMode || s_gbaActive) &&
                (TickType_t)(xTaskGetTickCount() - s_lastFrame) > pdMS_TO_TICKS(ADAPTER_SILENCE_MS))
            {
                s_awaitMode = false;
                s_gbaActive = false;
            }
        }

        /* Retry until AwaitMode. Mode entry samples the cable, and a powered GBA
           driving SO can make it answer WrongCable. Same schedule as the PC host. */
        /* Telemetry arrives even with no mode (all zero), so frames do not imply a
           mode. Skip only while a GBA is clocking the adapter; re-entry strands it. */
        if ((!s_awaitMode && !s_gbaActive) || s_wrongCable)
        {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(now - nextMode) >= 0)
            {
                send_set_mode();
                nextMode = now + pdMS_TO_TICKS(s_rxFrames ? 2000 : 500);
            }
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void pico_link_start(void)
{
    if (s_running) return;

    if (!s_inbound) s_inbound = xQueueCreate(FRAME_QUEUE, sizeof(pico_frame_t));
    ESP_ERROR_CHECK(s_inbound ? ESP_OK : ESP_ERR_NO_MEM);

    s_state = RX_SYNC0;
    s_running = true;
    static StaticSemaphore_t ready_space;
    SemaphoreHandle_t ready = xSemaphoreCreateBinaryStatic(&ready_space);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    /* Off the core running Wi-Fi and the Pia tick. */
    xTaskCreatePinnedToCore(link_task, "pico_link", 4096, ready, 10, &s_task, 1);
#else
    xTaskCreate(link_task, "pico_link", 4096, ready, 10, &s_task);
#endif
    xSemaphoreTake(ready, portMAX_DELAY);
}

void pico_link_stop(void)
{
    if (!s_running) return;
    s_running = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(2));
    remove_driver();
}

bool pico_link_running(void) { return s_running; }

bool pico_link_write(const uint8_t *bytes, size_t length)
{
    if (!s_running || !bytes || !length) return false;
    if (s_port == PICO_PORT_HOST)
    {
        /* Queued for the console task, the only wire-layer user. */
        if (length > sizeof(((pico_frame_t *)0)->bytes) || !s_host_out) return false;
        pico_frame_t frame;
        frame.length = (uint16_t)length;
        memcpy(frame.bytes, bytes, length);
        return xQueueSend(s_host_out, &frame, 0) == pdTRUE;
    }
    if (!s_uart_installed) return false;
    return uart_write_bytes(PICO_LINK_UART, bytes, length) == (int)length;
}

bool pico_link_set_port(pico_port_t port)
{
    if (port == s_port) return true;
    /* Outbound queue for the host port, allocated on first use. */
    if (port == PICO_PORT_HOST && !s_host_out)
    {
        s_host_out = xQueueCreate(FRAME_QUEUE, sizeof(pico_frame_t));
        if (!s_host_out) return false;
    }
    s_port = port;
    /* Adapter state is per port. */
    s_state = RX_SYNC0;
    s_awaitMode = false;
    s_wrongCable = false;
    s_gbaActive = false;
    s_firstFrameLength = 0;
    pico_link_reset_inbound();
    pico_frame_t discard;
    while (s_host_out && xQueueReceive(s_host_out, &discard, 0) == pdTRUE) { }
    return true;
}

pico_port_t pico_link_port(void) { return s_port; }

void pico_link_feed_host(const uint8_t *bytes, size_t length)
{
    if (s_port != PICO_PORT_HOST) return;
    for (size_t i = 0; i < length; ++i) feed(bytes[i]);
}

bool pico_link_poll_host_out(uint8_t *frame, size_t capacity, size_t *length)
{
    pico_frame_t next;
    if (!s_host_out || xQueueReceive(s_host_out, &next, 0) != pdTRUE) return false;
    if (next.length > capacity) return false;
    memcpy(frame, next.bytes, next.length);
    *length = next.length;
    return true;
}

bool pico_link_send(uint8_t channel, const uint8_t *payload, size_t length)
{
    if (length > PICO_LINK_MAX_PAYLOAD) return false;
    uint8_t frame[HEADER_LENGTH + PICO_LINK_MAX_PAYLOAD];
    frame[0] = SYNC0;
    frame[1] = SYNC1;
    frame[2] = channel;
    frame[3] = (uint8_t)(length & 0xff);
    frame[4] = (uint8_t)(length >> 8);
    if (length) memcpy(frame + HEADER_LENGTH, payload, length);
    if (!pico_link_write(frame, HEADER_LENGTH + length)) return false;
    ++s_txFrames;
    return true;
}

bool pico_link_poll_inbound(uint8_t *frame, size_t capacity, size_t *length)
{
    if (!s_inbound || !frame || !length) return false;
    pico_frame_t held;
    if (xQueueReceive(s_inbound, &held, 0) != pdTRUE) return false;
    if (held.length > capacity) return false;
    memcpy(frame, held.bytes, held.length);
    *length = held.length;
    return true;
}

void pico_link_set_mode(void) { send_set_mode(); }

void pico_link_flush(void) { uart_wait_tx_done(PICO_LINK_UART, pdMS_TO_TICKS(200)); }

void pico_link_stats(uint32_t *rx_frames, uint32_t *tx_frames, uint32_t *rx_bytes,
                     uint32_t *resync, uint32_t *dropped)
{
    if (rx_frames) *rx_frames = s_rxFrames;
    if (tx_frames) *tx_frames = s_txFrames;
    if (rx_bytes) *rx_bytes = s_rxBytes;
    if (resync) *resync = s_resync;
    if (dropped) *dropped = s_dropped;
}

uint32_t pico_link_mode_sent(void) { return s_modeSent; }
bool pico_link_await_mode(void) { return s_awaitMode; }
bool pico_link_wrong_cable(void) { return s_wrongCable; }
bool pico_link_gba_active(void) { return s_gbaActive; }

/* Drop frames buffered before a consumer attached, e.g. a stale WrongCable from a
   mode entry since retried. */
void pico_link_reset_inbound(void)
{
    if (!s_inbound) return;
    pico_frame_t discard;
    while (xQueueReceive(s_inbound, &discard, 0) == pdTRUE) { }
}

bool pico_link_swapped(void) { return s_swapped; }
/* Reboot the Pico into its USB bootloader. Its command channel is this UART, so
   without this a reflash needs BOOTSEL held. */
void pico_link_bootsel(void)
{
    static const uint8_t reboot_bootloader[] = {0x43};
    pico_link_send(PICO_LINK_CHANNEL_COMMAND, reboot_bootloader, sizeof(reboot_bootloader));
}


/* Fixes a reversed TX/RX pair at runtime via the GPIO matrix. */
void pico_link_swap(void)
{
    const bool wasRunning = s_running;
    if (wasRunning) pico_link_stop();
    s_swapped = !s_swapped;
    s_rxFrames = s_txFrames = s_rxBytes = s_resync = s_dropped = s_modeSent = 0;
    s_firstFrameLength = 0;
    if (wasRunning) pico_link_start();
}

/* Idle UART lines sit high. A pin reading high against a pull-down (or low against
   a pull-up) is driven; one that follows both pulls is unwired. */
void pico_link_probe(bool *tx_driven, bool *rx_driven)
{
    const bool wasRunning = s_running;
    if (wasRunning) pico_link_stop();

    const int pins[2] = {PICO_LINK_PIN_TX, PICO_LINK_PIN_RX};
    bool driven[2] = {false, false};
    for (int i = 0; i < 2; ++i)
    {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(5));
        const int pulledDown = gpio_get_level((gpio_num_t)pins[i]);

        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(5));
        const int pulledUp = gpio_get_level((gpio_num_t)pins[i]);

        driven[i] = (pulledDown == 1) || (pulledUp == 0);
    }
    if (tx_driven) *tx_driven = driven[0];
    if (rx_driven) *rx_driven = driven[1];

    if (wasRunning) pico_link_start();
}


uint8_t pico_link_frame_log(uint8_t slot, uint8_t *out, uint8_t capacity)
{
    if (slot >= FRAME_LOG_SLOTS) return 0;
    const uint8_t held = s_frameLogLength[slot];
    const uint8_t copy = held < capacity ? held : capacity;
    if (out && copy) memcpy(out, s_frameLog[slot], copy);
    return copy;
}

uint8_t pico_link_status_log(uint16_t *out, uint8_t capacity)
{
    const uint8_t slots = (uint8_t)(sizeof(s_statusLog) / sizeof(s_statusLog[0]));
    const uint8_t held = s_statusCount < slots ? s_statusCount : slots;
    const uint8_t copy = held < capacity ? held : capacity;
    for (uint8_t i = 0; i < copy; ++i)
    {
        const uint8_t oldest = (uint8_t)((s_statusCount >= slots ? s_statusCount : 0) % slots);
        out[i] = s_statusLog[(oldest + i) % slots];
    }
    return copy;
}

uint8_t pico_link_first_frame(uint8_t *out, uint8_t capacity)
{
    const uint8_t copy = s_firstFrameLength < capacity ? s_firstFrameLength : capacity;
    if (out && copy) memcpy(out, s_firstFrame, copy);
    return copy;
}
