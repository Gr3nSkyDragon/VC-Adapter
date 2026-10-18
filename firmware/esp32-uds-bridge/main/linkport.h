/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * A Game Boy link port wired straight to this board's GPIOs, with no GB-Link (Pico) between: firmware 1.8 on, CONFIG_UDS_LINK_PORT.
 * The board does here what the Pico's SPI did in Game Boy mode: it is the clock master and shifts one byte at a time, MSB first, the
 * clock idle high, each bit put out on the falling edge and read before the rising one, fast as the Pico does (262144 Hz unless
 * menuconfig says otherwise; linkport.c says why not a Game Boy's own 8192 Hz). The cartridge, in a GBA (in its GB/GBC mode), a GBC or
 * a Game Boy, is the slave.
 *
 * The port's signals are 5 V in GB/GBC mode (3.3 V for a GBA game); this chip's pins are 3.3 V only. A level shifter goes between, its
 * high side powered from the port's own VCC (pin 1) so that it follows the console. Between bytes the pins are open drain (the
 * shifter's pull-ups make the high level), so a cartridge that drives the clock itself never fights a driven high; during a byte they
 * are driven both ways, as the Pico drives them, for crisp edges through the shifter. SI and SO
 * are this board's, as in the GB-Link's notation (its SI takes the console's SO). SD is wired for the shared pinout but unused. See
 * the README's "GBA link port" for the wiring.
 *
 * Optional: the port's VCC through a 1:2 divider to an ADC pin tells whether a console is there and in which mode.
 */
#ifndef UDS_LINKPORT_H
#define UDS_LINKPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Sets up the pins (and the sense pin's ADC). ESP_ERR_NOT_SUPPORTED when CONFIG_UDS_LINK_PORT is off. */
esp_err_t linkport_init(void);

/* The port is set up: the wrapper may clock it. */
bool linkport_enabled(void);

enum linkport_power {
    LINKPORT_POWER_UNKNOWN, /* no sense pin: assumed on, in GB/GBC mode */
    LINKPORT_POWER_OFF,     /* nothing on the port, or the console is off */
    LINKPORT_POWER_GBA,     /* about 3.3 V: a GBA running a GBA game; Game Boy link traffic means nothing to it */
    LINKPORT_POWER_GB,      /* about 5 V: a GBA in GB/GBC mode, a GBC or a Game Boy */
};

/* The port's VCC, read now (a few tens of microseconds). `millivolts` (may be NULL) gets the reading at the port, 0 without a sense
 * pin. */
enum linkport_power linkport_power(uint32_t *millivolts);

/* Watches the clock line for a little more than one bit at 8192 Hz (125 us): true if it went low, which this board did not do, so the
 * other side is clocking a byte of its own (or the line is stuck low: no console, or one switched off). */
bool linkport_clock_busy(void);

/* One transfer with this board as the master: `out` is shifted out while the byte the other side shifts back comes in. Takes 16 half
 * periods (about 31 us at 262144 Hz) with interrupts held off on this core. Nobody on the other end reads as FF (the lines idle high). */
uint8_t linkport_exchange(uint8_t out);

/* Lets go of the clock and the data line (both idle high). */
void linkport_release(void);

#endif
