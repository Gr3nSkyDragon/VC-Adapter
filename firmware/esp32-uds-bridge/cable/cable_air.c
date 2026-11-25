/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The board's half of the wrapper's wireless side (cable_air.h): what mGBA gives rfu-wrapper-air.c, its adapter's event
 * queue and a backend, done here with the bridge on this chip. The backend is the client half of mGBA's ESP32 backend
 * (src/gba/sio/esp32/rfu-esp32.c, _handleRfu1 and the senders), which already speaks this bridge's RFU1 frames over
 * USB; here the frames go to the bridge directly.
 */
#include "cable_air.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h> // before rfu.h, whose calloc/free stand for the air file's allocation (and are undone below)
#include <string.h>

#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/rfu-wrapper-air.h>
#include <mgba/internal/gba/sio/rfu.h>

#undef calloc
#undef free

enum {
	RFU1_BROADCAST = 0,
	RFU1_CONNECT_REQ = 1,
	RFU1_CONNECT_ACK = 2,
	RFU1_CONNECT_NACK = 3,
	RFU1_DISCONNECT = 4,
	RFU1_HOST_SEND = 5,
	RFU1_CLIENT_SEND = 6,
	RFU1_CLIENT_ACK = 7,

	MAX_PAYLOAD = 92,
	// mGBA's: the bridge acknowledges a join only once the Switch has taken it.
	CONNECT_TIMEOUT_MS = 8000,
};

static const uint8_t kRfu1[4] = {'R', 'F', 'U', '1'};

// --- the adapter's event queue (mGBA's rfu.c, without its mutex: one task here) -------------------------------------

static struct GBASIORFUEvent s_events[RFU_EVENT_QUEUE];

static struct GBASIORFUEvent* _queueEvent(struct GBASIORFU* rfu) {
	unsigned next = (rfu->eventHead + 1) % RFU_EVENT_QUEUE;
	if (next == rfu->eventTail) {
		return NULL; // full: dropped, as in mGBA
	}
	struct GBASIORFUEvent* event = &rfu->events[rfu->eventHead];
	memset(event, 0, sizeof(*event));
	rfu->eventHead = next;
	return event;
}

void GBASIORFUCreate(struct GBASIORFU* rfu, struct GBASIORFUBackend* backend) {
	memset(rfu, 0, sizeof(*rfu));
	rfu->backend = backend;
	rfu->events = s_events;
}

// --- the air file's state ---------------------------------------------------------------------------------------------

static void* s_storage;
static size_t s_storageSize;
static bool s_storageUsed;

void* cable_air_calloc(size_t count, size_t size) {
	if (!s_storage) {
		return calloc(count, size);
	}
	if (s_storageUsed || count * size > s_storageSize) {
		return NULL;
	}
	s_storageUsed = true;
	memset(s_storage, 0, count * size);
	return s_storage;
}

void cable_air_free(void* memory) {
	if (memory && memory == s_storage) {
		s_storageUsed = false;
	} else {
		free(memory);
	}
}

void GBASIORFUDestroy(struct GBASIORFU* rfu) {
	(void) rfu;
}

void GBASIORFUSetTraceFile(struct GBASIORFU* rfu, const char* path) {
	(void) rfu;
	(void) path;
}

void GBASIORFUBroadcastReceived(struct GBASIORFU* rfu, uint16_t deviceId, uint8_t nextSlot, const uint32_t data[RFU_BROADCAST_WORDS]) {
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_BROADCAST;
		event->deviceId = deviceId;
		event->slot = nextSlot;
		memcpy(event->words, data, sizeof(event->words));
	}
}

void GBASIORFUConnectResult(struct GBASIORFU* rfu, bool accepted, uint16_t deviceId, unsigned slot) {
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_CONNECT_RESULT;
		event->accepted = accepted;
		event->deviceId = deviceId;
		event->slot = slot;
	}
}

void GBASIORFUDataReceived(struct GBASIORFU* rfu, unsigned slot, const uint8_t* data, size_t length) {
	if (length > RFU_PACKET_MAX) {
		length = RFU_PACKET_MAX;
	}
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_DATA;
		event->slot = slot;
		event->length = (uint8_t) length;
		memcpy(event->data, data, length);
	}
}

void GBASIORFUDisconnected(struct GBASIORFU* rfu, int slot) {
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_DISCONNECTED;
		event->slot = slot;
	}
}

