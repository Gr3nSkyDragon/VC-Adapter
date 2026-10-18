/* Copyright 2026 Gr3nSkyDragon. Licensed under GPLv3 or any later version. */
#include "keys.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define KEY_NAMESPACE "uds_keys"

/* Where each slot lives. The Switch keys use GB-Link Switch LDN's own namespace and names (ldn_keys.c). */
static const struct {
    uint8_t slot;
    const char *space;
    const char *name;
} kSlots[] = {
    {UDS_KEY_SLOT_DATA, KEY_NAMESPACE, "slot2d"},
    {UDS_KEY_SLOT_SWITCH_KEK, "ldnkeys", "kek"},
    {UDS_KEY_SLOT_SWITCH_GEN, "ldnkeys", "gen"},
    {UDS_KEY_SLOT_SWITCH_MASTER_00, "ldnkeys", "m00"},
    {UDS_KEY_SLOT_SWITCH_MASTER_12, "ldnkeys", "m12"},
};

static bool slot_place(uint8_t slot, const char **space, const char **name)
{
    for (size_t i = 0; i < sizeof(kSlots) / sizeof(kSlots[0]); ++i) {
        if (kSlots[i].slot == slot) {
            *space = kSlots[i].space;
            *name = kSlots[i].name;
            return true;
        }
    }
    return false;
}

esp_err_t keys_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition cannot be used as it is (full, or written by another NVS version): it has to be erased, keys included. */
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t keys_store(uint8_t slot, const uint8_t key[16])
{
    const char *space, *name;
    if (!slot_place(slot, &space, &name)) return ESP_ERR_NOT_SUPPORTED;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(space, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, name, key, 16);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool keys_load(uint8_t slot, uint8_t key[16])
{
    const char *space, *name;
    nvs_handle_t handle;
    if (!slot_place(slot, &space, &name) || nvs_open(space, NVS_READONLY, &handle) != ESP_OK) {
        memset(key, 0, 16);
        return false;
    }
    size_t length = 16;
    bool ok = nvs_get_blob(handle, name, key, &length) == ESP_OK && length == 16;
    nvs_close(handle);
    if (!ok) memset(key, 0, 16);
    return ok;
}

bool keys_present(uint8_t slot)
{
    uint8_t key[16];
    bool present = keys_load(slot, key);
    memset(key, 0, sizeof(key));
    return present;
}

esp_err_t keys_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KEY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
