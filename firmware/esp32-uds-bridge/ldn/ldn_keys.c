#include "ldn_keys.h"
#include "pia_crypto.h"

#include <stdio.h>

#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#define KEY_NAMESPACE "ldnkeys"

static const char *const kNames[] = {
    "aes_kek_generation_source", "aes_key_generation_source", "master_key_00", "master_key_12"};
/* NVS key names are limited to 15 characters. */
static const char *const kSlots[] = {"kek", "gen", "m00", "m12"};
#define KEY_COUNT (sizeof(kSlots) / sizeof(kSlots[0]))

static int slot_of(const char *name)
{
    for (size_t i = 0; i < KEY_COUNT; ++i) if (!strcmp(name, kNames[i])) return (int)i;
    return -1;
}

static bool load_key(int slot, uint8_t out[16])
{
    nvs_handle_t h;
    if (nvs_open(KEY_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t length = 16;
    bool ok = nvs_get_blob(h, kSlots[slot], out, &length) == ESP_OK && length == 16;
    nvs_close(h);
    return ok;
}

static bool have_key(int slot)
{
    uint8_t scratch[16];
    bool ok = load_key(slot, scratch);
    memset(scratch, 0, sizeof(scratch));
    return ok;
}

bool ldn_keys_store(const char *name, const uint8_t value[16])
{
    int slot = slot_of(name);
    if (slot < 0) return false;
    nvs_handle_t h;
    if (nvs_open(KEY_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, kSlots[slot], value, 16) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void ldn_keys_status(bool *kek, bool *gen, bool *master00, bool *master12)
{
    if (kek) *kek = have_key(0);
    if (gen) *gen = have_key(1);
    if (master00) *master00 = have_key(2);
    if (master12) *master12 = have_key(3);
}

bool ldn_keys_supports(int protocol)
{
    return have_key(0) && have_key(1) && have_key(protocol == 1 ? 2 : 3);
}

void ldn_keys_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(KEY_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static const uint8_t kAdvertiseSeed[16] = {
    0x19, 0x18, 0x84, 0x74, 0x3e, 0x24, 0xc7, 0x7d, 0x87, 0xc6, 0x9e, 0x42, 0x07, 0xd0, 0xc4, 0x38};
static const uint8_t kSessionSeed[16] = {
    0xf1, 0xe7, 0x01, 0x84, 0x19, 0xa8, 0x4f, 0x71, 0x1d, 0xa7, 0x14, 0xc2, 0xcf, 0x91, 0x9c, 0x9c};

bool ldn_keys_derive(int protocol, const uint8_t *data, size_t len, bool advertise, uint8_t out[16])
{
    uint8_t key[16], kek[16], gen[16];
    if (!load_key(protocol == 1 ? 2 : 3, key) || !load_key(0, kek) || !load_key(1, gen)) return false;

    /* Each step unwraps the previous key with the next source, ending at SHA256(data). */
    uint8_t stage[16];
    pia_ecb(key, kek, stage, false);
    pia_ecb(stage, advertise ? kAdvertiseSeed : kSessionSeed, key, false);
    pia_ecb(key, gen, stage, false);

    uint8_t digest[32];
    pia_sha256(data, len, digest);
    pia_ecb(stage, digest, out, false);

    memset(key, 0, sizeof(key)); memset(kek, 0, sizeof(kek));
    memset(gen, 0, sizeof(gen)); memset(stage, 0, sizeof(stage));
    return true;
}

static const uint8_t kNetworkSeed[64] = {
    0xfc, 0xb6, 0xf6, 0xad, 0xb9, 0xdf, 0xea, 0x66, 0xac, 0xa9, 0xc3, 0x26, 0x14, 0x9d, 0x2b, 0x3b,
    0x08, 0xa7, 0x81, 0x89, 0x5c, 0xbf, 0x78, 0xf7, 0x20, 0xd7, 0x8b, 0x85, 0xa5, 0x75, 0x84, 0xa9,
    0x96, 0x65, 0xd2, 0x37, 0x79, 0x7b, 0x2a, 0x41, 0xdd, 0xef, 0x14, 0x06, 0x3e, 0xc2, 0x8d, 0x25,
    0x91, 0x43, 0xaf, 0x78, 0x32, 0xfb, 0x3c, 0xbc, 0xf2, 0x75, 0x9c, 0xbf, 0xbd, 0xc8, 0x1d, 0x8c};

bool ldn_keys_network(const ldn_network_t *net, uint8_t out[16])
{
    uint8_t material[16 + sizeof(kNetworkSeed)];
    memcpy(material, net->random, 16);
    memcpy(material + 16, kNetworkSeed, sizeof(kNetworkSeed));
    return ldn_keys_derive(net->protocol, material, sizeof(material), false, out);
}

static void little_id(const ldn_network_t *net, uint8_t out[32])
{
    memcpy(out, net->id, 32);
    bin_w64(out, net->communication_id);
    bin_w16(out + 10, bin_b16(net->id + 10));
}

static const uint8_t kHeaderMagic[8] = {0x7f, 0x00, 0x22, 0xaa, 0x04, 0x00, 0x01, 0x01};

bool ldn_decode_advertisement(const uint8_t *raw, size_t len, const uint8_t host[6], int channel,
                              ldn_network_t *out)
{
    if (len < 68 || len > 1536 || memcmp(raw, kHeaderMagic, 8) != 0) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->host, host, 6);
    out->channel = channel;
    memcpy(out->id, raw + 12, 32);
    out->version = raw[44];
    if (out->version < 2 || out->version > 4) return false;
    if (raw[45] == 2) out->protocol = 1;
    else if (raw[45] == 3) out->protocol = 3;
    else return false;                       /* unauthenticated advertisement */
    out->communication_id = bin_b64(out->id);
    memcpy(out->ssid, out->id + 16, 16);
    if (!ldn_keys_supports(out->protocol)) return false;

    uint8_t key[16];
    if (!ldn_keys_derive(out->protocol, out->id, 32, true, key)) return false;

    static uint8_t data[1536];
    int size = bin_b16(raw + 46), length;

    if (out->protocol == 3)
    {
        if (len != (size_t)(68 + size)) return false;
        uint8_t nonce[12] = {0};
        memcpy(nonce, raw + 48, 4);
        size_t body = len - 68;
        if (body > sizeof(data)) return false;
        /* Wire layout: 16-byte tag at 52, ciphertext at 68. */
        if (!pia_gcm(key, 16, nonce, 12, raw + 12, 40, raw + 68, body, data,
                     (uint8_t *)(raw + 52), 16, false))
            return false;
        length = (int)body;
        if (length < 42) return false;
        memcpy(out->random, data, 16);
        out->challenge = bin_b64(data + 16);
        out->security = data[24];
        out->policy = data[25];
        out->app_version = bin_b16(data + 26);
        int encoded = bin_b16(data + 36) & 1023;
        if (encoded != 0 && encoded != channel) return false;
        out->maximum = data[38];
        int count = data[39];
        if (count > LDN_MAX_MEMBERS || out->maximum > LDN_MAX_MEMBERS || count > out->maximum ||
            length < 42 + 48 * count)
            return false;
        for (int i = 0; i < count; ++i)
        {
            int o = 40 + 48 * i, index = data[o + 10];
            if (index > 7) return false;
            for (int j = 0; j < out->member_count; ++j) if (out->members[j].index == index) return false;
            ldn_member_t *m = &out->members[out->member_count++];
            memcpy(m->mac, data + o + 4, 6);
            snprintf(m->ip, sizeof(m->ip), "%u.%u.%u.%u", data[o], data[o + 1], data[o + 2], data[o + 3]);
            memcpy(m->name, data + o + 12, 32);
            m->name[32] = 0;
            m->index = (uint8_t)index;
            m->platform = data[o + 11];
        }
        int app_at = 40 + 48 * count, app_size = bin_b16(data + app_at);
        if (app_size > LDN_MAX_APP_DATA || app_at + 2 + app_size != length) return false;
        memcpy(out->app_data, data + app_at + 2, (size_t)app_size);
        out->app_data_len = app_size;
    }
    else
    {
        if (size != 1280 || len != (size_t)(52 + 32 + size)) return false;
        static uint8_t plain[1312];
        pia_ctr(key, raw + 48, raw + 52, len - 52, plain);
        uint8_t digest[32], material[1312];
        memcpy(material, raw + 12, 40);
        memset(material + 40, 0, 32);
        memcpy(material + 72, plain + 32, len - 52 - 32);
        pia_sha256(material, 72 + (len - 52 - 32), digest);
        if (memcmp(plain, digest, 32) != 0) return false;
        memcpy(data, plain + 32, len - 52 - 32);
        length = (int)(len - 52 - 32);
        memcpy(out->random, data, 16);
        out->security = bin_b16(data + 16);
        out->policy = data[18];
        out->maximum = data[22];
        out->app_version = bin_b16(data + 68);
        for (int i = 0; i < 8; ++i)
        {
            int o = 24 + i * 56;
            if (data[o + 10] == 0) continue;
            ldn_member_t *m = &out->members[out->member_count++];
            memcpy(m->mac, data + o + 4, 6);
            snprintf(m->ip, sizeof(m->ip), "%u.%u.%u.%u", data[o], data[o + 1], data[o + 2], data[o + 3]);
            memcpy(m->name, data + o + 12, 32);
            m->name[32] = 0;
            m->index = (uint8_t)i;
        }
        int app_size = bin_b16(data + 474);
        if (app_size > LDN_MAX_APP_DATA) return false;
        memcpy(out->app_data, data + 476, (size_t)app_size);
        out->app_data_len = app_size;
        out->challenge = bin_b64(data + 1272);
    }

    memset(key, 0, sizeof(key));
    return out->security == 1;   /* production security only */
}

static const uint8_t kChallengeKey[32] = {
    0xf8, 0x4b, 0x48, 0x7f, 0xb3, 0x72, 0x51, 0xc2, 0x63, 0xbf, 0x11, 0x60, 0x90, 0x36, 0x58, 0x92,
    0x66, 0xaf, 0x70, 0xca, 0x79, 0xb4, 0x4c, 0x93, 0xc7, 0x37, 0x0c, 0x57, 0x69, 0xc0, 0xf6, 0x02};
static const uint8_t kAuthMagic[6] = {0x00, 0x22, 0xaa, 0x01, 0x02, 0x00};

bool ldn_auth_begin(ldn_auth_t *a, const ldn_network_t *net)
{
    memset(a, 0, sizeof(*a));
    a->net = *net;
    esp_fill_random(a->random, sizeof(a->random));
    esp_fill_random(a->nonce, sizeof(a->nonce));
    esp_fill_random(a->device, sizeof(a->device));

    static uint8_t payload[868];
    const int payload_len = net->version >= 3 ? 868 : 64;
    memset(payload, 0, (size_t)payload_len);
    memcpy(payload, "GBLINK", 6);
    bin_wb16(payload + 32, (uint16_t)net->app_version);
    if (net->version >= 3)
    {
        static uint8_t body[720];
        memset(body, 0, sizeof(body));
        bin_w64(body + 8, net->challenge);
        memcpy(body + 16, a->nonce, 8);
        memcpy(body + 24, a->device, 8);
        pia_hmac_sha256(kChallengeKey, sizeof(kChallengeKey), body, sizeof(body), payload + 104);
        memcpy(payload + 148, body, sizeof(body));
    }

    uint8_t header[72];
    memset(header, 0, sizeof(header));
    header[0] = (uint8_t)net->version;
    header[1] = (uint8_t)payload_len;
    header[4] = (uint8_t)(payload_len >> 8);
    header[5] = (uint8_t)(net->protocol == 3 ? 1 : 0);
    little_id(net, header + 8);
    memcpy(header + 40, net->random, 16);
    memcpy(header + 56, a->random, 16);

    int total = 6 + 72 + payload_len + (net->protocol == 3 ? 16 : 0);
    if (total > LDN_AUTH_MAX_REQUEST) return false;
    memcpy(a->request, kAuthMagic, 6);
    memcpy(a->request + 6, header, 72);

    if (net->protocol == 3)
    {
        uint8_t key[16];
        if (!ldn_keys_derive(net->protocol, a->random, 16, false, key)) return false;
        static uint8_t cipher[868];
        uint8_t tag[16];
        bool ok = pia_gcm(key, 16, header, 12, header, 72, payload, (size_t)payload_len,
                          cipher, tag, 16, true);
        memset(key, 0, sizeof(key));
        if (!ok) return false;
        /* Tag precedes the ciphertext on the wire. */
        memcpy(a->request + 78, tag, 16);
        memcpy(a->request + 94, cipher, (size_t)payload_len);
    }
    else memcpy(a->request + 78, payload, (size_t)payload_len);

    a->request_len = total;
    return true;
}

bool ldn_auth_accept(ldn_auth_t *a, const uint8_t *frame, size_t len)
{
    if (len < 78 || memcmp(frame, kAuthMagic, 6) != 0) return false;
    const uint8_t *header = frame + 6;
    uint8_t expected[32];
    little_id(&a->net, expected);
    if (header[0] != a->net.version || header[3] != 1 ||
        header[5] != (a->net.protocol == 3 ? 1 : 0) ||
        memcmp(header + 8, expected, 32) != 0 ||
        memcmp(header + 40, a->net.random, 16) != 0 ||
        memcmp(header + 56, a->random, 16) != 0)
        return false;

    int size = header[1] | header[4] << 8;
    if (len != (size_t)(78 + size + (a->net.protocol == 3 ? 16 : 0))) return false;
    if (header[2] != 0) return false;      /* rejected by the host */

    static uint8_t payload[868];
    if ((size_t)size > sizeof(payload)) return false;
    if (a->net.protocol == 3)
    {
        uint8_t key[16];
        if (!ldn_keys_derive(a->net.protocol, a->random, 16, false, key)) return false;
        bool ok = pia_gcm(key, 16, header, 12, header, 72, frame + 94, (size_t)size,
                          payload, (uint8_t *)(frame + 78), 16, false);
        memset(key, 0, sizeof(key));
        if (!ok) return false;
    }
    else memcpy(payload, frame + 78, (size_t)size);

    if (a->net.version >= 3)
    {
        if (a->verified && size == 132) return true;
        if (size != 388) return false;
        const uint8_t *challenge = payload + 132;
        uint8_t digest[32];
        pia_hmac_sha256(kChallengeKey, sizeof(kChallengeKey), challenge + 48, (size_t)(size - 132 - 48), digest);
        if (memcmp(challenge + 4, digest, 32) != 0) return false;
        if (memcmp(challenge + 56, a->nonce, 8) != 0 || memcmp(challenge + 64, a->device, 8) != 0)
            return false;
    }
    a->verified = true;
    return true;
}
