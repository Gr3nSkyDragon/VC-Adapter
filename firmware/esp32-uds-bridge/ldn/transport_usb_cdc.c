/* transport_usb_cdc.c — ESP32-S2 native USB CDC for the UDS host link. */
#include "transport.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

void bridge_transport_open(size_t rx_buffer, size_t tx_buffer)
{
    /* TinyUSB's endpoint buffers are set by sdkconfig, not allocated per open. */
    (void)rx_buffer;
    (void)tx_buffer;
    const tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    const tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0};
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb));
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm));
}

int bridge_transport_read_wait(void *buffer, size_t length, uint32_t wait_ticks)
{
    const TickType_t started = xTaskGetTickCount();
    for (;;) {
        const uint32_t count = tud_cdc_read(buffer, length);
        if (count || xTaskGetTickCount() - started >= wait_ticks) return count;
        vTaskDelay(1);
    }
}

int bridge_transport_write_wait(const void *buffer, size_t length, uint32_t wait_ticks)
{
    const TickType_t started = xTaskGetTickCount();
    while (tud_ready()) {
        /* With DTR low, writing past available capacity can overwrite pending bytes. */
        const size_t available = tud_cdc_write_available();
        const uint32_t written = tud_cdc_write(buffer, length < available ? length : available);
        tud_cdc_write_flush();
        if (written || xTaskGetTickCount() - started >= wait_ticks) return written;
        vTaskDelay(1);
    }
    return 0;
}
