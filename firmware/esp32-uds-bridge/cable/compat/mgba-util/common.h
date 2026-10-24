/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Stand-in for mGBA's <mgba-util/common.h>: what ../../rfu-wrapper-air.c needs of it on the board. */
#ifndef CABLE_COMPAT_MGBA_UTIL_COMMON_H
#define CABLE_COMPAT_MGBA_UTIL_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#define CXX_GUARD_START extern "C" {
#define CXX_GUARD_END }
#else
#define CXX_GUARD_START
#define CXX_GUARD_END
#endif

#endif
