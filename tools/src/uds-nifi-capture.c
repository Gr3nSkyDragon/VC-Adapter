/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * Licensed under the GNU General Public License, version 3 or any later version. See LICENSE at the top of this repository. */

/*
 * uds-nifi-capture: captures Nintendo DS local wireless (NiFi) with an esp32-uds-bridge board in NiFi mode, into a pcap that
 * Wireshark opens (radiotap: the board's receive timestamp, rate, channel, signal and FCS), and sums up what the DS
 * multiplayer exchange looked like: the rooms seen, CMD -> reply -> ACK timing, the reply time each CMD asked for, whether
 * anything was WEP protected, and how late the board's capture callback ran.
 *
 * It answers what has to be known before the board can talk to a DS: does the ESP32 receive the DS's frames at all (the CMDs
 * and replies are CF-Poll / CF-Ack data subtypes), at what rate and preamble, how long a client has to answer a CMD, and how
 * much jitter the board's own receive path adds.
 *
 * Usage: uds-nifi-capture [--esp32 PORT] [--channel N | --scan 1,7,13] [--dwell MS] [--quiet MS] [--out FILE.pcap]
 *                         [--seconds N] [--all] [--fcs-fail] [--list] [--log]
 *   --esp32     the board's port (COM4, /dev/ttyACM0, /dev/cu.usbmodem1101); found by its USB ids on Windows, by name elsewhere
 *   --channel   stay on one channel
 *   --scan      channels to hop through until a DS beacon is heard (default 1,7,13), --dwell ms on each (default 1200)
 *   --quiet     back to hopping once the channel it stays on has heard nothing from a DS this long (default 3000 ms; 0: stay).
 *               A DS picks its channel (1, 7 or 13 in Nintendo's games) whenever it starts hosting, so a session that
 *               follows a scan, as a trade after the Union Room, can be on another channel than the beacons first heard.
 *   --out       the pcap (default nifi-<date>-<time>.pcap)
 *   --seconds   stop after this long (default: Ctrl+C)
 *   --all       every frame the radio hears, not only the DS's (big pcaps)
 *   --fcs-fail  frames with a failed FCS too (marked bad in the pcap): do the DS's frames arrive damaged?
 *   --list      one line per DS multiplayer frame on the console
 *   --log       the board's log lines
 *
 * Builds on Windows (Esp32Serial's COM ports) and on Linux and macOS (POSIX serial, installed with Esp32SerialSetOps).
 */
#include "uds-esp32.h"

#include "esp32-serial.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#define strtok_r strtok_s
#endif

#define MAX_CHANNELS 14
#define MAX_ROOMS 8
#define MAX_CLIENTS 16
#define MAX_SAMPLES 65536

// The POSIX serial port ------------------------------------------------------------------------------------------------------

#ifndef _WIN32
struct Esp32Serial {
	int fd;
};

static struct Esp32Serial* _posixOpen(const char* name, unsigned baud) {
	(void) baud; // the board's USB Serial/JTAG port ignores it
	int fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		return NULL;
	}
	struct termios tio;
	if (tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
#ifdef B921600
		cfsetispeed(&tio, B921600);
		cfsetospeed(&tio, B921600);
#else
		cfsetispeed(&tio, B115200);
		cfsetospeed(&tio, B115200);
#endif
		tio.c_cflag |= CLOCAL | CREAD;
		tio.c_cflag &= ~CRTSCTS;
		tcsetattr(fd, TCSANOW, &tio);
	}
	tcflush(fd, TCIOFLUSH);
	struct Esp32Serial* port = calloc(1, sizeof(*port));
	if (!port) {
		close(fd);
		return NULL;
	}
	port->fd = fd;
	return port;
}

static void _posixClose(struct Esp32Serial* port) {
	if (port) {
		close(port->fd);
		free(port);
	}
}

static int _posixRead(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	ssize_t got = read(port->fd, buffer, capacity);
	if (got < 0) {
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
	}
	return (int) got;
}

static bool _posixWrite(struct Esp32Serial* port, const void* data, size_t length) {
	const uint8_t* bytes = data;
	while (length) {
		ssize_t put = write(port->fd, bytes, length);
		if (put < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				return false;
			}
			struct pollfd pfd = {port->fd, POLLOUT, 0};
			if (poll(&pfd, 1, 2000) <= 0) {
				return false;
			}
			continue;
		}
		bytes += put;
		length -= (size_t) put;
	}
	return true;
}

