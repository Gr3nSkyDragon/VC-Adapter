/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * PC smoke test of the wrapper's wireless side on the board's bridge backend (cable_air.c): the game's LinkPlayer comes
 * in on the cable, a FireRed trade room's broadcast and the bridge's answers come in as RFU1 frames, and every frame the
 * wireless side sends the bridge is printed and checked. Built from this folder with any C compiler:
 *
 *   cl /nologo /W3 /DCABLE_AIR_HOOK_ALLOC /I.. /I../compat cable_air_test.c ../cable_air.c
 *   cc -Wall -DCABLE_AIR_HOOK_ALLOC -I.. -I../compat cable_air_test.c ../cable_air.c
 *
 * The air file is included here, not linked, so the test can report the size of its state.
 */
#include <stdlib.h> // before the air file, whose calloc/free the compat rfu.h redirects

#include "../rfu-wrapper-air.c"

#include "cable_air.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned s_sent[8];
static unsigned s_failures;

static void _check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		++s_failures;
	}
}

static void _emit(const uint8_t* frame, size_t length) {
	uint32_t type = ((uint32_t) frame[4] << 24) | (frame[5] << 16) | (frame[6] << 8) | frame[7];
	uint32_t header = ((uint32_t) frame[8] << 24) | (frame[9] << 16) | (frame[10] << 8) | frame[11];
	if (type < 8) {
		++s_sent[type];
	}
	printf("  -> bridge: type %u header %08X (%u bytes)", (unsigned) type, (unsigned) header, (unsigned) length);
	if (type == 6) {
		unsigned n = header >> 24, i;
		printf(" data");
		for (i = 0; i < n && i < 16; ++i) {
			printf(" %02X", frame[12 + i]);
		}
	}
	printf("\n");
}

static void _traceLine(const char* text) {
	printf("  trace: %s\n", text);
}

static void _put32be(uint8_t* p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void _bridgeFrame(uint32_t type, uint32_t header, const uint8_t* body, size_t bodyLength, size_t size, uint32_t now) {
	uint8_t frame[104] = {'R', 'F', 'U', '1'};
	_put32be(&frame[4], type);
	_put32be(&frame[8], header);
	if (body) {
		memcpy(&frame[12], body, bodyLength);
	}
	cable_air_net_frame(frame, size, now);
}

int main(void) {
	uint32_t now = 1000;
	unsigned i;
	printf("state: struct Air %u bytes, room for %u\n", (unsigned) sizeof(struct Air), (unsigned) CABLE_AIR_STATE_BYTES);
	_check(sizeof(struct Air) <= CABLE_AIR_STATE_BYTES, "the wireless side's state fits CABLE_AIR_STATE_BYTES");

	// Its state in memory of the caller's, as on the board (the adapter core's).
	static union {
		uint8_t bytes[CABLE_AIR_STATE_BYTES];
		long double align;
	} memory;
	cable_air_set_trace(_traceLine);
	_check(cable_air_start(_emit, now, memory.bytes, sizeof(memory.bytes)), "the wireless side starts in the given memory");
	cable_air_cable_reset();

	// The cable master's packets: after ANNOUNCE_PACKETS the virtual player 0 asks for the player data exchange.
	uint16_t cmd[CABLE_AIR_CMD_WORDS];
	bool asked = false;
	for (i = 0; i < 40 && !asked; ++i) {
		memset(cmd, 0, sizeof(cmd));
		if (cable_air_next_command(cmd) && cmd[0] == 0x2222) {
			asked = true;
		}
	}
	_check(asked, "player 0 sends SEND_LINK_TYPE (2222) to the game");

	// The game's LinkPlayer, as Ruby sends it on the cable: INIT_BLOCK with the size, then 14-byte CONT_BLOCK chunks.
	uint8_t lp[LINK_PLAYER_BLOCK_SIZE];
	memset(lp, 0, sizeof(lp));
	_put16(&lp[LP_VERSION_OFFSET], 0x0202); // Ruby, English
	lp[LP_TRAINER_ID_OFFSET] = 0x34;
	lp[LP_TRAINER_ID_OFFSET + 1] = 0x12;
	static const uint8_t name[] = {0xCC, 0xCF, 0xBC, 0xD3, 0xFF}; // RUBY in the game's charset
	memcpy(&lp[LP_NAME_OFFSET], name, sizeof(name));
	_put16(&lp[LP_LINK_TYPE_OFFSET], 0x1133);
	uint16_t block[CABLE_AIR_CMD_WORDS] = {0xBBBB, LINK_PLAYER_BLOCK_SIZE};
	cable_air_game_command(block);
	unsigned at;
	for (at = 0; at < sizeof(lp); at += 14) {
		memset(block, 0, sizeof(block));
		block[0] = 0x8888;
		for (i = 0; i < 7; ++i) {
			unsigned b = at + i * 2;
			block[1 + i] = (b < sizeof(lp) ? lp[b] : 0) | ((b + 1 < sizeof(lp) ? lp[b + 1] : 0) << 8);
		}
		cable_air_game_command(block);
	}

	// A GBA frame: with the LinkPlayer in, the wireless side looks for a leader.
	cable_air_frame(now += 17);

	// The bridge relays a FireRed trade room: word 3's low byte is the activity, 4 = trade.
	uint8_t words[24] = {0};
	_put32be(&words[12], 0x00000004);
	memset(s_sent, 0, sizeof(s_sent));
	_bridgeFrame(0, 0x00001234, words, sizeof(words), 36, now);
	cable_air_frame(now += 17);
	_check(s_sent[1] == 1, "it asks the bridge to join (CONNECT_REQ)");

	// The bridge accepts once the Switch has: then the child's game data goes out (NI handshake).
	_bridgeFrame(2, 0x0000ABCD, NULL, 0, 16, now);
	for (i = 0; i < 9; ++i) {
		cable_air_frame(now += 17);
	}
	_check(s_sent[6] >= 1, "it starts its game data before the leader speaks (CLIENT_SEND)");

	// The leader's first bare frame asks the child to speak: the next NI sub-frame answers it.
	unsigned before = s_sent[6];
	uint8_t bare[1] = {0};
	_bridgeFrame(5, 1, bare, 1, 104, now);
	cable_air_frame(now += 17);
	_check(s_sent[6] > before, "it answers the leader's frame with its next NI sub-frame");

	// The Switch's session ends: the bridge's DISCONNECT for our id.
	_bridgeFrame(4, 0x0000ABCD, NULL, 0, 16, now);
	cable_air_frame(now += 17);

	cable_air_stop();
	_check(!cable_air_running(), "it stops");
	printf("%s\n", s_failures ? "FAILED" : "passed");
	return s_failures ? 1 : 0;
}
