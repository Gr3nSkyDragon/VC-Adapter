#include "pia_crypto.h"
#include "sdkconfig.h"

#include <stdio.h>
#include "psa/crypto.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

static const uint8_t kSessionKey[16] = {
    0x83, 0xca, 0x7f, 0xab, 0x73, 0x4c, 0x34, 0x63, 0x3b, 0x10, 0x18, 0x35, 0x26, 0xc1, 0xe8, 0x5b};
static const uint8_t kMagic[4] = {0x32, 0xab, 0x98, 0x64};

uint32_t bin_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(0u - (crc & 1u)));
    }
    return ~crc;
}

/* mbedtls 4 exposes only the PSA API (mbedtls/aes.h is private). PSA's shortened-tag
   GCM gives Pia's 8-byte tag directly. */
static void psa_ready(void)
{
    static bool done;
    if (!done) { psa_crypto_init(); done = true; }
}

static psa_status_t import_aes(const uint8_t *key, size_t key_len, psa_algorithm_t alg,
                               psa_key_usage_t usage, mbedtls_svc_key_id_t *out)
{
    psa_ready();
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, alg);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, (size_t)key_len * 8);
    return psa_import_key(&attr, key, key_len, out);
}

void pia_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16], bool encrypt)
{
    mbedtls_svc_key_id_t k;
    if (import_aes(key, 16, PSA_ALG_ECB_NO_PADDING,
                   PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT, &k) != PSA_SUCCESS)
    {
        memset(out, 0, 16);
        return;
    }
    size_t written = 0;
    if (encrypt) psa_cipher_encrypt(k, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &written);
    else psa_cipher_decrypt(k, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &written);
    if (written != 16) memset(out, 0, 16);
    psa_destroy_key(k);
}

static bool aead(psa_algorithm_t base, const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t nonce_len,
                 const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t in_len,
                 uint8_t *out, uint8_t *tag, size_t tag_len, bool encrypt)
{
    psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(base, tag_len);
    mbedtls_svc_key_id_t k;
    if (import_aes(key, key_len, alg, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT, &k) != PSA_SUCCESS)
        return false;

    /* PSA outputs ciphertext then tag; Pia puts the tag in its header. */
    static uint8_t joined[PIA_MAX_DATAGRAM + 16];
    bool ok = false;
    size_t written = 0;
    if (in_len + tag_len <= sizeof(joined))
    {
        if (encrypt)
        {
            ok = psa_aead_encrypt(k, alg, nonce, nonce_len, aad, aad_len, in, in_len,
                                  joined, sizeof(joined), &written) == PSA_SUCCESS &&
                 written == in_len + tag_len;
            if (ok) { memcpy(out, joined, in_len); memcpy(tag, joined + in_len, tag_len); }
        }
        else
        {
            memcpy(joined, in, in_len);
            memcpy(joined + in_len, tag, tag_len);
            ok = psa_aead_decrypt(k, alg, nonce, nonce_len, aad, aad_len, joined, in_len + tag_len,
                                  out, in_len, &written) == PSA_SUCCESS && written == in_len;
        }
    }
    psa_destroy_key(k);
    return ok;
}

bool pia_gcm(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t nonce_len,
             const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t in_len,
             uint8_t *out, uint8_t *tag, size_t tag_len, bool encrypt)
{
    return aead(PSA_ALG_GCM, key, key_len, nonce, nonce_len, aad, aad_len, in, in_len, out, tag, tag_len, encrypt);
}

void pia_ctr(const uint8_t key[16], const uint8_t nonce4[4], const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t counter[16] = {0}, mask[16];
    memcpy(counter, nonce4, 4);
    for (size_t i = 0; i < len; i += 16)
    {
        pia_ecb(key, counter, mask, true);
        size_t n = len - i < 16 ? len - i : 16;
        for (size_t j = 0; j < n; ++j) out[i + j] = (uint8_t)(in[i + j] ^ mask[j]);
        for (int j = 15; j >= 4 && ++counter[j] == 0; --j) { }
    }
}