// The first port that looks like the board: /dev/ttyACM* on Linux (the S3's USB Serial/JTAG), /dev/cu.usbmodem* on macOS.
static bool _posixFind(char* out, size_t capacity) {
	static const char* const kPatterns[] = {"/dev/ttyACM*", "/dev/cu.usbmodem*", "/dev/ttyUSB*", "/dev/cu.usbserial*"};
	size_t i;
	for (i = 0; i < sizeof(kPatterns) / sizeof(kPatterns[0]); ++i) {
		glob_t found;
		if (glob(kPatterns[i], 0, NULL, &found) == 0 && found.gl_pathc) {
			snprintf(out, capacity, "%s", found.gl_pathv[0]);
			globfree(&found);
			return true;
		}
		globfree(&found);
	}
	return false;
}

static const struct Esp32SerialOps kPosixOps = {_posixOpen, _posixClose, _posixRead, _posixWrite, _posixFind};
#endif

// Time ------------------------------------------------------------------------------------------------------------------------

static uint64_t _wallUs(void) {
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	uint64_t t = ((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100 ns since 1601
	return t / 10 - 11644473600000000ULL;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t) tv.tv_sec * 1000000 + (uint64_t) tv.tv_usec;
#endif
}

static uint32_t _nowMs(void) {
	return (uint32_t) (_wallUs() / 1000);
}

static void _sleepMs(unsigned ms) {
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
#endif
}

// Rates ----------------------------------------------------------------------------------------------------------------------

// wifi_phy_rate_t -> 500 kbit/s units (radiotap's), and whether it is a short-preamble DSSS rate.
static uint8_t _rate500k(uint8_t code, bool* shortPreamble, bool* ofdm) {
	static const uint8_t kOfdm[8] = {96, 48, 24, 12, 108, 72, 36, 18}; // 0x8..0xF: 48, 24, 12, 6, 54, 36, 18, 9 Mbit/s
	*shortPreamble = false;
	*ofdm = false;
	switch (code) {
	case 0: return 2;
	case 1: return 4;
	case 2: return 11;
	case 3: return 22;
	case 5: *shortPreamble = true; return 4;
	case 6: *shortPreamble = true; return 11;
	case 7: *shortPreamble = true; return 22;
	default:
		if (code >= 8 && code <= 15) {
			*ofdm = true;
			return kOfdm[code - 8];
		}
		return 0;
	}
}

static const char* _rateText(uint8_t code) {
	static char text[16];
	bool shortPreamble, ofdm;
	uint8_t r = _rate500k(code, &shortPreamble, &ofdm);
	if (!r) {
		snprintf(text, sizeof(text), "code %u", code);
	} else if (r == 11) {
		snprintf(text, sizeof(text), "5.5M%s", shortPreamble ? " short" : "");
	} else {
		snprintf(text, sizeof(text), "%uM%s", r / 2, ofdm ? "" : (shortPreamble ? " short" : " long"));
	}
	return text;
}

// Airtime of a DSSS frame in us (preamble and PLCP header, then the MPDU with its FCS).
static unsigned _airtimeUs(uint8_t code, size_t bytesWithFcs) {
	bool shortPreamble, ofdm;
	uint8_t r = _rate500k(code, &shortPreamble, &ofdm);
	if (!r || ofdm) {
		return 0;
	}
	return (shortPreamble ? 96 : 192) + (unsigned) ((bytesWithFcs * 16 + r - 1) / r);
}

// pcap ------------------------------------------------------------------------------------------------------------------------

static FILE* sPcap;
static unsigned sPcapFrames;

static void _put16(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}
static void _put32(uint8_t* p, uint32_t v) {
	_put16(p, (uint16_t) v);
	_put16(p + 2, (uint16_t) (v >> 16));
}
static void _put64(uint8_t* p, uint64_t v) {
	_put32(p, (uint32_t) v);
	_put32(p + 4, (uint32_t) (v >> 32));
}

static bool _pcapOpen(const char* path) {
	sPcap = fopen(path, "wb");
	if (!sPcap) {
		return false;
	}
	uint8_t header[24];
	_put32(header, 0xA1B2C3D4); // microseconds
	_put16(header + 4, 2);
	_put16(header + 6, 4);
	_put32(header + 8, 0);
	_put32(header + 12, 0);
	_put32(header + 16, 65535);
	_put32(header + 20, 127); // LINKTYPE_IEEE802_11_RADIOTAP
	return fwrite(header, sizeof(header), 1, sPcap) == 1;
}

// Radiotap: TSFT (the board's receive timestamp), Flags, Rate, Channel, dBm antenna signal. 23 bytes; each field naturally aligned.
#define RADIOTAP_LENGTH 23
static void _pcapWrite(uint64_t timeUs, uint64_t tsft, const struct UDSEsp32NifiRx* rx) {
	if (!sPcap) {
		return;
	}
	bool shortPreamble, ofdm;
	uint8_t rate = _rate500k(rx->rate, &shortPreamble, &ofdm);
	uint8_t rt[RADIOTAP_LENGTH];
	memset(rt, 0, sizeof(rt));
	_put16(rt + 2, RADIOTAP_LENGTH);
	_put32(rt + 4, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5));
	_put64(rt + 8, tsft);
	uint8_t flags = 0;
	if (shortPreamble) {
		flags |= 0x02;
	}
	if (rx->flags & UDS_ESP32_NIFI_RX_FCS) {
		flags |= 0x10;
	}
	if (rx->flags & UDS_ESP32_NIFI_RX_ERROR) {
		flags |= 0x40;
	}
	rt[16] = flags;
	rt[17] = rate;
	const unsigned channel = rx->channel;
	_put16(rt + 18, (uint16_t) (channel == 14 ? 2484 : 2407 + 5 * channel));
	_put16(rt + 20, (uint16_t) (0x0080 | (ofdm ? 0x0040 : 0x0020))); // 2 GHz, OFDM or CCK
	rt[22] = (uint8_t) rx->rssi;

	uint8_t record[16];
	_put32(record, (uint32_t) (timeUs / 1000000));
	_put32(record + 4, (uint32_t) (timeUs % 1000000));
	_put32(record + 8, (uint32_t) (RADIOTAP_LENGTH + rx->length));
	// The original length: what the radio reported (sigLen) when the board had to cut the frame short.
	size_t original = rx->length;
	if ((rx->flags & UDS_ESP32_NIFI_RX_TRUNCATED) && rx->sigLen > original) {
		original = rx->sigLen;
	}
	_put32(record + 12, (uint32_t) (RADIOTAP_LENGTH + original));
	fwrite(record, sizeof(record), 1, sPcap);
	fwrite(rt, sizeof(rt), 1, sPcap);
	fwrite(rx->mpdu, rx->length, 1, sPcap);
	++sPcapFrames;
}

