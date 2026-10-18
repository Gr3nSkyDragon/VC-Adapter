/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version.
 *
 * The keys the board needs, stored on it (NVS), written once by the host (UDS_CMD_SET_KEY) and never sent back: the host can only
 * ask whether one is there (UDS_CMD_KEY_STATUS). They survive reflashing the firmware's app, but not a full erase of the flash (a
 * merged release image counts as one).
 *
 * - The 3DS key the Game Boy wrapper needs, so that a cartridge, which cannot supply one, can trade: the UDS data key, AES key slot
 *   0x2D (KeyN, the "normal" key Azahar reads from aes_keys.txt).
 * - The four Switch keys from the user's prod.keys that GB-Link Switch LDN's side of the merged firmware reads. They are stored
 *   exactly where its ldn_keys.c keeps them (namespace "ldnkeys", entries kek, gen, m00, m12), so that one installer sets up both.
 *   This firmware does not use them itself.
 */
#ifndef UDS_KEYS_H
#define UDS_KEYS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define UDS_KEY_SLOT_DATA 0x2D

/* The Switch keys' slots in SET_KEY and KEY_STATUS (not 3DS key slots: those end at 0x3F). */
#define UDS_KEY_SLOT_SWITCH_KEK 0x80       /* prod.keys aes_kek_generation_source */
#define UDS_KEY_SLOT_SWITCH_GEN 0x81       /* prod.keys aes_key_generation_source */
#define UDS_KEY_SLOT_SWITCH_MASTER_00 0x82 /* prod.keys master_key_00 */
#define UDS_KEY_SLOT_SWITCH_MASTER_12 0x83 /* prod.keys master_key_12 */

esp_err_t keys_init(void);
/* ESP_ERR_NOT_SUPPORTED for a slot this firmware does not keep. */
esp_err_t keys_store(uint8_t slot, const uint8_t key[16]);
bool keys_load(uint8_t slot, uint8_t key[16]);
bool keys_present(uint8_t slot);
/* The 3DS key only: the Switch keys are left alone. */
esp_err_t keys_erase(void);

#endif
