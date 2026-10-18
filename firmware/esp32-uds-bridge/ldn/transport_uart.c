#include "transport.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "soc/uart_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"

static uint32_t dropped;

/* Console traffic is a few KB/s, read every 1 ms. 4 KiB rings keep RAM free for the
   GB-Link UART on the original ESP32, the smallest of the four chips. While the page
   carries the adapter's frames they arrive here at 921600 baud, and the GBA never sends
   a frame twice. The default RX threshold (120 of the 128-byte FIFO) leaves under
   0.1 ms, which a busy Wi-Fi core overruns: the driver is installed from the other core,
   where its interrupt then runs, and drains the FIFO at 32 bytes. */
static void install(void *arg)
{
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 4096, 4096, 0, NULL, 0));
    const uart_intr_config_t interrupts = {
        .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M | UART_RXFIFO_TOUT_INT_ENA_M,
        .rxfifo_full_thresh = 32,
        .rx_timeout_thresh = 2,
    };
    ESP_ERROR_CHECK(uart_intr_config(CONFIG_ESP_CONSOLE_UART_NUM, &interrupts));
    xSemaphoreGive(arg);
    vTaskDelete(NULL);
}

void bridge_transport_init(void)
{
    static StaticSemaphore_t done_space;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&done_space);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    xTaskCreatePinnedToCore(install, "console_init", 3072, done, 5, NULL, 1);
#else
    xTaskCreate(install, "console_init", 3072, done, 5, NULL);
#endif
    xSemaphoreTake(done, portMAX_DELAY);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
}

int bridge_transport_read(void *buffer, size_t length)
{
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, length, 0);
}

void bridge_transport_write(const void *buffer, size_t length)
{
    /* Blocks only while the 4 KiB TX ring is full. */
    int written = uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, length);
    if (written != (int)length) ++dropped;
}

uint32_t bridge_transport_dropped(void) { return dropped; }
const char *bridge_transport_name(void) { return "UART"; }

void bridge_transport_set_baud(int baud)
{
    uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(1000));
    uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, baud);
}
void bridge_transport_flush(void) { uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(200)); }
