#include "transport.h"
#include "sdkconfig.h"
#include "driver/uart.h"
/* VC-Adapter merge: ESP-IDF 5.x builds (the 3DS side alone) have no _vfs header; only bridge_transport_init needs it. */
#if __has_include("driver/uart_vfs.h")
#include "driver/uart_vfs.h"
#define BRIDGE_HAVE_VFS_HEADER 1
#endif
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
/* VC-Adapter merge: the ring sizes are the caller's (the 3DS side picks its own, bridge_transport_open). */
typedef struct {
    SemaphoreHandle_t done;
    int rx_buffer, tx_buffer;
} install_args_t;

static void install(void *arg)
{
    const install_args_t *args = arg;
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, args->rx_buffer, args->tx_buffer, 0, NULL, 0));
    const uart_intr_config_t interrupts = {
        .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M | UART_RXFIFO_TOUT_INT_ENA_M,
        .rxfifo_full_thresh = 32,
        .rx_timeout_thresh = 2,
    };
    ESP_ERROR_CHECK(uart_intr_config(CONFIG_ESP_CONSOLE_UART_NUM, &interrupts));
    xSemaphoreGive(args->done);
    vTaskDelete(NULL);
}

static void install_driver(int rx_buffer, int tx_buffer)
{
    static StaticSemaphore_t done_space;
    install_args_t args = {.done = xSemaphoreCreateBinaryStatic(&done_space), .rx_buffer = rx_buffer, .tx_buffer = tx_buffer};
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    xTaskCreatePinnedToCore(install, "console_init", 3072, &args, 5, NULL, 1);
#else
    xTaskCreate(install, "console_init", 3072, &args, 5, NULL);
#endif
    xSemaphoreTake(args.done, portMAX_DELAY);
}

void bridge_transport_init(void)
{
    install_driver(4096, 4096);
#if BRIDGE_HAVE_VFS_HEADER
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
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

/* VC-Adapter merge: the 3DS side's calls (transport.h). The console UART runs at CONFIG_ESP_CONSOLE_UART_BAUDRATE from boot. */
void bridge_transport_open(size_t rx_buffer, size_t tx_buffer)
{
    install_driver((int)rx_buffer, (int)tx_buffer);
}

int bridge_transport_read_wait(void *buffer, size_t length, uint32_t wait_ticks)
{
    /* uart_read_bytes waits until all `length` bytes have come: wait for one, then take what else is there, as USB Serial/JTAG's
       read does. */
    if (!length) return 0;
    size_t buffered = 0;
    uart_get_buffered_data_len(CONFIG_ESP_CONSOLE_UART_NUM, &buffered);
    if (buffered) return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, buffered < length ? buffered : length, 0);
    int count = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, 1, wait_ticks);
    if (count <= 0) return count;
    uart_get_buffered_data_len(CONFIG_ESP_CONSOLE_UART_NUM, &buffered);
    if (buffered && length > 1) {
        const int more = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, (uint8_t *)buffer + 1,
                                         buffered < length - 1 ? buffered : length - 1, 0);
        if (more > 0) count += more;
    }
    return count;
}

int bridge_transport_write_wait(const void *buffer, size_t length, uint32_t wait_ticks)
{
    /* The UART sends at its own rate whether or not anyone listens: this waits only while the TX ring is full. */
    (void)wait_ticks;
    return uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, length);
}
