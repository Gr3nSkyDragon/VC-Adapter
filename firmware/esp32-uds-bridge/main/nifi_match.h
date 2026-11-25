/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * Which captured frames belong to Nintendo DS local wireless (NiFi). No ESP-IDF in here, so the host test
 * (test/nifi_match_test.c) builds it with gcc.
 *
 * A DS room is found by its beacon: a vendor element with Nintendo's DS OUI 00:09:BF (the 3DS uses 00:1F:32). The beacon's
 * BSSID is the host's MAC; it is learned, and from then on every frame to or from it passes (probe responses, authentication,
 * association, the clients' replies, whose first address is the BSSID). The stations seen talking to a learned BSSID are
 * learned too, so the link-layer ACKs sent to them pass. The DS multiplayer frames go to 03:09:BF:00:00:xx (CMD :00, ACK :03,
 * reply :10) and pass whatever their BSSID.
 *
 * Lengths are of the MPDU without its FCS.
 */
#ifndef UDS_NIFI_MATCH_H
#define UDS_NIFI_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NIFI_MAX_BSSIDS 4
#define NIFI_MAX_STATIONS 16

/* Why a frame passed (NIFI_RX's match byte). 0: it did not. */
#define NIFI_MATCH_BEACON 0x01  /* beacon or probe response with the DS vendor element */
#define NIFI_MATCH_MP 0x02      /* an address is 03:09:BF:00:00:xx */
#define NIFI_MATCH_BSS 0x04     /* an address is a learned BSSID */
#define NIFI_MATCH_STATION 0x08 /* an address is a learned station (ACKs to clients) */
#define NIFI_MATCH_OWN 0x10     /* an address is ours */
#define NIFI_MATCH_ALL 0x80     /* the capture passes everything and nothing else matched */

typedef struct {
    uint8_t own[6];
    uint8_t bssid[NIFI_MAX_BSSIDS][6];
    uint8_t bssid_count, bssid_next;
    uint8_t station[NIFI_MAX_STATIONS][6];
    uint8_t station_count, station_next;
    bool all;
} nifi_match_t;

void nifi_match_init(nifi_match_t *match, const uint8_t own[6], bool all);
/* True for a beacon (subtype 8) or probe response (5) carrying the DS vendor element (DD, 00 09 BF). */
bool nifi_is_ds_beacon(const uint8_t *frame, size_t length);
/* The NIFI_MATCH_* bits for this frame, learning BSSIDs and stations as it goes. Control frames are matched on their first
 * address only. */
uint8_t nifi_match(nifi_match_t *match, const uint8_t *frame, size_t length);

#endif
