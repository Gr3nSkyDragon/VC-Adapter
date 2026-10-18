#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ldn_wire_enable(void);
bool ldn_wire_active(void);
void ldn_wire_session(uint32_t session);
void ldn_wire_feed(uint8_t byte, void (*dispatch)(const char *));
/* Frames from the host that arrived damaged and were dropped. */
uint32_t ldn_wire_bad_frames(void);

/* Binary RFU1 adapter frames, framed like UDP datagrams (kinds 4/5).
   Kind 6 to the host, kind 7 from it. */
void ldn_wire_send_rfu(const uint8_t *frame, size_t length);
void ldn_wire_set_rfu_handler(void (*handler)(const uint8_t *frame, size_t length));
int ldn_wire_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
