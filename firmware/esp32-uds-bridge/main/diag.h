/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * Diagnostics for the 3DS side. The native USB port carries the binary protocol there, so nothing may be printed to it as text:
 * ESP-IDF's log (Wi-Fi's own messages included) is redirected into LOG frames (UDS_EVT_LOG), which every host already shows
 * (uds-esp32-probe "board log:", mGBA's vclink trace, uds-gblink-relay's trace). GB-Link Switch LDN's mode never starts this and stays
 * silent, as its text console needs.
 */
#ifndef UDS_DIAG_H
#define UDS_DIAG_H

#include <stddef.h>

#include "esp_err.h"

/* Queues one log line for the host; must not block (main.c puts it on the ring its forwarding task sends from). */
typedef void (*diag_queue_t)(const char *text, size_t length);

/* Redirects ESP-IDF's log into `queue` and raises its level (sdkconfig.defaults compiles messages in up to INFO; the default level
 * stays NONE, so nothing is logged before this or in Switch LDN mode). Once the USB port is up. */
void diag_start(diag_queue_t queue);

/* The boot's facts, kept for diag_report: the Switch side's memory given to the heap in this mode (bridge_mode_reclaim_ldn_ram). */
void diag_set_reclaimed(size_t bytes, esp_err_t result);

/* One line: why the chip last reset, the reclaimed memory, and free / largest internal and DMA-capable memory. `when` says where. */
void diag_report(const char *when);

#endif
