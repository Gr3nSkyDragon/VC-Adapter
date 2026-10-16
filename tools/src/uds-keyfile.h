/* Copyright (c) 2026 Gr3nSkyDragon
 *
 * Licensed under the GNU General Public License, version 3 or any later version. See LICENSE at the top of this repository. */
#ifndef GB_SIO_UDS_KEYFILE_H
#define GB_SIO_UDS_KEYFILE_H

#include "common.h"

CXX_GUARD_START

/*
 * The 3DS UDS data key (AES key slot 0x2D) for the real-air side of the Virtual Console wrapper. It is a Nintendo secret that is
 * the same on every console; it is never shipped with mGBA and never logged. The user supplies it as a file in the format
 * Citra and Azahar read (one `name=hex` line per key, `#` or `;` or `//` start a comment), and Settings > BIOS points at it.
 *
 * The file may be
 *   - the finished key on one line:                 slot0x2DKeyN=<32 hex digits>
 *   - or slot0x2DKeyX and slot0x2DKeyY (as in a full aes_keys.txt), from which the key is made as Azahar does:
 *     normal = ((KeyX <<< 2) xor KeyY) + generator, rotated left 87, all as 128-bit big-endian values. The generator is the
 *     constant built into uds-keyfile.c unless the file has its own generatorConstant (or generator) line, which wins.
 * Everything else in the file is ignored; if the finished key and KeyX/KeyY are both present the finished key is used.
 */

enum UDSKeyStatus {
	UDS_KEY_OK_NORMAL, // slot0x2DKeyN was in the file
	UDS_KEY_OK_DERIVED, // made from KeyX and KeyY (with the built-in or the file's generator constant)
	UDS_KEY_NO_FILE, // empty path or the file cannot be read
	UDS_KEY_NOT_FOUND, // no slot 0x2D key in the file
	UDS_KEY_INCOMPLETE, // only one of KeyX and KeyY is present, and there is no KeyN
	UDS_KEY_BAD_VALUE, // a line for slot 0x2D (or the generator) is not 32 hex digits
};

enum UDSKeyStatus udsKeyFileLoad(const char* path, uint8_t key[16]);
// The same, from the text of a file (for tests).
enum UDSKeyStatus udsKeyTextLoad(const char* text, size_t length, uint8_t key[16]);
const char* udsKeyStatusText(enum UDSKeyStatus status);
bool udsKeyStatusOk(enum UDSKeyStatus status);

// Writes the finished key as a one-line file (`slot0x2DKeyN=<hex>`, after a comment line). The Settings page uses it so that a file
// that only had KeyX and KeyY is replaced, in the setting, by a file that does not need the key made again.
bool udsKeyFileWrite(const char* path, const uint8_t key[16]);

// The 128-bit arithmetic of the key generator, exposed for the test.
void udsKeyDerive(const uint8_t keyX[16], const uint8_t keyY[16], const uint8_t generator[16], uint8_t out[16]);

/*
 * The four Switch keys GB-Link Switch LDN's side of the board needs, from the user's prod.keys (one `name = hex` line per key, as
 * Switch emulators and dumping tools write it). Read as GB-Link's own web client reads it: names in any case, each value exactly 32
 * hex digits, all four required. Nothing else in the file is kept.
 */
enum UDSProdKey {
	UDS_PROD_KEY_KEK, // aes_kek_generation_source
	UDS_PROD_KEY_GEN, // aes_key_generation_source
	UDS_PROD_KEY_MASTER_00, // master_key_00
	UDS_PROD_KEY_MASTER_12, // master_key_12
	UDS_PROD_KEY_COUNT
};

enum UDSProdKeysStatus {
	UDS_PROD_KEYS_OK,
	UDS_PROD_KEYS_NO_FILE, // empty path or the file cannot be read
	UDS_PROD_KEYS_MISSING, // one of the four is not in the file (the first missing one is reported)
	UDS_PROD_KEYS_BAD_VALUE, // one of the four is not 32 hex digits (the first such one is reported)
};

// The prod.keys name of each key, in enum UDSProdKey order.
const char* udsProdKeyName(enum UDSProdKey key);
// `which` (may be NULL) gets the key a MISSING or BAD_VALUE status is about.
enum UDSProdKeysStatus udsProdKeysLoad(const char* path, uint8_t keys[UDS_PROD_KEY_COUNT][16], enum UDSProdKey* which);
enum UDSProdKeysStatus udsProdKeysTextLoad(const char* text, size_t length, uint8_t keys[UDS_PROD_KEY_COUNT][16],
                                           enum UDSProdKey* which);
// One line for the user, naming the key a MISSING or BAD_VALUE status is about.
void udsProdKeysStatusText(enum UDSProdKeysStatus status, enum UDSProdKey which, char* out, size_t capacity);

CXX_GUARD_END

#endif
