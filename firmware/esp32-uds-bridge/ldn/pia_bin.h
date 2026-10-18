#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Byte helpers, mirror of the host's Bin class. Pia transport headers are big-endian,
   adapter payloads little-endian. */

static inline uint16_t bin_u16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static inline uint32_t bin_u32(const uint8_t *b)
{ return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static inline uint64_t bin_u64(const uint8_t *b)
{ return (uint64_t)bin_u32(b) | (uint64_t)bin_u32(b + 4) << 32; }

static inline uint16_t bin_b16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }
static inline uint32_t bin_b32(const uint8_t *b)
{ return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3]; }
static inline uint64_t bin_b64(const uint8_t *b)
{ return (uint64_t)bin_b32(b) << 32 | (uint64_t)bin_b32(b + 4); }

static inline void bin_w16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void bin_w32(uint8_t *b, uint32_t v)
{ b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static inline void bin_w64(uint8_t *b, uint64_t v) { bin_w32(b, (uint32_t)v); bin_w32(b + 4, (uint32_t)(v >> 32)); }

static inline void bin_wb16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static inline void bin_wb32(uint8_t *b, uint32_t v)
{ b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static inline void bin_wb64(uint8_t *b, uint64_t v) { bin_wb32(b, (uint32_t)(v >> 32)); bin_wb32(b + 4, (uint32_t)v); }

/* True when 16-bit sequence a is behind b, with wraparound. */
static inline bool bin_less(uint16_t a, uint16_t b)
{ uint16_t d = (uint16_t)(b - a); return d > 0 && d < 32768; }

uint32_t bin_crc32(const uint8_t *data, size_t length);
