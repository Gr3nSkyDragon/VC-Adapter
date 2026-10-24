/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The cable link's master (player 0) to a Ruby/Sapphire game (player 1): mGBA's RFU Cable Wrapper driver
 * (src/gba/sio/rfu-wrapper.c, GBASIORFUWrapperFinishMultiplayer and its helpers) without the emulator. The caller does the
 * transfers on the wire: for each it takes cable_master_word, moves one 16-bit word each way, and hands the game's word
 * back with cable_master_done, which says when the next transfer is due.
 *
 * Handshake: 0x8FFF, one transfer per frame, until the game answers 0xB9A0 twice in a row. Then packets, one per frame:
 * the checksum of the previous packet's words, then the eight words of a command (a command and seven arguments) each
 * way, a timer's gap apart. The game's command is complete after the eighth; the next command of player 0 is loaded after
 * the checksum transfer.
 */
#ifndef CABLE_MASTER_H
#define CABLE_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	CABLE_MASTER_HANDSHAKE = 0x8FFF,
	CABLE_SLAVE_HANDSHAKE = 0xB9A0,
	CABLE_CMD_WORDS = 8,
	// The game's master starts the rest of a packet from a timer: 197 ticks of 64 cycles (16.78 MHz) after each transfer.
	CABLE_TRANSFER_GAP_US = 751,
	// A GBA frame: 280896 cycles.
	CABLE_FRAME_US = 16743,
	// Transfers in a row with no answer from the game before its link counts as closed: a packet's worth. The game closes its
	// link and opens a fresh one at set points of the cable club (the trade machine among them), and its new link starts with
	// the handshake again; at 90, a hardware test went on sending packets to a game that was waiting for 0x8FFF, until its
	// link error.
	CABLE_LOST_TRANSFERS = 9,
	// Handshake answers in a row from a linked game: it opened a fresh link (and is past the silence already).
	CABLE_REOPEN_ANSWERS = 2,
};

enum cable_next {
	CABLE_NEXT_FRAME, // the next transfer starts with the next frame
	CABLE_NEXT_GAP,   // the next transfer follows after CABLE_TRANSFER_GAP_US
};

struct cable_peer {
	void* context;
	void (*gameCommand)(void* context, const uint16_t command[CABLE_CMD_WORDS]);
	bool (*nextCommand)(void* context, uint16_t command[CABLE_CMD_WORDS]);
	void (*reset)(void* context);
	void (*note)(void* context, const char* text); // optional: link events, for a log
};

struct cable_master {
	struct cable_peer peer;
	bool linked; // the handshake has passed
	int handshakeReplies;
	int slot; // 0: the checksum transfer, 1-8: the command words
	uint16_t tx[CABLE_CMD_WORDS];
	uint16_t rx[CABLE_CMD_WORDS];
	uint16_t sum;
	uint16_t sumToSend;
	unsigned packets;
	unsigned linkPackets; // since the link was last established
	unsigned unanswered;
	unsigned handshakeAnswers; // linked, and the game answering with its handshake word
	unsigned reopens;
	// The game sends its own checksum of the previous packet in each checksum transfer, as every player does (pokeruby
	// link.c, DoRecv): one that differs from this side's means a word was misread on one side or the other, which the game
	// answers with its link error ("transmission difficulties").
	unsigned checksumErrors;
	unsigned checksumsNoted; // logged since the link was last (re)opened
	unsigned notedCommands;
};

// Starts over at the handshake, and tells the peer the cable link is new.
void cable_master_init(struct cable_master* master, const struct cable_peer* peer);
void cable_master_restart(struct cable_master* master);
// The game already answered one handshake (the board's probe). The game counts its link as up after two answered in a row
// (pokeruby link.c, DoHandshake), so this side must too: counted from zero again, the board went on handshaking one transfer
// after the game had begun its packets, and every word after that was a transfer off (the game's checksum check failed).
void cable_master_handshake_answered(struct cable_master* master);

uint16_t cable_master_word(const struct cable_master* master);
// The game's word, and whether it answered at all (its start bit came). False ends a link that stays silent.
enum cable_next cable_master_done(struct cable_master* master, uint16_t slaveWord, bool answered);

#ifdef __cplusplus
}
#endif

#endif