// What was seen ---------------------------------------------------------------------------------------------------------------

struct Samples {
	uint32_t* v;
	size_t count;
	uint64_t total;
	uint32_t min, max;
};

static void _sample(struct Samples* s, uint32_t value) {
	if (!s->count || value < s->min) {
		s->min = value;
	}
	if (!s->count || value > s->max) {
		s->max = value;
	}
	s->total += value;
	if (!s->v) {
		s->v = malloc(sizeof(uint32_t) * MAX_SAMPLES);
	}
	if (s->v && s->count < MAX_SAMPLES) {
		s->v[s->count] = value;
	}
	++s->count;
}

static int _compareU32(const void* a, const void* b) {
	uint32_t x = *(const uint32_t*) a, y = *(const uint32_t*) b;
	return x < y ? -1 : x > y;
}

static void _printSamples(const char* name, struct Samples* s, const char* unit) {
	if (!s->count) {
		printf("  %-34s none\n", name);
		return;
	}
	size_t kept = s->count < MAX_SAMPLES ? s->count : MAX_SAMPLES;
	if (s->v) {
		qsort(s->v, kept, sizeof(uint32_t), _compareU32);
	}
	uint32_t p50 = s->v ? s->v[kept / 2] : 0, p99 = s->v ? s->v[(kept * 99) / 100 < kept ? (kept * 99) / 100 : kept - 1] : 0;
	printf("  %-34s n=%zu min %u  median %u  p99 %u  max %u %s\n", name, s->count, s->min, p50, p99, s->max, unit);
}

struct Room {
	uint8_t bssid[6];
	uint8_t channel;    // the last channel its beacons came on
	uint16_t channels;  // every channel they came on (bit n: channel n)
	uint16_t beaconIntervalTu;
	uint16_t capability;
	unsigned beacons;
	uint8_t vendor[64];
	uint8_t vendorLength;
};

struct Client {
	uint8_t mac[6];
	unsigned replies, emptyReplies;
	struct Samples replyDelay; // us from the start of the CMD to the start of this client's reply (both receive timestamps)
};

