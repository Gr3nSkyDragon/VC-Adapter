/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
#include "nifi_match.h"

#include <string.h>

static const uint8_t kMpPrefix[5] = {0x03, 0x09, 0xBF, 0x00, 0x00};

static bool mac_equal(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static bool is_group(const uint8_t *mac) { return (mac[0] & 1) != 0; }

void nifi_match_init(nifi_match_t *match, const uint8_t own[6], bool all)
{
    memset(match, 0, sizeof(*match));
    memcpy(match->own, own, 6);
    match->all = all;
}

bool nifi_is_ds_beacon(const uint8_t *frame, size_t length)
{
    if (length < 24 + 12) return false;
    const int type = (frame[0] >> 2) & 3, subtype = (frame[0] >> 4) & 0xF;
    if (type != 0 || (subtype != 8 && subtype != 5)) return false;
    size_t offset = 24 + 12; /* management header + timestamp, interval, capabilities */
    while (offset + 2 <= length) {
        const uint8_t tag = frame[offset], len = frame[offset + 1];
        offset += 2;
        if (offset + len > length) return false;
        if (tag == 221 && len >= 4 && frame[offset] == 0x00 && frame[offset + 1] == 0x09 && frame[offset + 2] == 0xBF)
            return true;
        offset += len;
    }
    return false;
}

static bool in_list(const uint8_t (*list)[6], uint8_t count, const uint8_t *mac)
{
    for (uint8_t i = 0; i < count; ++i)
        if (mac_equal(list[i], mac)) return true;
    return false;
}

/* A small ring: the oldest entry goes when it is full. */
static void learn(uint8_t (*list)[6], uint8_t capacity, uint8_t *count, uint8_t *next, const uint8_t *mac)
{
    if (is_group(mac) || in_list((const uint8_t (*)[6])list, *count, mac)) return;
    memcpy(list[*next], mac, 6);
    *next = (uint8_t)((*next + 1) % capacity);
    if (*count < capacity) ++*count;
}

static uint8_t match_address(const nifi_match_t *match, const uint8_t *mac)
{
    uint8_t bits = 0;
    if (memcmp(mac, kMpPrefix, sizeof(kMpPrefix)) == 0) bits |= NIFI_MATCH_MP;
    if (in_list((const uint8_t (*)[6])match->bssid, match->bssid_count, mac)) bits |= NIFI_MATCH_BSS;
    if (in_list((const uint8_t (*)[6])match->station, match->station_count, mac)) bits |= NIFI_MATCH_STATION;
    if (mac_equal(mac, match->own)) bits |= NIFI_MATCH_OWN;
    return bits;
}

uint8_t nifi_match(nifi_match_t *match, const uint8_t *frame, size_t length)
{
    if (length < 10) return 0;
    const int type = (frame[0] >> 2) & 3;
    uint8_t bits = 0;

    if (type == 1) {
        /* Control: ACK and CTS have one address, RTS and the rest two; only the receiver is looked at. */
        bits = match_address(match, frame + 4) & (NIFI_MATCH_BSS | NIFI_MATCH_STATION | NIFI_MATCH_OWN);
    } else if (length >= 24) {
        const uint8_t *a1 = frame + 4, *a2 = frame + 10, *a3 = frame + 16;
        if (nifi_is_ds_beacon(frame, length)) {
            bits |= NIFI_MATCH_BEACON;
            learn(match->bssid, NIFI_MAX_BSSIDS, &match->bssid_count, &match->bssid_next, a3);
        }
        bits |= match_address(match, a1) | match_address(match, a2) | match_address(match, a3);
        /* A station talking to a learned BSS: the transmitter, unless it is the BSSID itself. */
        if ((bits & NIFI_MATCH_BSS) && !(bits & NIFI_MATCH_BEACON) && !is_group(a2) &&
            !in_list((const uint8_t (*)[6])match->bssid, match->bssid_count, a2))
            learn(match->station, NIFI_MAX_STATIONS, &match->station_count, &match->station_next, a2);
    }
    if (!bits && match->all) bits = NIFI_MATCH_ALL;
    return bits;
}
