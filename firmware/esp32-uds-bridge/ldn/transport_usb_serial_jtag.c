#include "transport.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

static uint32_t dropped;

void bridge_transport_init(void)
{
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = CONFIG_BRIDGE_USB_RX_BUFFER,
                                              .tx_buffer_size = CONFIG_BRIDGE_USB_TX_BUFFER};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();
}

int bridge_transport_read(void *buffer, size_t length)
{
    return usb_serial_jtag_read_bytes(buffer, length, 0);
}

void bridge_transport_write(const void *buffer, size_t length)
{
    /* Timeout in case the host closed the port; the next COBS delimiter resyncs RX. */
    int written = usb_serial_jtag_write_bytes(buffer, length, pdMS_TO_TICKS(100));
    if (written != (int)length) ++dropped;
}

uint32_t bridge_transport_dropped(void) { return dropped; }
const char *bridge_transport_name(void) { return "USB Serial/JTAG"; }
void bridge_transport_set_baud(int baud) { (void)baud; }
void bridge_transport_flush(void) { usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200)); }