void pia_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    psa_ready();
    size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &written) != PSA_SUCCESS || written != 32)
        memset(out, 0, 32);
}

void pia_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32])
{
    psa_ready();
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, (size_t)key_len * 8);
    mbedtls_svc_key_id_t k;
    if (psa_import_key(&attr, key, key_len, &k) != PSA_SUCCESS) { memset(out, 0, 32); return; }
    size_t written = 0;
    if (psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, len, out, 32, &written) != PSA_SUCCESS)
        memset(out, 0, 32);
    psa_destroy_key(k);
}

void pia_crypto_init(pia_crypto_t *c, const uint8_t ssid[16])
{
    pia_ecb(kSessionKey, ssid, c->key, true);
    c->net_id = bin_crc32(ssid + 1, 15);
}

static bool parse_ip(const char *text, uint8_t out[4])
{
    unsigned a, b, cc, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return false;
    if (a > 255 || b > 255 || cc > 255 || d > 255) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)cc; out[3] = (uint8_t)d;
    return true;
}

/* Nonce = network id XOR sender IP, plus seed. A datagram only authenticates against
   its claimed source. */
static bool build_nonce(const pia_crypto_t *c, const char *ip, const uint8_t seed[8], uint8_t out[12])
{
    uint8_t addr[4];
    if (!parse_ip(ip, addr)) return false;
    bin_wb32(out, c->net_id ^ bin_b32(addr));
    memcpy(out + 4, seed, 8);
    return true;
}

int pia_decrypt(const pia_crypto_t *c, const uint8_t *datagram, size_t length,
                const char *source_ip, uint8_t *out, size_t out_cap)
{
    if (length < 29 || memcmp(datagram, kMagic, 4) != 0) return -1;
    size_t body = length - 29;
    if (body > out_cap) return -1;
    uint8_t nonce[12];
    if (!build_nonce(c, source_ip, datagram + 13, nonce)) return -1;
    /* Wire order: header, tag, ciphertext. */
    if (!pia_gcm(c->key, 16, nonce, 12, NULL, 0, datagram + 29, body, out,
                 (uint8_t *)(datagram + 21), 8, false))
        return -1;
    return (int)body;
}

int pia_encrypt(const pia_crypto_t *c, const uint8_t *body, size_t body_len, const char *source_ip,
                uint16_t dst, uint16_t src, uint16_t packet, uint64_t nonce, uint8_t flags,
                uint8_t footer, uint8_t *out, size_t out_cap)
{
    if (body_len + 29 > out_cap) return -1;
    uint8_t *h = out;
    memcpy(h, kMagic, 4);
    h[4] = 0x90; h[5] = flags;
    bin_wb16(h + 6, dst); bin_wb16(h + 8, src); bin_wb16(h + 10, packet);
    h[12] = footer;
    bin_wb64(h + 13, nonce);
    uint8_t iv[12];
    if (!build_nonce(c, source_ip, h + 13, iv)) return -1;
    if (!pia_gcm(c->key, 16, iv, 12, NULL, 0, body, body_len, out + 29, out + 21, 8, true)) return -1;
    return (int)(29 + body_len);
}

static ZSTD_DCtx *s_dctx;

/* The context needs 38,464 contiguous bytes with this zstd build. The heap fragments at
   start-up; without a context every compressed Switch datagram is undecodable and the
   room looks silent. Static where the chip's data region allows, else allocated from
   the heap at the start of boot. */
#if CONFIG_PIA_STATIC_ZSTD_CONTEXT
static uint64_t s_dctx_space[40960 / sizeof(uint64_t)];
#endif

size_t pia_crypto_dctx_size(void) { return ZSTD_estimateDCtxSize(); }