static struct {
	uint8_t locked; // the channel a DS beacon was heard on; 0 while scanning
	uint32_t lastDsMs; // when the last DS frame came (_nowMs), for --quiet
	struct { uint8_t channel; uint32_t atMs; } follows[32]; // the channels stayed on, in order
	unsigned followCount;
	uint32_t startMs;
	unsigned frames, dsFrames, beacons, cmds, replies, acks, other, protectedFrames, errors, misc, truncated, notLegacy;
	unsigned rates[16];
	struct Room rooms[MAX_ROOMS];
	unsigned roomCount;
	struct Client clients[MAX_CLIENTS];
	unsigned clientCount;
	unsigned replyTimes[8]; // the distinct reply times CMDs asked for
	unsigned replyTimeCount[8];
	unsigned replyTimeKinds;
	bool haveCmd;
	uint64_t cmdTs; // receive timestamp of the last CMD
	unsigned cmdAirtime;
	struct Samples cmdAirtimes;
	struct Samples ackDelay; // CMD start to ACK start
	struct Samples cmdInterval; // CMD start to the next CMD start
	struct Samples callbackLag; // (callback time - receive timestamp), less the smallest seen: the board's receive path jitter
	bool lagBase;
	int64_t lagMin;
	struct Samples lagRaw;
	uint64_t firstTs, lastTs;
	bool haveTs;
	uint32_t lastRaw;
	uint64_t unwrapped;
	uint64_t wallStart;
	bool list, log;
	int32_t startStatus;
	bool startAnswered;
} sSeen;

static const uint8_t kMpCmd[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x00};
static const uint8_t kMpAck[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x03};
static const uint8_t kMpReply[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x10};

static bool _macEqual(const uint8_t* a, const uint8_t* b) {
	return memcmp(a, b, 6) == 0;
}

static const char* _macText(const uint8_t* mac) {
	static char text[4][18];
	static unsigned next;
	char* out = text[next++ % 4];
	snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return out;
}

static struct Client* _client(const uint8_t* mac) {
	unsigned i;
	for (i = 0; i < sSeen.clientCount; ++i) {
		if (_macEqual(sSeen.clients[i].mac, mac)) {
			return &sSeen.clients[i];
		}
	}
	if (sSeen.clientCount == MAX_CLIENTS) {
		return NULL;
	}
	struct Client* c = &sSeen.clients[sSeen.clientCount++];
	memset(c, 0, sizeof(*c));
	memcpy(c->mac, mac, 6);
	return c;
}

static void _noteReplyTime(unsigned us) {
	unsigned i;
	for (i = 0; i < sSeen.replyTimeKinds; ++i) {
		if (sSeen.replyTimes[i] == us) {
			++sSeen.replyTimeCount[i];
			return;
		}
	}
	if (sSeen.replyTimeKinds < 8) {
		sSeen.replyTimes[sSeen.replyTimeKinds] = us;
		sSeen.replyTimeCount[sSeen.replyTimeKinds++] = 1;
	}
}

static void _noteBeacon(const uint8_t* f, size_t length, uint8_t channel) {
	const uint8_t* bssid = f + 16;
	struct Room* room = NULL;
	unsigned i;
	for (i = 0; i < sSeen.roomCount; ++i) {
		if (_macEqual(sSeen.rooms[i].bssid, bssid)) {
			room = &sSeen.rooms[i];
		}
	}
	if (!room) {
		if (sSeen.roomCount == MAX_ROOMS) {
			return;
		}
		room = &sSeen.rooms[sSeen.roomCount++];
		memset(room, 0, sizeof(*room));
		memcpy(room->bssid, bssid, 6);
	}
	if (room->beacons && room->channel != channel) {
		printf("DS room %s now on channel %u (was %u)\n", _macText(bssid), channel, room->channel);
	}
	room->channel = channel;
	room->channels |= (uint16_t) (1u << channel);
	room->beaconIntervalTu = (uint16_t) (f[32] | (f[33] << 8));
	room->capability = (uint16_t) (f[34] | (f[35] << 8));
	size_t offset = 36;
	while (offset + 2 <= length) {
		uint8_t tag = f[offset], len = f[offset + 1];
		if (offset + 2 + len > length) {
			break;
		}
		if (tag == 221 && len >= 3 && f[offset + 2] == 0x00 && f[offset + 3] == 0x09 && f[offset + 4] == 0xBF) {
			room->vendorLength = len < sizeof(room->vendor) ? len : sizeof(room->vendor);
			memcpy(room->vendor, f + offset + 2, room->vendorLength);
		}
		offset += 2 + len;
	}
	if (!room->beacons++) {
		printf("DS room %s on channel %u: beacon every %u TU, capability %04X%s\n", _macText(bssid), channel,
		       room->beaconIntervalTu, room->capability, (room->capability & 0x0010) ? " (privacy: WEP)" : "");
	}
}

