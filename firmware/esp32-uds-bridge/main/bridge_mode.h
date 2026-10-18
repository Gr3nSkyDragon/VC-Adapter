/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * Which of the merged firmware's two sides the board runs. They cannot share the radio (the 3DS side captures and injects raw
 * frames; GB-Link Switch LDN's side joins or hosts a Switch room through the Wi-Fi driver's own station and access point), so the
 * board boots in one mode and changes mode by restarting. The mode is kept in NVS, so the board comes back in the last one used.
 *
 *   BRIDGE_MODE_LDN  GB-Link Switch LDN's bridge (ldn/), for mGBA's Switch backend, GB-Link's web client and a GB-Link (the
 *                    default)
 *   BRIDGE_MODE_UDS  this firmware's 3DS side: Azahar, mGBA's 3DS backends, uds-gblink-relay, uds-esp32-setup
 *
 * The host picks the mode by talking: each side watches what the host sends when no host of its own is talking, and restarts the
 * board in the other mode when the host speaks the other side's protocol (see bridge_mode_watch_uds and bridge_usb_sniff). The
 * host's first attempt then fails while the board restarts; its next one finds the right mode. Opening the USB port does not reset
 * the board: 3DS mode watches again once its host has been quiet for a moment (main.c, MODE_WATCH_QUIET_US; "LDN_" could occur in
 * its binary traffic), and Switch LDN mode looks at everything the host sends, since none of its own frames can be a 3DS HELLO
 * (bridge_usb_sniff).
 */
#ifndef BRIDGE_MODE_H
#define BRIDGE_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BRIDGE_MODE_UDS = 0,
    BRIDGE_MODE_LDN = 1,
} bridge_mode_t;

/* The mode to boot in (NVS must be initialised): the stored one, Switch LDN when none is stored. After three crashes in a row
 * (panic or watchdog resets) it stores and returns the other mode, so that a mode that cannot start never holds the board in a
 * reset loop. Always BRIDGE_MODE_UDS when the Switch side is not built in. Call once per boot. */
bridge_mode_t bridge_mode_get(void);
/* Whether this build has the Switch side (ESP-IDF 6.1). */
bool bridge_mode_has_ldn(void);
/* Stores the mode and restarts into it. Does not return. */
void bridge_mode_switch(bridge_mode_t mode);

/* 3DS mode, first thing: GB-Link Switch LDN's static data (all of it in one block, components/ldn_bridge/linker.lf) is never used
 * when the board runs its 3DS side, so it is handed to the heap, which that side needs. Returns the bytes added (0 without the
 * Switch side). Never call it in Switch LDN mode. `result` (may be NULL) gets heap_caps_add_region's answer. */
size_t bridge_mode_reclaim_ldn_ram(esp_err_t *result);

/* 3DS mode: bytes from the host while no 3DS host is talking (before the first valid frame, or after a quiet moment). Restarts
 * the board in Switch LDN mode when they carry GB-Link Switch LDN's console ("LDN_..." commands, "\nLDN_BINARY\n"). */
void bridge_mode_watch_uds(const uint8_t *bytes, size_t length);

#endif