bool GBASIORFUPopEvent(struct GBASIORFU* rfu, struct GBASIORFUEvent* out) {
	if (rfu->eventHead == rfu->eventTail) {
		return false;
	}
	*out = rfu->events[rfu->eventTail];
	rfu->eventTail = (rfu->eventTail + 1) % RFU_EVENT_QUEUE;
	return true;
}

// --- the trace --------------------------------------------------------------------------------------------------------

static void (*s_traceLine)(const char* text);

// The trace's own formatter, for what the air file and this file use (%u, %d, %s, %X with a zero-padded width): on the
// board it runs on the adapter's core with interrupts masked, where the C library's printf, which can take locks, must not.
static void _format(char* out, size_t cap, const char* format, va_list args) {
	size_t n = 0;
#define PUT(c) \
	do { \
		if (n + 1 < cap) { \
			out[n++] = (c); \
		} \
	} while (0)
	for (; *format; ++format) {
		if (*format != '%') {
			PUT(*format);
			continue;
		}
		++format;
		char pad = ' ';
		unsigned width = 0;
		if (*format == '0') {
			pad = '0';
			++format;
		}
		while (*format >= '0' && *format <= '9') {
			width = width * 10 + (unsigned) (*format++ - '0');
		}
		uint32_t value;
		unsigned base = 10;
		bool negative = false;
		switch (*format) {
		case 's': {
			const char* text = va_arg(args, const char*);
			while (text && *text) {
				PUT(*text++);
			}
			continue;
		}
		case 'd': {
			int v = va_arg(args, int);
			negative = v < 0;
			value = negative ? 0u - (uint32_t) v : (uint32_t) v;
			break;
		}
		case 'u':
			value = va_arg(args, unsigned);
			break;
		case 'X':
			value = va_arg(args, unsigned);
			base = 16;
			break;
		case '\0':
			--format; // the loop's ++ ends it
			continue;
		default:
			PUT(*format);
			continue;
		}
		char digits[12];
		unsigned count = 0;
		do {
			digits[count++] = "0123456789ABCDEF"[value % base];
			value /= base;
		} while (value && count < sizeof(digits));
		if (negative) {
			PUT('-');
		}
		while (width > count + (negative ? 1 : 0)) {
			PUT(pad);
			--width;
		}
		while (count) {
			PUT(digits[--count]);
		}
	}
#undef PUT
	out[n < cap ? n : cap - 1] = '\0';
}

void GBASIOCableTrace(struct GBASIO* sio, const char* format, ...) {
	(void) sio;
	if (!s_traceLine) {
		return;
	}
	char line[160];
	va_list args;
	va_start(args, format);
	_format(line, sizeof(line), format, args);
	va_end(args);
	s_traceLine(line);
}

void cable_air_set_trace(void (*line)(const char* text)) {
	s_traceLine = line;
}

// --- the bridge backend (client role) ---------------------------------------------------------------------------------

struct BridgeBackend {
	struct GBASIORFUBackend d;
	struct GBASIORFU* rfu;
	cable_air_emit_fn emit;
	uint32_t nowMs;
	bool connectPending;
	bool connected;
	uint16_t connectDevice;
	uint16_t ourId; // the id the bridge gave us in CONNECT_ACK
	uint32_t connectDeadline;
};

static struct BridgeBackend s_bridge;