static void _onRx(void* context, const struct UDSEsp32NifiRx* rx) {
	(void) context;
	++sSeen.frames;

	// The board's receive clock, unwrapped to 64 bits (it wraps every 71 minutes).
	if (!sSeen.haveTs) {
		sSeen.haveTs = true;
		sSeen.unwrapped = rx->rxTimestamp;
		sSeen.firstTs = sSeen.unwrapped;
		sSeen.wallStart = _wallUs();
	} else {
		sSeen.unwrapped += (uint32_t) (rx->rxTimestamp - sSeen.lastRaw);
	}
	sSeen.lastRaw = rx->rxTimestamp;
	const uint64_t ts = sSeen.unwrapped;
	sSeen.lastTs = ts;
	_pcapWrite(sSeen.wallStart + (ts - sSeen.firstTs), ts, rx);

	// The callback's lag behind the receive timestamp. The two are different clocks, so only its spread means anything.
	const int64_t lag = (int64_t) (int32_t) (rx->callbackUs - rx->rxTimestamp);
	if (!sSeen.lagBase || lag < sSeen.lagMin) {
		sSeen.lagMin = lag;
		sSeen.lagBase = true;
	}
	_sample(&sSeen.lagRaw, (uint32_t) (lag - INT32_MIN));

	if (rx->flags & UDS_ESP32_NIFI_RX_ERROR) {
		++sSeen.errors;
	}
	if (rx->flags & UDS_ESP32_NIFI_RX_MISC) {
		++sSeen.misc;
	}
	if (rx->flags & UDS_ESP32_NIFI_RX_TRUNCATED) {
		++sSeen.truncated;
	}
	if (rx->flags & UDS_ESP32_NIFI_RX_NOT_LEGACY) {
		++sSeen.notLegacy;
	}
	if (rx->match == UDS_ESP32_NIFI_MATCH_ALL) {
		return; // --all's other traffic: in the pcap, not in the summary
	}
	++sSeen.dsFrames;
	sSeen.lastDsMs = _nowMs();
	sSeen.rates[rx->rate & 15]++;

	const size_t length = (rx->flags & UDS_ESP32_NIFI_RX_FCS) && rx->length >= 4 ? rx->length - 4 : rx->length;
	const uint8_t* f = rx->mpdu;
	if (length < 10) {
		return;
	}
	const uint16_t fc = (uint16_t) (f[0] | (f[1] << 8));
	const int type = (fc >> 2) & 3;
	if ((fc & 0x4000) && type == 2) {
		++sSeen.protectedFrames;
	}
	if (rx->flags & UDS_ESP32_NIFI_RX_ERROR) {
		return; // damaged: counted, not trusted for timing
	}
	if (length < 24) {
		++sSeen.other;
		return;
	}
	const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16;
	const unsigned airtime = _airtimeUs(rx->rate, length + 4);

	if (rx->match & UDS_ESP32_NIFI_MATCH_BEACON) {
		++sSeen.beacons;
		if (!sSeen.locked) {
			sSeen.locked = rx->channel;
			if (sSeen.followCount < sizeof(sSeen.follows) / sizeof(sSeen.follows[0])) {
				sSeen.follows[sSeen.followCount].channel = rx->channel;
				sSeen.follows[sSeen.followCount++].atMs = sSeen.lastDsMs;
			}
			printf("  staying on channel %u (a DS beacon from %s)\n", rx->channel, _macText(a3));
		}
		_noteBeacon(f, length, rx->channel);
	} else if (type == 2 && _macEqual(a1, kMpCmd)) {
		++sSeen.cmds;
		unsigned replyTime = 0, mask = 0;
		if (length >= 28) {
			replyTime = f[24] | (f[25] << 8);
			mask = f[26] | (f[27] << 8);
			_noteReplyTime(replyTime);
		}
		if (sSeen.haveCmd) {
			_sample(&sSeen.cmdInterval, (uint32_t) (ts - sSeen.cmdTs));
		}
		sSeen.haveCmd = true;
		sSeen.cmdTs = ts;
		sSeen.cmdAirtime = airtime;
		_sample(&sSeen.cmdAirtimes, airtime);
		if (sSeen.list) {
			printf("%12.6f CMD   %s fc %04X %4zu B %-9s airtime %4u us  reply time %u us  clients %04X\n", (ts - sSeen.firstTs) / 1e6,
			       _macText(a2), fc, length, _rateText(rx->rate), airtime, replyTime, mask);
		}
	} else if (type == 2 && _macEqual(a3, kMpReply)) {
		++sSeen.replies;
		struct Client* c = _client(a2);
		const uint32_t delay = sSeen.haveCmd ? (uint32_t) (ts - sSeen.cmdTs) : 0;
		if (c) {
			++c->replies;
			if (length <= 28) {
				++c->emptyReplies;
			}
			if (sSeen.haveCmd && delay < 20000) {
				_sample(&c->replyDelay, delay);
			}
		}
		if (sSeen.list) {
			printf("%12.6f REPLY %s fc %04X %4zu B %-9s airtime %4u us  %u us after the CMD started (%d after it ended)\n",
			       (ts - sSeen.firstTs) / 1e6, _macText(a2), fc, length, _rateText(rx->rate), airtime, delay,
			       (int) delay - (int) sSeen.cmdAirtime);
		}
	} else if (type == 2 && _macEqual(a1, kMpAck)) {
		++sSeen.acks;
		const uint32_t delay = sSeen.haveCmd ? (uint32_t) (ts - sSeen.cmdTs) : 0;
		if (sSeen.haveCmd && delay < 20000) {
			_sample(&sSeen.ackDelay, delay);
		}
		if (sSeen.list) {
			unsigned fail = length >= 28 ? (unsigned) (f[26] | (f[27] << 8)) : 0;
			printf("%12.6f ACK   %s fc %04X %4zu B %-9s %u us after the CMD started, failed clients %04X\n", (ts - sSeen.firstTs) / 1e6,
			       _macText(a2), fc, length, _rateText(rx->rate), delay, fail);
		}
	} else {
		++sSeen.other;
		if (sSeen.list) {
			printf("%12.6f      %s -> %s fc %04X %4zu B %-9s\n", (ts - sSeen.firstTs) / 1e6, _macText(a2), _macText(a1), fc, length,
			       _rateText(rx->rate));
		}
	}
}