bool pia_crypto_prepare(void)
{
#if CONFIG_PIA_STATIC_ZSTD_CONTEXT
    if (!s_dctx) s_dctx = ZSTD_initStaticDCtx(s_dctx_space, sizeof(s_dctx_space));
#endif
    if (!s_dctx) s_dctx = ZSTD_createDCtx();
    return s_dctx != NULL;
}

int pia_decompress(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
    if (len < 4 || bin_u32(in) != 0xfd2fb528u)
    {
        if (len > out_cap) return -1;
        memcpy(out, in, len);
        return (int)len;
    }
    if (!s_dctx && !pia_crypto_prepare()) return -1;
    /* Pia may append footer bytes; decode only the frame. */
    size_t frame = ZSTD_findFrameCompressedSize(in, len);
    if (ZSTD_isError(frame)) return -(1000 + (int)ZSTD_getErrorCode(frame));
    size_t written = ZSTD_decompressDCtx(s_dctx, out, out_cap, in, frame);
    if (ZSTD_isError(written)) return -(1000 + (int)ZSTD_getErrorCode(written));
    return (int)written;
}

int pia_compress_raw(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap)
{
    /* Magic, frame header 0x00 (no dictionary, size or checksum), window descriptor 0x18
       (canonical form the host uses), then one raw final block. */
    if (len > 0x1FFFF || out_cap < len + 9) return -1;
    static const uint8_t prefix[6] = {0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x18};
    memcpy(out, prefix, sizeof(prefix));
    uint32_t block = (uint32_t)(len << 3) | 1u;   /* last block, type raw */
    out[6] = (uint8_t)block; out[7] = (uint8_t)(block >> 8); out[8] = (uint8_t)(block >> 16);
    memcpy(out + 9, in, len);
    return (int)(9 + len);
}

int pia_message_encode(const pia_message_t *m, uint8_t *out, size_t out_cap)
{
    size_t need = (size_t)(m->has_flags ? 5 : 4) + m->length;
    if (need > out_cap) return -1;
    size_t o = 0;
    out[o++] = (uint8_t)(m->has_flags ? 7 : 6);
    if (m->has_flags) out[o++] = m->flags;
    bin_wb16(out + o, m->length); o += 2;
    out[o++] = m->protocol;
    memcpy(out + o, m->payload, m->length);
    return (int)(o + m->length);
}

void pia_message_iter_init(pia_message_iter_t *it, const uint8_t *data, size_t len)
{
    it->data = data;
    it->len = len;
    it->pos = 0;
    it->size = -1;
    it->proto = -1;
    it->flags = 0;
}

/* A message header carries only the fields that differ from the previous message. */
bool pia_message_next(pia_message_iter_t *it, pia_message_t *out)
{
    const uint8_t *data = it->data;
    size_t len = it->len, i = it->pos;
    if (i >= len) return false;
    uint8_t bits = data[i++];
    if ((bits & 0xf0) != 0 || (bits == 0 && it->size < 0)) return false;
    if (bits & 1) { if (i >= len) return false; it->flags = data[i++]; }
    if (bits & 2) { if (i + 2 > len) return false; it->size = bin_b16(data + i); i += 2; }
    if (bits & 4) { if (i >= len) return false; it->proto = data[i++]; }
    if (bits & 8) { if (i >= len) return false; ++i; }
    if (it->size < 0 || it->proto < 0 || i + (size_t)it->size > len) return false;
    out->protocol = (uint8_t)it->proto;
    out->flags = it->flags;
    out->has_flags = true;
    out->payload = data + i;
    out->length = (uint16_t)it->size;
    it->pos = i + (size_t)it->size;
    return true;
}

int pia_messages_decode(const uint8_t *data, size_t len, pia_message_t *out, int max)
{
    pia_message_iter_t it;
    pia_message_iter_init(&it, data, len);
    int count = 0;
    while (count < max && pia_message_next(&it, &out[count])) ++count;
    return count;
}
