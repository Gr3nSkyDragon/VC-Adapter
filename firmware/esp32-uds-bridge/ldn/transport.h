#pragma once
#include <stddef.h>
#include <stdint.h>

/* Console link to the host: USB Serial/JTAG where available, else the console
   UART behind a USB bridge (original ESP32). */
void bridge_transport_init(void);
int bridge_transport_read(void *buffer, size_t length);
void bridge_transport_write(const void *buffer, size_t length);
uint32_t bridge_transport_dropped(void);
const char *bridge_transport_name(void);
/* Applies after queued output drains at the old rate. No-op on USB Serial/JTAG. */
void bridge_transport_set_baud(int baud);
/* Block until queued output drains, with a timeout. */
void bridge_transport_flush(void);
