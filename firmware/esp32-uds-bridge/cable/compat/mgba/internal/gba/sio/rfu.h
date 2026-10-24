/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Stand-in for mGBA's <mgba/internal/gba/sio/rfu.h>: the part of mGBA's wireless adapter the wrapper's air file uses.
 * There it runs a headless adapter for its event queue and opens a backend; here the adapter is only that queue, and
 * the one backend is the bridge on this chip (cable_air.c). The types and names are mGBA's, so the air file builds
 * unchanged; the event queue is shorter (it is drained every GBA frame). */
#ifndef CABLE_COMPAT_RFU_H
#define CABLE_COMPAT_RFU_H

#include <mgba-util/common.h>

CXX_GUARD_START

enum {
	RFU_BROADCAST_WORDS = 6,
	RFU_PACKET_MAX = 96,
	RFU_EVENT_QUEUE = 8,
};

enum GBASIORFUEventType {
	RFU_EVENT_BROADCAST,
	RFU_EVENT_CONNECT_RESULT,
	RFU_EVENT_CLIENT_JOINED,
	RFU_EVENT_DATA,
	RFU_EVENT_DISCONNECTED,
	RFU_EVENT_CONNECT_REQUEST,
};

struct GBASIORFUEvent {
	enum GBASIORFUEventType type;
	uint16_t deviceId;
	int slot;
	bool accepted;
	uint8_t length;
	uint32_t words[RFU_BROADCAST_WORDS];
	uint8_t data[RFU_PACKET_MAX];
};

struct GBASIORFU;

struct GBASIORFUBackend {
	bool (*init)(struct GBASIORFUBackend*, struct GBASIORFU*);
	void (*deinit)(struct GBASIORFUBackend*);
	void (*reset)(struct GBASIORFUBackend*);
	void (*frame)(struct GBASIORFUBackend*);
	void (*poll)(struct GBASIORFUBackend*);
	void (*setBroadcast)(struct GBASIORFUBackend*, const uint32_t data[RFU_BROADCAST_WORDS]);
	void (*hostStart)(struct GBASIORFUBackend*, uint16_t deviceId);
	void (*hostStop)(struct GBASIORFUBackend*);
	void (*connectReply)(struct GBASIORFUBackend*, uint16_t clientId, bool accepted, unsigned slot);
	void (*searchStart)(struct GBASIORFUBackend*);
	void (*searchStop)(struct GBASIORFUBackend*);
	void (*connect)(struct GBASIORFUBackend*, uint16_t deviceId);
	void (*disconnect)(struct GBASIORFUBackend*, unsigned slotMask);
	void (*sendData)(struct GBASIORFUBackend*, const uint8_t* data, size_t length);
};

// The queue itself is cable_air.c's: the air file holds this struct in its own state, which on the board shares the
// adapter core's memory, and the queue would not fit there too.
struct GBASIORFU {
	struct GBASIORFUBackend* backend;
	struct GBASIORFUEvent* events; // RFU_EVENT_QUEUE of them
	unsigned eventHead;
	unsigned eventTail;
};

// On the board the air file's one allocation, its state, goes to the memory cable_air_start is given (the adapter core's,
// unused while the cable runs): the air file's calloc and free are these. The PC test turns this on too
// (CABLE_AIR_HOOK_ALLOC), with <stdlib.h> included first.
#if defined(ESP_PLATFORM) || defined(CABLE_AIR_HOOK_ALLOC)
void* cable_air_calloc(size_t count, size_t size);
void cable_air_free(void* memory);
#define calloc cable_air_calloc
#define free cable_air_free
#endif

void GBASIORFUCreate(struct GBASIORFU*, struct GBASIORFUBackend*);
void GBASIORFUDestroy(struct GBASIORFU*);
void GBASIORFUSetTraceFile(struct GBASIORFU*, const char* path);

// Only "bridge": the Switch side's bridge on this chip.
struct GBASIORFUBackend* GBASIORFUBackendCreate(const char* name);
void GBASIORFUBackendDestroy(struct GBASIORFUBackend*);

void GBASIORFUBroadcastReceived(struct GBASIORFU*, uint16_t deviceId, uint8_t nextSlot, const uint32_t data[RFU_BROADCAST_WORDS]);
void GBASIORFUConnectResult(struct GBASIORFU*, bool accepted, uint16_t deviceId, unsigned slot);
void GBASIORFUDataReceived(struct GBASIORFU*, unsigned slot, const uint8_t* data, size_t length);
void GBASIORFUDisconnected(struct GBASIORFU*, int slot);
bool GBASIORFUPopEvent(struct GBASIORFU*, struct GBASIORFUEvent* out);

CXX_GUARD_END

#endif
