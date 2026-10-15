/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * Licensed under the GNU General Public License, version 3 or any later version. See LICENSE at the top of this repository. */

/*
 * Stands in for mGBA's mgba-util/common.h, so these files build without mGBA: the C headers they expect from it and the
 * three macros they use. The other files here are copies of mGBA-LDN's src/gb/sio, src/gba/sio/esp32 and src/util/md5.c.
 */
#ifndef UDS_SETUP_COMMON_H
#define UDS_SETUP_COMMON_H

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
#define CXX_GUARD_START extern "C" {
#define CXX_GUARD_END }
#else
#define CXX_GUARD_START
#define CXX_GUARD_END
#endif

#define LOAD_32LE(DEST, ADDR, ARR) DEST = ((uint32_t) ((const uint8_t*) (ARR))[(ADDR)] | \
	((uint32_t) ((const uint8_t*) (ARR))[(ADDR) + 1] << 8) | \
	((uint32_t) ((const uint8_t*) (ARR))[(ADDR) + 2] << 16) | \
	((uint32_t) ((const uint8_t*) (ARR))[(ADDR) + 3] << 24))
#define STORE_32LE(SRC, ADDR, ARR) do { \
	uint32_t _v = (SRC); \
	uint8_t* _p = (uint8_t*) (ARR) + (ADDR); \
	_p[0] = (uint8_t) _v; \
	_p[1] = (uint8_t) (_v >> 8); \
	_p[2] = (uint8_t) (_v >> 16); \
	_p[3] = (uint8_t) (_v >> 24); \
} while (0)

#endif
