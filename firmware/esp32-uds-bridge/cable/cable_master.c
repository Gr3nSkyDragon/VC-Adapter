/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* See cable_master.h. Follows mGBA's rfu-wrapper.c: _restart, _masterWord, _packetDone, _loadNextCommand and
 * GBASIORFUWrapperFinishMultiplayer. What is new here is the silent link: in mGBA the game's leaving multiplayer mode
 * restarts the driver (GBASIORFUWrapperSetMode); on a real cable it only shows as the game no longer answering. */
#include "cable_master.h"

#include <string.h>

static void _note(struct cable_master* master, const char* text) {
	if (master->peer.note) {
		master->peer.note(master->peer.context, text);
	}
}

// "<label> XXXX XXXX ...": words in hex, without printf (the board calls this with interrupts masked).
static void _noteWords(struct cable_master* master, const char* label, const uint16_t* words, unsigned count) {
	if (!master->peer.note) {
		return;
	}
	char line[96];
	unsigned at = 0;
	while (*label && at < 40) {
		line[at++] = *label++;
	}
	unsigned i;
	for (i = 0; i < count && at + 6 < sizeof(line); ++i) {
		int shift;
		line[at++] = ' ';
		for (shift = 12; shift >= 0; shift -= 4) {
			line[at++] = "0123456789ABCDEF"[(words[i] >> shift) & 0xF];
		}
	}
	line[at] = '\0';
	master->peer.note(master->peer.context, line);
}

// Commands worth a log line: not the held keys every packet carries, nor a block's 14-byte chunks.
static bool _notable(uint16_t command) {
	return command && command != 0xCAFE && command != 0x4444 && command != 0x8888 && command != 0xFFFF; // FFFF: no answer
}

void cable_master_restart(struct cable_master* master) {
	master->linked = false;
	master->handshakeReplies = 0;
	master->slot = 0;
	master->sum = 0;
	master->sumToSend = 0;
	master->unanswered = 0;
	master->handshakeAnswers = 0;
	master->checksumsNoted = 0;
	memset(master->tx, 0, sizeof(master->tx));
	memset(master->rx, 0, sizeof(master->rx));
	if (master->peer.reset) {
		master->peer.reset(master->peer.context);
	}
}

void cable_master_init(struct cable_master* master, const struct cable_peer* peer) {
	memset(master, 0, sizeof(*master));
	if (peer) {
		master->peer = *peer;
	}
	cable_master_restart(master);
}

void cable_master_handshake_answered(struct cable_master* master) {
	if (!master->linked) {
		master->handshakeReplies = 1;
	}
}

uint16_t cable_master_word(const struct cable_master* master) {
	if (!master->linked) {
		return CABLE_MASTER_HANDSHAKE;
	}
	if (master->slot == 0) {
		return master->sumToSend;
	}
	return master->tx[master->slot - 1];
}

static void _packetDone(struct cable_master* master) {
	++master->packets;
	++master->linkPackets;
	bool any = false;
	unsigned i;
	for (i = 0; i < CABLE_CMD_WORDS; ++i) {
		any = any || master->rx[i];
	}
	if (master->packets <= 3) {
		_noteWords(master, "packet: board", master->tx, CABLE_CMD_WORDS);
		_noteWords(master, "packet: game ", master->rx, CABLE_CMD_WORDS);
	} else if (_notable(master->rx[0]) && master->notedCommands < 12) {
		++master->notedCommands;
		_noteWords(master, "game:", master->rx, 4);
	}
	if (any && master->peer.gameCommand) {
		master->peer.gameCommand(master->peer.context, master->rx);
	}
}

static void _loadNextCommand(struct cable_master* master) {
	memset(master->tx, 0, sizeof(master->tx));
	if (master->peer.nextCommand) {
		master->peer.nextCommand(master->peer.context, master->tx);
	}
	if (master->packets > 3 && _notable(master->tx[0]) && master->notedCommands < 12) {
		++master->notedCommands;
		_noteWords(master, "board:", master->tx, 4);
	}
}

enum cable_next cable_master_done(struct cable_master* master, uint16_t slaveWord, bool answered) {
	if (!master->linked) {
		// The game counts a connection after two identical answers in a row; the master starts one per frame.
		if (answered && (slaveWord & ~3) == CABLE_SLAVE_HANDSHAKE) {
			if (++master->handshakeReplies == 1) {
				_note(master, "handshake: the game answered");
			}
		} else {
			master->handshakeReplies = 0;
		}
		if (master->handshakeReplies >= 2) {
			_note(master, "link established (2 players: the board and the game)");
			master->linked = true;
			master->slot = 0;
			master->sum = 0;
			master->sumToSend = 0;
			master->unanswered = 0;
			master->linkPackets = 0;
		}
		return CABLE_NEXT_FRAME;
	}

	// The game's link closed (no answers: it left multiplayer mode), or it already opened a fresh one (it answers with its
	// handshake word): back to the handshake, as mGBA's driver restarts when the game leaves multiplayer mode. The wireless
	// side hears of it as a new cable session (the peer's reset) and keeps the Switch's.
	if (!answered) {
		if (++master->unanswered >= CABLE_LOST_TRANSFERS) {
			++master->reopens;
			_note(master, "the game stopped answering: its link closed, back to the handshake");
			cable_master_restart(master);
			return CABLE_NEXT_FRAME;
		}
	} else {
		master->unanswered = 0;
	}
	if (answered && (slaveWord & ~3) == CABLE_SLAVE_HANDSHAKE) {
		if (++master->handshakeAnswers >= CABLE_REOPEN_ANSWERS) {
			++master->reopens;
			_note(master, "the game is handshaking: it opened a fresh link, back to the handshake");
			cable_master_restart(master);
			return CABLE_NEXT_FRAME;
		}
	} else {
		master->handshakeAnswers = 0;
	}

	if (master->slot == 0) {
		// The checksum transfer is over: the game's sum of the previous packet should be this side's. Then the eight command
		// words follow.
		// (A handshake word here is the game opening a fresh link, not a misread: see above.)
		if (answered && master->linkPackets && slaveWord != master->sumToSend && (slaveWord & ~3) != CABLE_SLAVE_HANDSHAKE) {
			++master->checksumErrors;
			if (++master->checksumsNoted <= 3) {
				const uint16_t sums[2] = {master->sumToSend, slaveWord};
				_noteWords(master, "CHECKSUM board/game:", sums, 2);
				_noteWords(master, "  board sent:", master->tx, CABLE_CMD_WORDS);
				_noteWords(master, "  board read:", master->rx, CABLE_CMD_WORDS);
			}
		}
		_loadNextCommand(master);
		master->sum = 0;
		master->slot = 1;
		return CABLE_NEXT_GAP;
	}
	unsigned index = master->slot - 1;
	master->sum += cable_master_word(master) + slaveWord;
	master->rx[index] = slaveWord;
	if (master->slot == CABLE_CMD_WORDS) {
		master->sumToSend = master->sum;
		master->slot = 0;
		_packetDone(master);
		return CABLE_NEXT_FRAME; // one packet per frame, like the game's master
	}
	++master->slot;
	return CABLE_NEXT_GAP;
}
