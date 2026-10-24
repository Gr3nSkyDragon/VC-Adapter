/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * PC test of the cable master (cable_master.c) driving the wrapper's wireless side (cable_air.c), against a fake Ruby on
 * the cable: it answers the handshake, and when player 0 asks for the player data exchange (2222) it sends its
 * LinkPlayer block in cable packets. The wireless side should then look for the Switch's room and ask to join it.
 *
 *   cl /nologo /W3 /DCABLE_AIR_HOOK_ALLOC /I.. /I../compat cable_master_test.c ../cable_master.c ../cable_air.c ../rfu-wrapper-air.c
 *   cc -Wall -DCABLE_AIR_HOOK_ALLOC -I.. -I../compat cable_master_test.c ../cable_master.c ../cable_air.c ../rfu-wrapper-air.c
 */
#include "cable_air.h"
#include "cable_master.h"

#include <stdio.h>
#include <string.h>

static unsigned s_failures;
static unsigned s_connectRequests;
static bool s_answer = true;

static void _check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		++s_failures;
	}
}

static void _emit(const uint8_t* frame, size_t length) {
	(void) length;
	if (frame[7] == 1) {
		++s_connectRequests;
	}
}

static void _trace(const char* text) {
	printf("  trace: %s\n", text);
}

// --- the fake Ruby (player 1) ----------------------------------------------------------------------------------------

static struct {
	bool linked;
	unsigned slot;
	uint16_t heard[CABLE_CMD_WORDS];
	uint16_t queue[8][CABLE_CMD_WORDS];
	unsigned queued, next;
	uint16_t current[CABLE_CMD_WORDS];
	uint16_t sum, lastSum; // its checksum of the packet being moved, and of the one before (link.c DoRecv)
	unsigned handshakes;   // answered in a row; two and its link is up (link.c DoHandshake)
	bool linkPending;
	unsigned badChecksums; // the master's checksum against its own
} s_ruby;

static void _rubyQueueLinkPlayer(void) {
	uint8_t lp[60];
	memset(lp, 0, sizeof(lp));
	lp[16] = 0x02; // version: Ruby
	lp[17] = 0x02;
	lp[24] = 0xCC; // name RUBY
	lp[25] = 0xCF;
	lp[26] = 0xBC;
	lp[27] = 0xD3;
	lp[28] = 0xFF;
	lp[16 + 0x14] = 0x33; // link type 0x1133
	lp[16 + 0x15] = 0x11;
	uint16_t* c = s_ruby.queue[s_ruby.queued++];
	memset(c, 0, CABLE_CMD_WORDS * 2);
	c[0] = 0xBBBB;
	c[1] = sizeof(lp);
	unsigned at, i;
	for (at = 0; at < sizeof(lp); at += 14) {
		c = s_ruby.queue[s_ruby.queued++];
		c[0] = 0x8888;
		for (i = 0; i < 7; ++i) {
			unsigned b = at + i * 2;
			c[1 + i] = (b < sizeof(lp) ? lp[b] : 0) | ((b + 1 < sizeof(lp) ? lp[b + 1] : 0) << 8);
		}
	}
}

// The game's word for this transfer, given the master's.
static uint16_t _rubyWord(uint16_t master) {
	if (!s_ruby.linked) {
		// The game's own rule: two handshakes in a row and the link is up, with the next transfer its packets' first.
		// Its handshake word goes out whatever the master sends (SIOMLT_SEND holds it until the link is up).
		if (master == CABLE_MASTER_HANDSHAKE) {
			if (++s_ruby.handshakes >= 2) {
				s_ruby.linkPending = true; // from the next transfer
			}
		} else {
			s_ruby.handshakes = 0;
		}
		return CABLE_SLAVE_HANDSHAKE;
	}
	if (s_ruby.slot == 0) {
		if (master != s_ruby.sum) {
			++s_ruby.badChecksums; // the game's link error
		}
		// The checksum transfer starts a packet: pick this packet's command.
		memset(s_ruby.current, 0, sizeof(s_ruby.current));
		if (s_ruby.next < s_ruby.queued) {
			memcpy(s_ruby.current, s_ruby.queue[s_ruby.next++], sizeof(s_ruby.current));
		}
		s_ruby.lastSum = s_ruby.sum;
		s_ruby.sum = 0;
		return s_ruby.lastSum; // its sum of the previous packet, as every player sends
	}
	s_ruby.heard[s_ruby.slot - 1] = master;
	s_ruby.sum += master + s_ruby.current[s_ruby.slot - 1];
	return s_ruby.current[s_ruby.slot - 1];
}

static void _rubyAfter(void) {
	if (s_ruby.linkPending) {
		s_ruby.linkPending = false;
		s_ruby.linked = true;
		s_ruby.slot = 0;
		s_ruby.sum = 0;
		return;
	}
	if (!s_ruby.linked) {
		return;
	}
	if (s_ruby.slot == CABLE_CMD_WORDS) {
		s_ruby.slot = 0;
		if (s_ruby.heard[0] == 0x2222 && !s_ruby.queued) {
			_rubyQueueLinkPlayer();
		}
	} else {
		++s_ruby.slot;
	}
}

