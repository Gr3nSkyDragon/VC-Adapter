/* Host-side test of nifi_match.c:  gcc -I../main nifi_match_test.c ../main/nifi_match.c -o nifi_match_test && ./nifi_match_test
 * The frames are built from the layouts melonDS's Wifi.cpp sends (SendMPDefaultReply, SendMPAck, TXSendFrame's CMD). */
#include <stdio.h>
#include <string.h>

#include "nifi_match.h"

static int failures;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                           \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (0)

static const uint8_t kOwn[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kHost[6] = {0x00, 0x09, 0xBF, 0x12, 0x34, 0x56};
static const uint8_t kClient[6] = {0x00, 0x1A, 0xE9, 0xAA, 0xBB, 0xCC};
static const uint8_t kOther[6] = {0x5C, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kCmd[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x00};
static const uint8_t kAck[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x03};
static const uint8_t kReply[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x10};

static size_t header(uint8_t *f, uint16_t fc, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3)
{
    memset(f, 0, 24);
    f[0] = (uint8_t)fc;
    f[1] = (uint8_t)(fc >> 8);
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    return 24;
}

static size_t beacon(uint8_t *f, const uint8_t *bssid, const uint8_t oui[3])
{
    size_t n = header(f, 0x0080, kBroadcast, bssid, bssid);
    memset(f + n, 0, 12);
    n += 12;
    f[n++] = 0; /* SSID, empty */
    f[n++] = 0;
    f[n++] = 1; /* rates */
    f[n++] = 2;
    f[n++] = 0x82;
    f[n++] = 0x84;
    f[n++] = 221; /* vendor */
    f[n++] = 8;
    memcpy(f + n, oui, 3);
    n += 3;
    memset(f + n, 0, 5);
    n += 5;
    return n;
}

int main(void)
{
    static const uint8_t kDsOui[3] = {0x00, 0x09, 0xBF}, k3dsOui[3] = {0x00, 0x1F, 0x32};
    uint8_t f[256];
    size_t n;
    nifi_match_t m;
    nifi_match_init(&m, kOwn, false);

    /* Before any beacon: a frame between strangers does not pass, a 3DS beacon does not either. */
    n = header(f, 0x0008, kHost, kClient, kHost);
    CHECK(nifi_match(&m, f, n + 8) == 0);
    n = beacon(f, kOther, k3dsOui);
    CHECK(nifi_match(&m, f, n) == 0);
    CHECK(!nifi_is_ds_beacon(f, n));

    /* The DS beacon passes and teaches the BSSID. */
    n = beacon(f, kHost, kDsOui);
    CHECK(nifi_is_ds_beacon(f, n));
    CHECK(nifi_match(&m, f, n) & NIFI_MATCH_BEACON);
    CHECK(m.bssid_count == 1);
    nifi_match(&m, f, n);
    CHECK(m.bssid_count == 1); /* not twice */

    /* A truncated vendor element is not a DS beacon. */
    CHECK(!nifi_is_ds_beacon(f, n - 6));

    /* Authentication from the client to the host: passes by the BSSID, and teaches the client. */
    n = header(f, 0x00B0, kHost, kClient, kHost);
    CHECK(nifi_match(&m, f, n + 6) & NIFI_MATCH_BSS);
    CHECK(m.station_count == 1);

    /* The host's ACK to the client (a control frame, one address). */
    uint8_t ack[10] = {0xD4, 0x00, 0x00, 0x00};
    memcpy(ack + 4, kClient, 6);
    CHECK(nifi_match(&m, ack, sizeof(ack)) & NIFI_MATCH_STATION);
    memcpy(ack + 4, kOther, 6);
    CHECK(nifi_match(&m, ack, sizeof(ack)) == 0);

    /* CMD (0x0228: to 03:09:BF:00:00:00 from the host), reply (0x0158: to the BSSID from the client, a3 = :10), ACK (0x0218). */
    n = header(f, 0x0228, kCmd, kHost, kHost);
    CHECK(nifi_match(&m, f, n + 4) & NIFI_MATCH_MP);
    n = header(f, 0x0158, kHost, kClient, kReply);
    CHECK((nifi_match(&m, f, n + 4) & (NIFI_MATCH_MP | NIFI_MATCH_BSS)) == (NIFI_MATCH_MP | NIFI_MATCH_BSS));
    n = header(f, 0x0218, kAck, kHost, kHost);
    CHECK(nifi_match(&m, f, n + 8) & NIFI_MATCH_MP);

    /* MP frames of a room whose beacon was never heard still pass. */
    static const uint8_t kStranger[6] = {0x00, 0x09, 0xBF, 0x99, 0x99, 0x99};
    n = header(f, 0x0228, kCmd, kStranger, kStranger);
    CHECK(nifi_match(&m, f, n + 4) == NIFI_MATCH_MP);

    /* Frames for our own MAC. */
    n = header(f, 0x0008, kOwn, kOther, kOther);
    CHECK(nifi_match(&m, f, n) & NIFI_MATCH_OWN);

    /* Too short for anything. */
    CHECK(nifi_match(&m, f, 9) == 0);

    /* All: the rest passes, marked as such. */
    nifi_match_t all;
    nifi_match_init(&all, kOwn, true);
    n = header(f, 0x0008, kOther, kBroadcast, kOther);
    CHECK(nifi_match(&all, f, n) == NIFI_MATCH_ALL);

    /* The BSSID ring keeps the newest four. */
    nifi_match_t ring;
    nifi_match_init(&ring, kOwn, false);
    for (int i = 0; i < 6; ++i) {
        uint8_t b[6] = {0x00, 0x09, 0xBF, 0, 0, (uint8_t)i};
        n = beacon(f, b, kDsOui);
        nifi_match(&ring, f, n);
    }
    CHECK(ring.bssid_count == NIFI_MAX_BSSIDS);
    uint8_t newest[6] = {0x00, 0x09, 0xBF, 0, 0, 5}, oldest[6] = {0x00, 0x09, 0xBF, 0, 0, 0};
    n = header(f, 0x00B0, newest, kClient, newest);
    CHECK(nifi_match(&ring, f, n) & NIFI_MATCH_BSS);
    n = header(f, 0x00B0, oldest, kClient, oldest);
    CHECK(!(nifi_match(&ring, f, n) & NIFI_MATCH_BSS));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("nifi_match: all passed\n");
    return 0;
}
