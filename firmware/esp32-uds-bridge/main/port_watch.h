/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * The VC-Adapter board on its own, watching the console on its link port (CONFIG_UDS_LINK_PORT):
 *
 * - The mode: with the sense pin on the port's pin 1 (CONFIG_UDS_LINK_PORT_SENSE_GPIO), a Game Boy game (5 V) moves the board to 3DS
 *   mode and a GBA game (3.3 V) to Switch LDN mode, by storing the mode and restarting (bridge_mode_switch), once the reading has held
 *   for a second and no PC has talked to the board for a few seconds (a PC picks the mode itself, by what it speaks).
 * - Sleep: a board that runs whenever it has power (no chip-enable circuit, or the diode OR's USB side) goes into deep sleep with the
 *   console off and no PC on USB for CONFIG_UDS_LINK_PORT_SLEEP_S, and wakes (restarts) when a link line goes high again.
 */
#ifndef UDS_PORT_WATCH_H
#define UDS_PORT_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "bridge_mode.h"

/* Once per boot, with the mode the board booted in and where to log (printf-like). */
void port_watch_start(bridge_mode_t mode, int (*print)(const char *format, ...));

/* A PC sent something just now (either mode's USB input). */
void port_watch_note_host(void);

/* Whether no PC has sent anything for a few seconds (or ever, since boot). */
bool port_watch_host_quiet(void);

/* 3DS mode: every few hundred milliseconds, with whether every link line reads low right now (linkport_lines_low); half a second of
 * that is the console off. May switch the mode or sleep, and then does not return. Switch mode runs its own task for this. */
void port_watch_poll(bool lines_low, int64_t now_us);

/* Switch mode: starts the task that does port_watch_poll's work with the emulated adapter's word on the lines. Before ldn_app_main. */
void port_watch_start_switch_side(void);

#endif
