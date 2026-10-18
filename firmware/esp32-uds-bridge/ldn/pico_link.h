#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* UART link to a GB-Link Pico running the wireless-adapter firmware.

   The Pico drives the GBA cable. Its PIO meets the 2 MHz SIO32 timing; ESP32 GPIO
   (~403 ns per access over APB vs a 250 ns half-bit) cannot. Framing is the same
   'GB' transport the Pico uses over USB CDC:

       | 0x47 0x42 | channel:1 | len:2 LE | payload[len] |

   Frames are forwarded whole, header included. */

/* Any ESP pins work (GPIO matrix). Pico side is fixed: GP8 = uart1 TX, GP9 = uart1 RX. */
#define PICO_LINK_PIN_TX CONFIG_PICO_LINK_TX_GPIO /* -> Pico GP9 (uart1 RX) */
#define PICO_LINK_PIN_RX CONFIG_PICO_LINK_RX_GPIO /* <- Pico GP8 (uart1 TX) */
#define PICO_LINK_BAUD 921600

#define PICO_LINK_MAX_PAYLOAD 128
#define PICO_LINK_CHANNEL_COMMAND 0x00
#define PICO_LINK_CHANNEL_DATA 0x01
#define PICO_LINK_CHANNEL_STATUS 0x02

void pico_link_start(void);

/* Adapter location. UART: a GB-Link wired to this board. HOST: the same GB frames
   carried over the console protocol (kinds 6 and 7) to a web client, which forwards
   them to a USB GB-Link or its own simulated adapter. Transparent to upper layers. */
typedef enum { PICO_PORT_UART, PICO_PORT_HOST } pico_port_t;
bool pico_link_set_port(pico_port_t port);   /* false: no memory for the host port */
pico_port_t pico_link_port(void);
/* Host port: bytes in from the client, frames out to it. */
void pico_link_feed_host(const uint8_t *bytes, size_t length);
bool pico_link_poll_host_out(uint8_t *frame, size_t capacity, size_t *length);
void pico_link_stop(void);
bool pico_link_running(void);

/* Send already-framed bytes to the Pico. */
bool pico_link_write(const uint8_t *bytes, size_t length);

/* Wrap a payload in a 'GB' frame and send it. */
bool pico_link_send(uint8_t channel, const uint8_t *payload, size_t length);

/* Pop one complete frame from the Pico, header included. Called only from the
   control task, never the UART task. */
bool pico_link_poll_inbound(uint8_t *frame, size_t capacity, size_t *length);

/* Resend SetMode(rfuWireless). Sent on start and retried until the Pico answers. */
void pico_link_set_mode(void);
/* Block until the adapter TX queue has drained. */
void pico_link_flush(void);
uint32_t pico_link_mode_sent(void);
bool pico_link_await_mode(void);
bool pico_link_wrong_cable(void);
bool pico_link_gba_active(void);

/* Drop frames buffered before the host was listening. */
void pico_link_reset_inbound(void);

/* Reboot the Pico into its USB bootloader. */
void pico_link_bootsel(void);

/* Swap TX and RX pins at runtime. */
void pico_link_swap(void);
bool pico_link_swapped(void);

/* Whether each pin is driven by the far end. An idle UART line holds high; a
   floating pin follows the applied pull. */
void pico_link_probe(bool *tx_driven, bool *rx_driven);

void pico_link_stats(uint32_t *rx_frames, uint32_t *tx_frames, uint32_t *rx_bytes,
                     uint32_t *resync, uint32_t *dropped);

/* First frame since boot, to check pin order and baud. Returns bytes written. */
uint8_t pico_link_first_frame(uint8_t *out, uint8_t capacity);

/* Recent data-channel frames, header included. Tag 0x0E carries the adapter's
   dbgAnyRx / dbgNintendo / comstate. */
uint8_t pico_link_frame_log(uint8_t slot, uint8_t *out, uint8_t capacity);

/* Recent status-channel codes. 0xFF02 AwaitMode: adapter section running.
   0xFF0D WrongCable: wrong cable detected. */
uint8_t pico_link_status_log(uint16_t *out, uint8_t capacity);
