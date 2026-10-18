#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ldn_keys.h"

/* LDN network host: SoftAP, advertisement, authentication and membership, the counterpart
   of the joiner in ldn_probe.c. The encode/parse/build functions have no radio dependency
   and are tested against the joiner's code on a PC. */

typedef struct
{
    uint8_t mac[6];
    char ip[16];
    uint8_t index;
    char name[33];
    bool authenticated;
    uint8_t platform;
} ldn_host_member_t;

typedef struct
{
    uint8_t channel;              /* 1, 6 or 11 */
    uint64_t communication_id;
    uint16_t scene_id;
    uint16_t app_version;
    uint8_t maximum;              /* room capacity, the host included */
    char host_name[33];
    uint8_t app_data[LDN_MAX_APP_DATA];
    int app_data_len;
} ldn_host_config_t;

/* Start the AP and advertising; false with an LDN_HOST_ERROR line. The joiner must be
   stopped first (ldn_session_stop). `cfg` must stay valid while the host runs: its
   application data is read in place. */
/* Work memory used only while hosting, provided by the caller so it can share space with
   the joiner session. */
#define LDN_HOST_WORK 1024
#define LDN_HOST_ADV_MAX 1024
typedef struct
{
    uint8_t adv[LDN_HOST_ADV_MAX];
    uint8_t work[LDN_HOST_WORK];
    uint8_t response[512];
    uint8_t frame[24 + LDN_HOST_ADV_MAX];
    ldn_network_t net;
} ldn_host_scratch_t;

bool ldn_host_start(const ldn_host_config_t *cfg, ldn_host_scratch_t *scratch);
void ldn_host_stop(void);
bool ldn_host_running(void);
/* Call about every 1 ms: advertisements, auth frames, membership, timeouts. */
void ldn_host_poll(void);
/* Set the advertised application data; read in place, must stay valid while the host
   runs. */
void ldn_host_set_app_data(const uint8_t *data, int length);
int ldn_host_member_count(void);                /* stations, not counting the host */
const ldn_host_member_t *ldn_host_member(int i);
void ldn_host_set_handlers(void (*joined)(const ldn_host_member_t *),
                           void (*left)(const ldn_host_member_t *));
const char *ldn_host_ip(void);                  /* "169.254.S.1" */
void ldn_host_mac(uint8_t out[6]);
const uint8_t *ldn_host_ssid(void);             /* 16 bytes */

/* Protocol core, no radio; used by the PC test. */

typedef struct
{
    char name[33];
    int app_version;
    uint8_t platform;
    uint8_t joiner_random[16];
    uint8_t nonce[8];
    uint8_t device[8];
    uint8_t extra[16];            /* sent by a Switch 2, echoed in the response */
} ldn_host_request_t;

/* Encode an advertisement (members[0] is the host). `nonce4` changes with each change of
   the advertisement. Returns its length or -1. */
int ldn_host_encode_advertisement(const ldn_network_t *net, const uint8_t nonce4[4],
                                  uint8_t *out, size_t cap, uint8_t work[LDN_HOST_WORK]);
/* Parse and verify an authentication request (as ldn_auth_begin builds it): header, HMAC
   and challenge. */
bool ldn_host_parse_request(const ldn_network_t *net, const uint8_t *frame, size_t len,
                            ldn_host_request_t *out, uint8_t work[LDN_HOST_WORK]);
/* Build the response ldn_auth_accept expects: the host's platform byte and the challenge
   reply bound to the request's nonce and device, naming the host's device. `followup`
   builds the 132-byte reply instead of the 388-byte one. Returns its length or -1. */
int ldn_host_build_response(const ldn_network_t *net, const ldn_host_request_t *req,
                            const uint8_t host_device[8], bool followup, uint8_t *out, size_t cap,
                            uint8_t work[LDN_HOST_WORK]);
