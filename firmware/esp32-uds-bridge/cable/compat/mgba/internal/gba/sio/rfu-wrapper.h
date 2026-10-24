/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Stand-in for mGBA's <mgba/internal/gba/sio/rfu-wrapper.h>. In mGBA the wrapper is an SIO driver that plays the
 * cable's master in the emulator (rfu-wrapper.c); here the cable is real and its master is this board's own link code
 * (cable_link.c, to come), so the wrapper is only what the air file reaches through it: the peer it installs and its
 * own slot. The peer is mGBA's, field for field. */
#ifndef CABLE_COMPAT_RFU_WRAPPER_H
#define CABLE_COMPAT_RFU_WRAPPER_H

#include <mgba-util/common.h>

CXX_GUARD_START

enum {
	RFU_WRAPPER_CMD_LENGTH = 8,
};

struct GBASIO;

struct GBASIORFUWrapperPeer {
	void* context;
	// The game (cable player 1) finished sending a packet with a non-zero command.
	void (*gameCommand)(void* context, const uint16_t command[RFU_WRAPPER_CMD_LENGTH]);
	// Fill the virtual player's next command. Return false (or leave it zero) to send nothing this packet.
	bool (*nextCommand)(void* context, uint16_t command[RFU_WRAPPER_CMD_LENGTH]);
	// The cable link was (re)started from scratch (the game opened it, or closed it and opened it again).
	void (*reset)(void* context);
	// Once per GBA frame, whatever the cable is doing (the wireless side keeps running while the game's link is closed).
	void (*frame)(void* context);
};

struct GBASIORFUWrapper {
	struct {
		struct GBASIO* p; // the trace's handle (unused on the board)
	} d;
	void* air;
	void (*airDestroy)(void* air);
	struct GBASIORFUWrapperPeer peer;
};

CXX_GUARD_END

#endif