static void _onStatus(void* context, uint8_t requestType, int32_t result) {
	(void) context;
	if (requestType == UDS_ESP32_CMD_NIFI_START) {
		sSeen.startAnswered = true;
		sSeen.startStatus = result;
	}
}

static void _onLog(void* context, const char* text, size_t length) {
	(void) context;
	if (sSeen.log) {
		fprintf(stderr, "board: %.*s%s", (int) length, text, length && text[length - 1] == '\n' ? "" : "\n");
	}
}

static void _summary(void) {
	unsigned i;
	printf("\n== Summary ==\n");
	const double seconds = sSeen.haveTs ? (sSeen.lastTs - sSeen.firstTs) / 1e6 : 0;
	printf("%u frames in %.1f s (%u DS), %u written to the pcap\n", sSeen.frames, seconds, sSeen.dsFrames, sPcapFrames);
	printf("  beacons %u, CMDs %u, replies %u, ACKs %u, other %u\n", sSeen.beacons, sSeen.cmds, sSeen.replies, sSeen.acks, sSeen.other);
	printf("  WEP-protected data frames %u, receive errors %u, MISC %u, truncated %u, HT/VHT %u\n", sSeen.protectedFrames, sSeen.errors,
	       sSeen.misc, sSeen.truncated, sSeen.notLegacy);
	printf("  rates:");
	for (i = 0; i < 16; ++i) {
		if (sSeen.rates[i]) {
			printf("  %s x%u", _rateText((uint8_t) i), sSeen.rates[i]);
		}
	}
	printf("\n");
	for (i = 0; i < sSeen.roomCount; ++i) {
		const struct Room* r = &sSeen.rooms[i];
		char channelText[40] = "";
		unsigned ch;
		for (ch = 1; ch <= 14; ++ch) {
			if (r->channels & (1u << ch)) {
				snprintf(channelText + strlen(channelText), sizeof(channelText) - strlen(channelText), "%s%u", channelText[0] ? "+" : "", ch);
			}
		}
		printf("Room %s, channel %s, %u beacons, every %u TU, capability %04X; DS element (%u B):", _macText(r->bssid), channelText,
		       r->beacons, r->beaconIntervalTu, r->capability, r->vendorLength);
		unsigned b;
		for (b = 0; b < r->vendorLength; ++b) {
			printf(" %02X", r->vendor[b]);
		}
		printf("\n");
	}
	if (sSeen.replyTimeKinds) {
		printf("Reply time the CMDs asked for (us per client):");
		for (i = 0; i < sSeen.replyTimeKinds; ++i) {
			printf("  %u (x%u)", sSeen.replyTimes[i], sSeen.replyTimeCount[i]);
		}
		printf("\n");
	}
	if (sSeen.followCount) {
		printf("Channels followed:");
		unsigned f;
		for (f = 0; f < sSeen.followCount; ++f) {
			printf("%s %u from %.0f s", f ? "," : "", sSeen.follows[f].channel, (sSeen.follows[f].atMs - sSeen.startMs) / 1000.0);
		}
		printf("\n");
	}
	printf("Timing (receive timestamps, which mark a frame's start; CMD airtime is the CMD's own time on air):\n");
	_printSamples("CMD airtime", &sSeen.cmdAirtimes, "us");
	_printSamples("CMD to next CMD", &sSeen.cmdInterval, "us");
	_printSamples("CMD to ACK", &sSeen.ackDelay, "us");
	for (i = 0; i < sSeen.clientCount; ++i) {
		char name[48];
		snprintf(name, sizeof(name), "CMD to reply, %s", _macText(sSeen.clients[i].mac));
		_printSamples(name, &sSeen.clients[i].replyDelay, "us");
		printf("  %-34s %u replies, %u empty\n", "", sSeen.clients[i].replies, sSeen.clients[i].emptyReplies);
	}
	if (sSeen.lagRaw.count) {
		// Re-base the raw lags on the smallest one: what is left is how much later than its best the callback ran.
		size_t kept = sSeen.lagRaw.count < MAX_SAMPLES ? sSeen.lagRaw.count : MAX_SAMPLES;
		size_t k;
		for (k = 0; k < kept && sSeen.lagRaw.v; ++k) {
			_sample(&sSeen.callbackLag, (uint32_t) ((int64_t) sSeen.lagRaw.v[k] + INT32_MIN - sSeen.lagMin));
		}
		_printSamples("capture callback, beyond its best", &sSeen.callbackLag, "us");
	}
}

