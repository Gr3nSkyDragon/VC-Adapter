#pragma once
#include <stdbool.h>
#include "esp_err.h"

const char *ldn_session_ssid(void);
esp_err_t ldn_session_configure(const char *ssid, const char *bssid, const char *key, unsigned channel);
esp_err_t ldn_session_scan(unsigned channel);
void ldn_session_stop(void);
/* While hosting: keep decoding other hosts' advertisements on the hosted channel. */
void ldn_session_watch_advertisements(void);
void ldn_control_target(const unsigned char host[6]);

/* RSSI of the last LDN action frame, and min/avg/max since the previous call. */
int8_t ldn_probe_last_rssi(void);
void ldn_probe_rssi_window(int *count, int *average, int *low, int *high);
/* External (true) or onboard antenna. Returns false if the board has no switch. */
bool bridge_board_antenna(bool external);
/* The same, chosen by hand: ends the comparison of the antennas made after boot. */
bool ldn_session_set_antenna(bool external);
/* Applies CONFIG_BRIDGE_WIFI_MAX_TX_POWER; the driver forgets it on every esp_wifi_start. */
void bridge_wifi_tx_power(void);
