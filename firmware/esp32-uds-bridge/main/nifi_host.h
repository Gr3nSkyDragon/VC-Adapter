/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * The board as a Nintendo DS host, an experiment (UDS_CMD_NIFI_POLL). A retail DS joined to the board (its authentication and
 * association answered by the PC, through TX_FRAME) is polled the way a DS host polls: a CMD (Data+CF-Poll to 03:09:BF:00:00:00)
 * every interval, the client's hardware replying a SIFS after it, and the host's MP ACK (to 03:09:BF:00:00:03) after the reply
 * slot. The captures put that ACK about 395 us after the reply slot starts. What is not known is how closely esp_wifi_80211_tx
 * can place it, and how closely a retail client checks; this sends the ACK after a set delay, or as soon as a reply is heard, and
 * reports every CMD and ACK (UDS_EVT_NIFI_TX) with its transmit-done and reply times.
 *
 * The polling runs in a task of its own on the second core, which busy-waits the few hundred microseconds to the ACK.
 */
#ifndef UDS_NIFI_HOST_H
#define UDS_NIFI_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio.h"

esp_err_t nifi_host_init(uds_event_sink_t sink);
/* NIFI_POLL's payload (uds_wire.h). interval_us 0 stops the polling. */
esp_err_t nifi_host_poll(const uint8_t *payload, size_t length);
/* NIFI_CMD's payload: a CMD frame from the host (melonDS), sent now, then its ACK. No STATUS: NIFI_TX reports it. */
esp_err_t nifi_host_cmd(const uint8_t *payload, size_t length);
void nifi_host_stop(void);

/* From radio.c's transmit-done callback (the Wi-Fi task), for every frame sent while the radio is in NiFi mode. */
void nifi_host_tx_done(const uint8_t *frame, size_t length, int64_t now_us);
/* From the NiFi capture (the Wi-Fi task): a received MPDU (no FCS) and the capture callback's time. */
void nifi_host_rx(const uint8_t *frame, size_t length, uint32_t callback_us);

#endif