static uint32_t _be32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static void _putBe32(uint8_t* p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void _sendRfu1(struct BridgeBackend* b, uint32_t type, uint32_t header, const uint8_t* body, size_t bodyLength, size_t size) {
	uint8_t frame[104] = {0};
	memcpy(frame, kRfu1, 4);
	_putBe32(&frame[4], type);
	_putBe32(&frame[8], header);
	if (body && bodyLength) {
		memcpy(&frame[12], body, bodyLength < size - 12 ? bodyLength : size - 12);
	}
	if (b->emit) {
		b->emit(frame, size);
	}
}

#ifdef __GNUC__
static void _trace(const char* format, ...) __attribute__((format(printf, 1, 2)));
#endif
static void _trace(const char* format, ...) {
	if (!s_traceLine) {
		return;
	}
	char line[160];
	va_list args;
	va_start(args, format);
	_format(line, sizeof(line), format, args);
	va_end(args);
	s_traceLine(line);
}

static bool _init(struct GBASIORFUBackend* backend, struct GBASIORFU* rfu) {
	struct BridgeBackend* b = (struct BridgeBackend*) backend;
	b->rfu = rfu;
	return true;
}

static void _deinit(struct GBASIORFUBackend* backend) {
	struct BridgeBackend* b = (struct BridgeBackend*) backend;
	if (b->connected || b->connectPending) {
		_sendRfu1(b, RFU1_DISCONNECT, 0, NULL, 0, 16);
	}
	b->connected = b->connectPending = false;
	b->rfu = NULL;
}

static void _searchStart(struct GBASIORFUBackend* backend) {
	(void) backend; // the bridge relays the room's broadcasts all the time
}

static void _searchStop(struct GBASIORFUBackend* backend) {
	(void) backend;
}

static void _connect(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct BridgeBackend* b = (struct BridgeBackend*) backend;
	b->connectPending = true;
	b->connected = false;
	b->connectDevice = deviceId;
	b->connectDeadline = b->nowMs + CONNECT_TIMEOUT_MS;
	_trace("BRIDGE connect request for %04X", deviceId);
	_sendRfu1(b, RFU1_CONNECT_REQ, deviceId, NULL, 0, 16);
}

static void _disconnect(struct GBASIORFUBackend* backend, unsigned slotMask) {
	(void) slotMask; // client role: leave the host
	struct BridgeBackend* b = (struct BridgeBackend*) backend;
	bool was = b->connected || b->connectPending;
	b->connected = b->connectPending = false;
	if (was) {
		_trace("BRIDGE disconnect");
		_sendRfu1(b, RFU1_DISCONNECT, b->ourId, NULL, 0, 16);
	}
}

// The game's child frame (LLSF header and slot): CLIENT_SEND, the length in the header's top byte. The rest of the header
// is our id, as the emulated adapter sends it; the bridge reads only the length.
static void _sendData(struct GBASIORFUBackend* backend, const uint8_t* data, size_t length) {
	struct BridgeBackend* b = (struct BridgeBackend*) backend;
	if (!b->connected) {
		return;
	}
	if (length > MAX_PAYLOAD) {
		length = MAX_PAYLOAD;
	}
	_sendRfu1(b, RFU1_CLIENT_SEND, ((uint32_t) length << 24) | b->ourId, data, length, 104);
}

// mGBA's _handleRfu1, client role. A DISCONNECT for us also ends the link here: the bridge sends one when the Switch's
// session ends and it starts over without a restart (the adapter is on this chip), which mGBA's USB link never sees.
static void _handleRfu1(struct BridgeBackend* b, const uint8_t* rfu1, size_t length) {
	if (!b->rfu) {
		return;
	}
	uint32_t type = _be32(&rfu1[4]);
	uint32_t header = _be32(&rfu1[8]);
	if (type == RFU1_BROADCAST && length >= 12 + 24) {
		uint32_t words[RFU_BROADCAST_WORDS];
		int i;
		for (i = 0; i < RFU_BROADCAST_WORDS; ++i) {
			words[i] = _be32(&rfu1[12 + i * 4]);
		}
		bool occupied = (header >> 16) & 1;
		GBASIORFUBroadcastReceived(b->rfu, (uint16_t) header, occupied ? 0xFF : 0, words);
	} else if (type == RFU1_CONNECT_ACK) {
		if (b->connectPending) {
			b->connectPending = false;
			b->connected = true;
			b->ourId = (uint16_t) header;
			_trace("BRIDGE connect accepted (our id %04X)", b->ourId);
			GBASIORFUConnectResult(b->rfu, true, b->connectDevice, 0);
		}
	} else if (type == RFU1_CONNECT_NACK) {
		if (b->connectPending) {
			b->connectPending = false;
			_trace("BRIDGE connect refused");
			GBASIORFUConnectResult(b->rfu, false, b->connectDevice, 0);
		}
	} else if (type == RFU1_HOST_SEND) {
		size_t n = header & 0x7F;
		if (n > MAX_PAYLOAD) {
			n = MAX_PAYLOAD;
		}
		if (b->connected && length >= 12 + n) {
			GBASIORFUDataReceived(b->rfu, 0, &rfu1[12], n);
		}
	} else if (type == RFU1_DISCONNECT) {
		if (b->connected && (uint16_t) header == b->ourId) {
			b->connected = false;
			_trace("BRIDGE the Switch's session ended");
			GBASIORFUDisconnected(b->rfu, 0);
		}
	}
	// CLIENT_ACK is the bridge's keep-alive; CONNECT_REQ and CLIENT_SEND are the host role's, which the wrapper has not.
}

struct GBASIORFUBackend* GBASIORFUBackendCreate(const char* name) {
	if (strcmp(name, "bridge") != 0) {
		return NULL;
	}
	cable_air_emit_fn emit = s_bridge.emit;
	uint32_t now = s_bridge.nowMs;
	memset(&s_bridge, 0, sizeof(s_bridge));
	s_bridge.emit = emit;
	s_bridge.nowMs = now;
	s_bridge.d.init = _init;
	s_bridge.d.deinit = _deinit;
	s_bridge.d.searchStart = _searchStart;
	s_bridge.d.searchStop = _searchStop;
	s_bridge.d.connect = _connect;
	s_bridge.d.disconnect = _disconnect;
	s_bridge.d.sendData = _sendData;
	return &s_bridge.d;
}

void GBASIORFUBackendDestroy(struct GBASIORFUBackend* backend) {
	(void) backend; // static
}

// --- the board's API --------------------------------------------------------------------------------------------------

static struct GBASIORFUWrapper s_wrapper;

bool cable_air_start(cable_air_emit_fn emit, uint32_t now_ms, void* memory, size_t size) {
	cable_air_stop();
	s_storage = memory;
	s_storageSize = size;
	s_storageUsed = false;
	s_bridge.emit = emit;
	s_bridge.nowMs = now_ms;
	memset(&s_wrapper, 0, sizeof(s_wrapper));
	return GBASIORFUWrapperAttachAir(&s_wrapper, "bridge", NULL);
}

void cable_air_stop(void) {
	if (s_wrapper.air && s_wrapper.airDestroy) {
		s_wrapper.airDestroy(s_wrapper.air);
	}
	memset(&s_wrapper, 0, sizeof(s_wrapper));
}

bool cable_air_running(void) {
	return s_wrapper.air != NULL;
}

void cable_air_net_packet(uint32_t type, uint32_t header, const uint8_t* payload, size_t length, uint32_t now_ms) {
	uint8_t frame[104] = {'R', 'F', 'U', '1'};
	if (length > sizeof(frame) - 12) {
		length = sizeof(frame) - 12;
	}
	_putBe32(&frame[4], type);
	_putBe32(&frame[8], header);
	memcpy(&frame[12], payload, length);
	cable_air_net_frame(frame, 12 + length, now_ms);
}

void cable_air_net_frame(const uint8_t* frame, size_t length, uint32_t now_ms) {
	s_bridge.nowMs = now_ms;
	if (!s_wrapper.air || length < 12 || memcmp(frame, kRfu1, 4) != 0) {
		return;
	}
	_handleRfu1(&s_bridge, frame, length);
}

void cable_air_frame(uint32_t now_ms) {
	s_bridge.nowMs = now_ms;
	if (!s_wrapper.air) {
		return;
	}
	if (s_bridge.connectPending && (int32_t) (now_ms - s_bridge.connectDeadline) >= 0) {
		s_bridge.connectPending = false;
		_trace("BRIDGE connect to %04X timed out (no CONNECT_ACK)", s_bridge.connectDevice);
		GBASIORFUConnectResult(s_bridge.rfu, false, s_bridge.connectDevice, 0);
	}
	if (s_wrapper.peer.frame) {
		s_wrapper.peer.frame(s_wrapper.peer.context);
	}
}

void cable_air_cable_reset(void) {
	if (s_wrapper.peer.reset) {
		s_wrapper.peer.reset(s_wrapper.peer.context);
	}
}

void cable_air_game_command(const uint16_t command[CABLE_AIR_CMD_WORDS]) {
	if (s_wrapper.peer.gameCommand) {
		s_wrapper.peer.gameCommand(s_wrapper.peer.context, command);
	}
}

bool cable_air_next_command(uint16_t command[CABLE_AIR_CMD_WORDS]) {
	return s_wrapper.peer.nextCommand && s_wrapper.peer.nextCommand(s_wrapper.peer.context, command);
}
