/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
#include "nifi.h"

#include <string.h>

#include "esp_timer.h"
#include "nifi_match.h"
#include "soc/soc_caps.h"
#include "uds_wire.h"

static nifi_match_t s_match; /* the Wi-Fi task's only, once the radio runs */

void nifi_capture_reset(const uint8_t own_mac[6], uint8_t flags)
{
    nifi_match_init(&s_match, own_mac, (flags & UDS_NIFI_START_ALL) != 0);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

int nifi_capture_rx(const wifi_promiscuous_pkt_t *packet, wifi_promiscuous_pkt_type_t type, uint8_t *event, size_t capacity,
                    uds_event_sink_t sink)
{
    const uint32_t callback_us = (uint32_t)esp_timer_get_time(); /* first: this is what the reply timing will be up against */
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL && type != WIFI_PKT_MISC) return -1;

    const wifi_pkt_rx_ctrl_t *rx = &packet->rx_ctrl;
    size_t with_fcs = rx->sig_len;
    if (with_fcs < 10 + 4) return -1;
    const uint8_t match = nifi_match(&s_match, packet->payload, with_fcs - 4);
    if (!match) return -1;

    uint8_t flags = UDS_NIFI_RX_FCS;
    if (rx->rx_state != 0) flags |= UDS_NIFI_RX_ERROR;
    if (type == WIFI_PKT_MISC) flags |= UDS_NIFI_RX_MISC;
#if !SOC_WIFI_HE_SUPPORT
    if (rx->sig_mode != 0) flags |= UDS_NIFI_RX_NOT_LEGACY; /* 0: 802.11b/g; the DS sends nothing else */
#endif
    size_t copy = with_fcs;
    if (copy > capacity - NIFI_RX_HEADER) {
        copy = capacity - NIFI_RX_HEADER;
        flags |= UDS_NIFI_RX_TRUNCATED;
    }
    if (copy > UDS_WIRE_MAX_PAYLOAD - NIFI_RX_HEADER) {
        copy = UDS_WIRE_MAX_PAYLOAD - NIFI_RX_HEADER;
        flags |= UDS_NIFI_RX_TRUNCATED;
    }

    event[0] = (uint8_t)rx->channel;
    event[1] = (uint8_t)(int8_t)rx->rssi;
    event[2] = (uint8_t)rx->rate;
    event[3] = flags;
    event[4] = match;
    event[5] = (uint8_t)rx->rx_state;
    put32(event + 6, (uint32_t)rx->timestamp);
    put32(event + 10, callback_us);
    event[14] = (uint8_t)with_fcs;
    event[15] = (uint8_t)(with_fcs >> 8);
    memcpy(event + NIFI_RX_HEADER, packet->payload, copy);
    return sink && sink(UDS_EVT_NIFI_RX, event, NIFI_RX_HEADER + copy) ? 1 : 0;
}