static volatile sig_atomic_t sStop;
static void _onSignal(int sig) {
	(void) sig;
	sStop = 1;
}

static void _usage(void) {
	fprintf(stderr, "usage: uds-nifi-capture [--esp32 PORT] [--channel N | --scan 1,7,13] [--dwell MS] [--quiet MS] [--out FILE.pcap]\n"
	                "                        [--seconds N] [--all] [--fcs-fail] [--list] [--log]\n");
}

int main(int argc, char** argv) {
	const char* portName = NULL;
	const char* outPath = NULL;
	char defaultOut[64];
	uint8_t channels[MAX_CHANNELS] = {1, 7, 13};
	unsigned channelCount = 3, dwellMs = 1200, quietMs = 3000, seconds = 0;
	uint8_t flags = 0;
	int i;
	for (i = 1; i < argc; ++i) {
		const char* a = argv[i];
		const bool more = i + 1 < argc;
		if (!strcmp(a, "--esp32") && more) {
			portName = argv[++i];
		} else if (!strcmp(a, "--channel") && more) {
			channels[0] = (uint8_t) atoi(argv[++i]);
			channelCount = 1;
		} else if (!strcmp(a, "--scan") && more) {
			channelCount = 0;
			char* list = argv[++i];
			char* save = NULL;
			char* token;
			for (token = strtok_r(list, ",", &save); token && channelCount < MAX_CHANNELS; token = strtok_r(NULL, ",", &save)) {
				channels[channelCount++] = (uint8_t) atoi(token);
			}
		} else if (!strcmp(a, "--dwell") && more) {
			dwellMs = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(a, "--quiet") && more) {
			quietMs = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(a, "--out") && more) {
			outPath = argv[++i];
		} else if (!strcmp(a, "--seconds") && more) {
			seconds = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(a, "--all")) {
			flags |= UDS_ESP32_NIFI_START_ALL;
		} else if (!strcmp(a, "--fcs-fail")) {
			flags |= UDS_ESP32_NIFI_START_FCS_FAIL;
		} else if (!strcmp(a, "--list")) {
			sSeen.list = true;
		} else if (!strcmp(a, "--log")) {
			sSeen.log = true;
		} else {
			_usage();
			return 2;
		}
	}
	unsigned c;
	for (c = 0; c < channelCount; ++c) {
		if (channels[c] < 1 || channels[c] > 13) {
			fprintf(stderr, "channel %u: the board takes 1 to 13\n", channels[c]);
			return 2;
		}
	}
	if (!channelCount) {
		_usage();
		return 2;
	}
	if (!outPath) {
		time_t now = time(NULL);
		struct tm* t = localtime(&now);
		strftime(defaultOut, sizeof(defaultOut), "nifi-%Y%m%d-%H%M%S.pcap", t);
		outPath = defaultOut;
	}

#ifndef _WIN32
	Esp32SerialSetOps(&kPosixOps);
#endif
	struct UDSEsp32Handlers handlers;
	memset(&handlers, 0, sizeof(handlers));
	handlers.nifiRx = _onRx;
	handlers.status = _onStatus;
	handlers.log = _onLog;
	static struct UDSEsp32 esp; // large
	if (!udsEsp32Open(&esp, portName, &handlers)) {
		fprintf(stderr, "No board: %s\n", portName ? "that port does not open" : "none found (give --esp32 PORT)");
		return 1;
	}
	printf("Waiting for the board...\n");
	if (!udsEsp32WaitReady(&esp, 10000)) {
		fprintf(stderr, "The board did not answer Hello.\n");
		udsEsp32Close(&esp);
		return 1;
	}
	printf("Board: firmware %u.%u, protocol %u, features %02X\n", esp.info.major, esp.info.minor, esp.info.proto, esp.info.features);
	if (!udsEsp32HasNifi(&esp)) {
		fprintf(stderr, "This firmware has no NiFi capture (feature 0x08): flash one built with it.\n");
		udsEsp32Close(&esp);
		return 1;
	}

	// Before the radio starts: the board sends frames as soon as it is up, before its answer to NIFI_START.
	if (!_pcapOpen(outPath)) {
		fprintf(stderr, "Cannot write %s\n", outPath);
		udsEsp32Close(&esp);
		return 1;
	}
	printf("Capturing into %s on channel %u%s. Ctrl+C stops.\n", outPath, channels[0],
	       channelCount > 1 ? " (hopping until a DS beacon is heard)" : "");

	// A locally administered MAC that no console uses, so the board's hardware ACKs nothing the DSes send each other.
	srand((unsigned) _wallUs());
	uint8_t mac[6] = {0x02, 0x4E, 0x49, (uint8_t) rand(), (uint8_t) rand(), (uint8_t) rand()};
	udsEsp32NifiStart(&esp, channels[0], flags, mac);
	uint32_t asked = _nowMs();
	while (!sSeen.startAnswered && _nowMs() - asked < 3000) {
		udsEsp32Poll(&esp);
		_sleepMs(2);
	}
	if (!sSeen.startAnswered || sSeen.startStatus != 0) {
		fprintf(stderr, "NIFI_START: %s\n", sSeen.startAnswered ? udsEsp32StatusText(sSeen.startStatus) : "no answer");
		fclose(sPcap);
		udsEsp32Close(&esp);
		return 1;
	}

	signal(SIGINT, _onSignal);
	signal(SIGTERM, _onSignal);
	const uint32_t start = _nowMs();
	sSeen.startMs = start;
	uint32_t hopped = start, lastStatus = start;
	unsigned current = 0;
	while (!sStop) {
		if (!udsEsp32Poll(&esp)) {
			fprintf(stderr, "The port failed.\n");
			break;
		}
		const uint32_t now = _nowMs();
		if (sSeen.locked && channelCount > 1 && quietMs && now - sSeen.lastDsMs >= quietMs) {
			// The DS went quiet here: it may be hosting again on another channel. Hop on from this one.
			printf("  channel %u quiet for %.1f s: scanning again\n", sSeen.locked, (now - sSeen.lastDsMs) / 1000.0);
			for (current = 0; current + 1 < channelCount && channels[current] != sSeen.locked; ++current) {
			}
			sSeen.locked = 0;
			hopped = now - dwellMs;
		}
		if (!sSeen.locked && channelCount > 1 && now - hopped >= dwellMs) {
			current = (current + 1) % channelCount;
			udsEsp32SetChannel(&esp, channels[current]);
			hopped = now;
		}
		if (now - lastStatus >= 2000) {
			printf("  %5.0f s  %s %u  frames %u  beacons %u  CMD %u  reply %u  ACK %u  WEP %u  errors %u\n", (now - start) / 1000.0,
			       sSeen.locked ? "channel" : "scanning", sSeen.locked ? sSeen.locked : channels[current], sSeen.frames, sSeen.beacons,
			       sSeen.cmds, sSeen.replies, sSeen.acks, sSeen.protectedFrames, sSeen.errors);
			fflush(stdout);
			if (sPcap) {
				fflush(sPcap);
			}
			lastStatus = now;
		}
		if (seconds && now - start >= seconds * 1000u) {
			break;
		}
		_sleepMs(1);
	}
	udsEsp32Stop(&esp);
	_sleepMs(50);
	udsEsp32Poll(&esp);
	udsEsp32Close(&esp);
	if (sPcap) {
		fclose(sPcap);
	}
	_summary();
	printf("Board framing: %u frames decoded, %u corrupt dropped\n", esp.decoder.framesOk, esp.decoder.framesBad);
	return 0;
}
