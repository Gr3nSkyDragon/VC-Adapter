#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_netif.h"

void ldn_udp_init(esp_netif_t *netif, const uint8_t host[6]);
bool ldn_udp_command(const char *line, bool connected);
void ldn_udp_poll(bool connected);
void ldn_udp_stop(void);

/* Host mode: socket on the AP netif with a static address and no fixed peer. Peers are
   added as members authenticate and removed as they leave. No heartbeat watchdog. */
int ldn_udp_host_start(esp_netif_t *netif, const char *our_ip);
int ldn_udp_add_peer(const char *ip, const uint8_t mac[6]);
int ldn_udp_remove_peer(const char *ip);

/* Binary datagram path for the on-device session layer, bypassing the text protocol.
   With a handler set, received datagrams go to it instead of LDN_DATAGRAM lines. */
bool ldn_udp_send(const char *ip, const uint8_t *data, size_t length);
/* Feeds the 10 s watchdog that drops the link when host pings stop. Used by the
   on-device bridge in place of LDN_PING. */
void ldn_udp_heartbeat(void);
void ldn_udp_set_handler(void (*handler)(const char *ip, const uint8_t *data, size_t length));
