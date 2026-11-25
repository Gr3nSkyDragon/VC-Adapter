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

/* VC-Adapter merge: the merged firmware's 3DS side (main/main.c) uses the same port. It opens it with its own buffer sizes and
   without the stdio hook (it prints nothing: its log goes out as frames), and waits on reads and writes, as it did when it called
   the USB Serial/JTAG driver itself. Not called in Switch LDN mode, nor together with bridge_transport_init. */
void bridge_transport_open(size_t rx_buffer, size_t tx_buffer);
/* Waits up to `wait_ticks` for the first byte, then returns what has arrived (up to `length`). */
int bridge_transport_read_wait(void *buffer, size_t length, uint32_t wait_ticks);
/* How many bytes were queued: fewer when the host stopped reading for `wait_ticks` (USB; a UART never waits for the host). */
int bridge_transport_write_wait(const void *buffer, size_t length, uint32_t wait_ticks);
