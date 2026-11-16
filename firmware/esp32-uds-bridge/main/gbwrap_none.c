/* gbwrap_none.c — omit the on-board Game Boy wrapper from the S2 radio. */
#include "gbwrap.h"
#include "uds_wire.h"

static gbwrap_send_t send_frame;

void gbwrap_init(gbwrap_send_t send)
{
    send_frame = send;
}

void gbwrap_request(uint8_t type, uint8_t seq, const uint8_t *payload, size_t length)
{
    (void)payload;
    (void)length;
    const uint8_t reply[] = {type, ESP_ERR_NOT_SUPPORTED & 0xff, ESP_ERR_NOT_SUPPORTED >> 8, 0, 0};
    send_frame(UDS_EVT_STATUS, seq, reply, sizeof(reply));
}

bool gbwrap_active(void)
{
    return false;
}

void gbwrap_host_hello(void)
{
}

bool gbwrap_sink_rx(const uint8_t *payload, size_t length)
{
    (void)payload;
    (void)length;
    return false;
}

void gbwrap_get_status(gbwrap_status_t *status)
{
    *status = (gbwrap_status_t){0};
}
