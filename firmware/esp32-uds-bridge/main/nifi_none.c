/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
/* nifi_none.c: the ldn and aio lines, which leave the DS side (NiFi) out (Kconfig.projbuild, UDS_NIFI). main.c refuses the NIFI_*
 * commands, so the radio never enters NiFi mode and these are never reached but for init and stop. */
#include "nifi.h"
#include "nifi_host.h"

void nifi_capture_reset(const uint8_t own_mac[6], uint8_t flags)
{
    (void)own_mac;
    (void)flags;
}

int nifi_capture_rx(const wifi_promiscuous_pkt_t *packet, wifi_promiscuous_pkt_type_t type, uint8_t *event, size_t capacity,
                    uds_event_sink_t sink)
{
    (void)packet;
    (void)type;
    (void)event;
    (void)capacity;
    (void)sink;
    return -1;
}

esp_err_t nifi_host_init(uds_event_sink_t sink)
{
    (void)sink;
    return ESP_OK;
}

esp_err_t nifi_host_poll(const uint8_t *payload, size_t length)
{
    (void)payload;
    (void)length;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nifi_host_cmd(const uint8_t *payload, size_t length)
{
    (void)payload;
    (void)length;
    return ESP_ERR_NOT_SUPPORTED;
}

void nifi_host_stop(void)
{
}

void nifi_host_tx_done(const uint8_t *frame, size_t length, int64_t now_us)
{
    (void)frame;
    (void)length;
    (void)now_us;
}

void nifi_host_rx(const uint8_t *frame, size_t length, uint32_t callback_us)
{
    (void)frame;
    (void)length;
    (void)callback_us;
}
