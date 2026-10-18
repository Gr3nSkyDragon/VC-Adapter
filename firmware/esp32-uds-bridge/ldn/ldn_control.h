#pragma once
#include <stdbool.h>
#include "esp_netif.h"
void ldn_control_init(esp_netif_t *netif, const unsigned char host[6]);
void ldn_control_link(bool connected);
void ldn_control_poll(void);
void ldn_control_sniff(const unsigned char *frame, size_t length);
void ldn_control_target(const unsigned char host[6]);

/* Action frames for on-device room authentication. */
int ldn_control_tx_action(const uint8_t *frame, size_t length);
void ldn_control_set_action_handler(void (*handler)(const uint8_t source[6], const uint8_t *body, size_t length));
bool ldn_control_connected(void);
/* Milliseconds since the console last delivered bytes; INT64_MAX before any. */
int64_t ldn_control_console_age_ms(void);
void ldn_control_mac(uint8_t out[6]);
/* Firmware version reported by LDN_INFO. */
#define BRIDGE_VERSION "2.1.5"
/* Apply a baud rate requested by LDN_BAUD. Call after the reply is sent. */
void ldn_control_apply_baud(void);
/* Flush adapter and console output before a restart. */
void ldn_control_flush_adapter(void);
