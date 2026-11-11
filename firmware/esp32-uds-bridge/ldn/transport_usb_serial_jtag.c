#include "transport.h"
#include "driver/usb_serial_jtag.h"
/* VC-Adapter merge: ESP-IDF 5.x builds (the 3DS side alone) have no _vfs header; only bridge_transport_init needs it. */
#if __has_include("driver/usb_serial_jtag_vfs.h")
#include "driver/usb_serial_jtag_vfs.h"
#define BRIDGE_HAVE_VFS_HEADER 1
#endif
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

static uint32_t dropped;

void bridge_transport_init(void)
{
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = CONFIG_BRIDGE_USB_RX_BUFFER,
                                              .tx_buffer_size = CONFIG_BRIDGE_USB_TX_BUFFER};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
#if BRIDGE_HAVE_VFS_HEADER
    usb_serial_jtag_vfs_use_driver();
#endif
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

/* VC-Adapter merge: the 3DS side's calls (transport.h), as main/main.c made them on the driver itself before 2.0. */
void bridge_transport_open(size_t rx_buffer, size_t tx_buffer)
{
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = rx_buffer;
    config.tx_buffer_size = tx_buffer;
    usb_serial_jtag_driver_install(&config);
}

int bridge_transport_read_wait(void *buffer, size_t length, uint32_t wait_ticks)
{
    return usb_serial_jtag_read_bytes(buffer, length, wait_ticks);
}

int bridge_transport_write_wait(const void *buffer, size_t length, uint32_t wait_ticks)
{
    return usb_serial_jtag_write_bytes(buffer, length, wait_ticks);
}
