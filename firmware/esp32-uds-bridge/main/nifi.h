/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * Nintendo DS local wireless (NiFi). For now the capture only: the radio started with UDS_CMD_NIFI_START passes what
 * nifi_match.h picks out to the host as NIFI_RX events, with the receive timestamp, rate and FCS kept so the host can write a
 * pcap with radiotap and time the DS's CMD -> reply -> ACK exchange.
 *
 * The DS's multiplayer timing (a reply within its slot, microseconds after the host's CMD) is too tight for a round trip over
 * USB, so the part that answers CMDs and sends ACKs will live here on the board, next to the capture.
 */
#ifndef UDS_NIFI_H
#define UDS_NIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"
#include "radio.h"

/* NIFI_RX's header ahead of the MPDU (uds_wire.h has the layout). */
#define NIFI_RX_HEADER 16

void nifi_capture_reset(const uint8_t own_mac[6], uint8_t flags);

/* From radio.c's promiscuous callback, on the Wi-Fi task, while the radio runs in NiFi mode. `event` has room for
 * NIFI_RX_HEADER plus the largest MPDU and its FCS. Returns -1 when the frame is not wanted, otherwise whether the sink took it. */
int nifi_capture_rx(const wifi_promiscuous_pkt_t *packet, wifi_promiscuous_pkt_type_t type, uint8_t *event, size_t capacity,
                    uds_event_sink_t sink);

#endif
