#pragma once

/* The GBA Wireless Adapter, emulated on this board's own link port pins (CONFIG_UDS_RFU_ADAPTER): the Switch side's GBA end
 * without a GB-Link Pico. GB-Link Switch LDN's bridge (ldn/) talks to the adapter through pico_link in the Pico's 'GB' frames
 * (SetMode, status codes, 64-byte RFU1 data chunks); with this option pico_link hands those frames to rfu_adapter_write and
 * takes the adapter's from rfu_adapter_read instead of using its UART, so the bridge itself is unchanged.
 *
 * The adapter's protocol is GBLink-Firmware's RfuCore (rfuProtocol.hpp, vendored). This file's implementation stands in for
 * the Pico's PIO link layer and its section thread: the second core is given over to the link port while the adapter runs. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the link task on the second core. The adapter stays off the wire until the bridge sends SetMode. */
void rfu_adapter_start(void);

/* Takes the adapter off the wire (pins back to inputs) until the next SetMode. */
void rfu_adapter_stop(void);

/* Bytes the bridge sends to the adapter: a 'GB' frame stream, split anywhere. */
void rfu_adapter_write(const uint8_t *bytes, size_t length);

/* Bytes the adapter sends to the bridge ('GB' frames: status, telemetry, RFU1 chunks). Returns how many were written. Also runs
 * the adapter's half-second telemetry and the status LED, so it is called every millisecond or two. */
size_t rfu_adapter_read(uint8_t *out, size_t capacity);

/* CONFIG_UDS_RFU_CABLE: Ruby and Sapphire's cable (../cable). Hands `print` a summary line (which link the game uses, the
 * cable's state) and the cable's latest log lines, oldest first. From the bridge's side (LDN_CABLE_LOG). */
void rfu_adapter_cable_log(void (*print)(const char *line));

#ifdef __cplusplus
}
#endif