// --- the board side ----------------------------------------------------------------------------------------------------

static void _gameCommand(void* context, const uint16_t command[CABLE_CMD_WORDS]) {
	(void) context;
	cable_air_game_command(command);
}

static bool _nextCommand(void* context, uint16_t command[CABLE_CMD_WORDS]) {
	(void) context;
	return cable_air_next_command(command);
}

static void _reset(void* context) {
	(void) context;
	cable_air_cable_reset();
}

static void _note(void* context, const char* text) {
	(void) context;
	printf("  cable: %s\n", text);
}

// One GBA frame of transfers: until the master's next is due with the next frame.
static void _frame(struct cable_master* master) {
	enum cable_next next;
	do {
		uint16_t word = cable_master_word(master);
		uint16_t reply = s_answer ? _rubyWord(word) : 0xFFFF;
		next = cable_master_done(master, reply, s_answer);
		_rubyAfter();
	} while (next == CABLE_NEXT_GAP);
}

int main(void) {
	static union {
		uint8_t bytes[CABLE_AIR_STATE_BYTES];
		long double align;
	} memory;
	uint32_t now = 1000;
	cable_air_set_trace(_trace);
	_check(cable_air_start(_emit, now, memory.bytes, sizeof(memory.bytes)), "the wireless side starts");

	// The board's probe: one handshake transfer, which the game answers (and counts).
	_check(_rubyWord(CABLE_MASTER_HANDSHAKE) == CABLE_SLAVE_HANDSHAKE, "the game answers the probe");
	_rubyAfter();

	const struct cable_peer peer = {NULL, _gameCommand, _nextCommand, _reset, _note};
	struct cable_master master;
	cable_master_init(&master, &peer);
#ifndef CABLE_TEST_WITHOUT_PROBE_COUNT // build with it to see the hardware test's one-transfer slip
	cable_master_handshake_answered(&master); // as cableSession does after a probe
#endif

	unsigned frames, transfers = 0;
	bool roomSent = false;
	for (frames = 0; frames < 400 && !s_connectRequests; ++frames) {
		enum cable_next next;
		do {
			uint16_t word = cable_master_word(&master);
			uint16_t reply = s_answer ? _rubyWord(word) : 0xFFFF;
			next = cable_master_done(&master, reply, s_answer);
			_rubyAfter();
			++transfers;
		} while (next == CABLE_NEXT_GAP);
		cable_air_frame(now += 17);
		// Once the game's LinkPlayer is in, the bridge relays the Switch's trade room.
		if (!roomSent && frames > 60) {
			uint8_t room[24] = {0};
			room[15] = 4; // word 3, activity 4: trade
			cable_air_net_packet(0, 0x1234, room, sizeof(room), now);
			roomSent = true;
		}
	}
	printf("  %u frames, %u transfers, %u packets\n", frames, transfers, master.packets);
	_check(master.linked, "the handshake passes");
	_check(master.checksumErrors == 0, "the game's checksums match the board's");
	_check(s_ruby.badChecksums == 0, "the board's checksums match the game's (no link error)");
	_check(s_ruby.queued > 0, "player 0 asked the game for its LinkPlayer (2222)");
	_check(s_connectRequests == 1, "with the game's LinkPlayer in, it asks the bridge to join the room");

	// The game closes its link and opens a fresh one at once (as at the trade machine): it answers its handshake word.
	unsigned badBefore = s_ruby.badChecksums;
	s_ruby.linked = false;
	s_ruby.handshakes = 0;
	for (frames = 0; frames < 30; ++frames) {
		_frame(&master);
	}
	_check(master.reopens == 1 && master.linked, "a game handshaking again gets a fresh link");
	_check(s_ruby.badChecksums == badBefore && master.checksumErrors == 0, "the fresh link's checksums match");

	// It closes its link, is silent a few frames, then opens a fresh one.
	s_answer = false;
	for (frames = 0; frames < 3; ++frames) {
		_frame(&master);
	}
	s_answer = true;
	s_ruby.linked = false;
	s_ruby.handshakes = 0;
	for (frames = 0; frames < 30; ++frames) {
		_frame(&master);
	}
	_check(master.reopens == 2 && master.linked, "a game silent for a while, then handshaking, gets a fresh link");
	_check(s_ruby.badChecksums == badBefore, "that link's checksums match too");

	// The game leaves multiplayer mode: no more answers, and the link goes back to the handshake.
	s_answer = false;
	for (frames = 0; frames < 40 && master.linked; ++frames) {
		enum cable_next next;
		do {
			next = cable_master_done(&master, 0xFFFF, false);
		} while (next == CABLE_NEXT_GAP);
	}
	_check(!master.linked, "a silent game closes the link");

	cable_air_stop();
	printf("%s\n", s_failures ? "FAILED" : "passed");
	return s_failures ? 1 : 0;
}
