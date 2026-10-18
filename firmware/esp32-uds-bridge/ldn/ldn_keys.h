#pragma once
#include "pia_bin.h"

/* Switch key material and LDN advertisement/authentication crypto. Keys are stored in
   NVS, written once over the control link, and never read back, logged or compiled
   into the firmware image. */

#define LDN_MAX_MEMBERS 8
#define LDN_MAX_APP_DATA 384

typedef struct
{
    uint8_t mac[6];
    char ip[16];
    char name[33];
    uint8_t index;
    uint8_t platform;   /* 0 a Switch, 1 a Switch 2 */
} ldn_member_t;

typedef struct
{
    int protocol, version, channel, security, policy, app_version, maximum;
    uint8_t id[32], ssid[16], random[16], host[6];
    uint64_t communication_id, challenge;
    uint8_t app_data[LDN_MAX_APP_DATA];
    int app_data_len;
    ldn_member_t members[LDN_MAX_MEMBERS];
    int member_count;
} ldn_network_t;

/* Store one named 16-byte key. False if the name is unknown. */
bool ldn_keys_store(const char *name, const uint8_t value[16]);
/* Key presence only. Values are never returned. */
void ldn_keys_status(bool *kek, bool *gen, bool *master00, bool *master12);
bool ldn_keys_supports(int protocol);
void ldn_keys_erase(void);

/* LDN key ladder: master -> kek -> generation -> SHA256(data). */
bool ldn_keys_derive(int protocol, const uint8_t *data, size_t len, bool advertise, uint8_t out[16]);
/* CCMP key for the room's Wi-Fi network. */
bool ldn_keys_network(const ldn_network_t *net, uint8_t out[16]);

/* Parse and authenticate a raw LDN advertisement. False unless it is a valid
   production-security room the stored keys can join. */
bool ldn_decode_advertisement(const uint8_t *raw, size_t len, const uint8_t host[6], int channel,
                              ldn_network_t *out);

/* Room authentication. ldn_auth_begin builds the join request, ldn_auth_accept checks
   the host's response and challenge. One instance per join attempt. */

#define LDN_AUTH_MAX_REQUEST 1024

typedef struct
{
    ldn_network_t net;
    uint8_t random[16], nonce[8], device[8];
    uint8_t request[LDN_AUTH_MAX_REQUEST];
    int request_len;
    bool verified;
} ldn_auth_t;

bool ldn_auth_begin(ldn_auth_t *a, const ldn_network_t *net);
bool ldn_auth_accept(ldn_auth_t *a, const uint8_t *frame, size_t len);
