/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The RFU Cable Wrapper's wireless side on the board: mGBA's rfu-wrapper-air.c (unchanged, this folder), joined to the
 * Switch side's bridge instead of an mGBA backend. A Ruby or Sapphire game on the link port's cable then joins the
 * Switch's FireRed room as an FRLG child, as it does in mGBA.
 *
 * Toward the bridge it speaks the RFU1 frames the bridge exchanges with the emulated Wireless Adapter (../rfu): the
 * room's broadcasts, CONNECT_REQ/ACK, CLIENT_SEND/HOST_SEND, DISCONNECT. Toward the cable it is the peer of the cable
 * master: the game's commands in, the virtual player 0's out.
 *
 * Not thread safe: every call from the one task that runs the cable. Times are a millisecond clock the caller passes in,
 * so this builds and runs on a PC too.
 */
#ifndef CABLE_AIR_H
#define CABLE_AIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	CABLE_AIR_CMD_WORDS = 8, // a cable command: the command word and seven arguments
	// The wireless side's state (mGBA's struct Air, without its event queue), rounded up; the PC test checks it fits.
	CABLE_AIR_STATE_BYTES = 8192,
};

// A whole RFU1 frame for the bridge (16, 36 or 104 bytes).
typedef void (*cable_air_emit_fn)(const uint8_t* frame, size_t length);

// Sets up the wireless side, its state in `memory` (CABLE_AIR_STATE_BYTES, aligned for any type; on the board the adapter
// core's, which is not running then). NULL: from the heap. False when it does not fit.
bool cable_air_start(cable_air_emit_fn emit, uint32_t now_ms, void* memory, size_t size);
void cable_air_stop(void);
bool cable_air_running(void);

// One whole RFU1 frame from the bridge, or the same as its fields (the RFU1 stream parser's callback).
void cable_air_net_frame(const uint8_t* frame, size_t length, uint32_t now_ms);
void cable_air_net_packet(uint32_t type, uint32_t header, const uint8_t* payload, size_t length, uint32_t now_ms);
// Once per GBA frame (~59.7 Hz), whatever the cable is doing.
void cable_air_frame(uint32_t now_ms);

// The cable master's side (mGBA's GBASIORFUWrapperPeer).
void cable_air_cable_reset(void);
void cable_air_game_command(const uint16_t command[CABLE_AIR_CMD_WORDS]);
bool cable_air_next_command(uint16_t command[CABLE_AIR_CMD_WORDS]);

// The wrapper's trace lines (its AIRLOG and this side's), as they are written. Optional.
void cable_air_set_trace(void (*line)(const char* text));

#ifdef __cplusplus
}
#endif

#endif
