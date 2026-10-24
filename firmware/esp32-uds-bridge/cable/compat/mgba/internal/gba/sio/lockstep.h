/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Stand-in for mGBA's <mgba/internal/gba/sio/lockstep.h>: only the cable trace, which the wrapper's AIRLOG writes to.
 * The board's version (cable_air.c) keeps the latest lines for the bridge's log. */
#ifndef CABLE_COMPAT_LOCKSTEP_H
#define CABLE_COMPAT_LOCKSTEP_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct GBASIO;

// No printf format check: the air file prints uint32_t with %X, right on mGBA's platforms and here (both 32-bit types),
// but uint32_t is unsigned long on the ESP32's compiler, whose check would then fail the build.
void GBASIOCableTrace(struct GBASIO* sio, const char* format, ...);

CXX_GUARD_END

#endif
