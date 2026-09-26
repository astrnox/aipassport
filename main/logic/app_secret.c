// main/logic/app_secret.c —— 设备绑定密封容器的实现。密码学原语复用 app_crypto。
#include "app_secret.h"

#include "app_crypto.h"

#include <string.h>

#define SECRET_MAGIC_0 'P'
#define SECRET_MAGIC_1 'S'
#define SECRET_MAGIC_2 'E'
#define SECRET_MAGIC_3 'C'
#define SECRET_VERSION 1

// KDF 与密钥派生的域分离标签。改标签等于换一套密钥，因此一旦发布不得随意更改。
static const char KDF_LABEL[] = "passport-app-secret-v1";
static const char ENC_LABEL[] = "enc";
static const char MAC_LABEL[] = "mac";

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void app_secret_device_key(const void *binding, size_t binding_len,
                           const void *seed, size_t seed_len,
                           uint8_t out[APP_SECRET_KEY_LEN])
{
    if (!out) return;

    // 先以固定标签为密钥吸收设备随机种子，再把设备唯一标识作为消息吸收进去。
    // 两步 HMAC 避免为大缓冲拼内存，同时保证两个输入都参与派生。
    uint8_t mid[APP_SHA256_LEN];
    app_hmac_sha256((const uint8_t *)KDF_LABEL, sizeof(KDF_LABEL) - 1,
                    seed, seed_len, mid);
    app_hmac_sha256(mid, sizeof(mid), binding, binding_len, out);
    app_crypto_zero(mid, sizeof(mid));
}

size_t app_secret_seal(const uint8_t key[APP_SECRET_KEY_LEN],
                       const uint8_t iv[APP_SECRET_IV_LEN],
                       const void *plain, size_t plain_len,
                       uint8_t *out, size_t out_cap)
{
    if (!key || !iv || !plain || plain_len == 0 || !out) return 0;
    // 明文长度要用 4 字节字段承载。
    if (plain_len > 0xFFFFFFFFu) return 0;
    if (out_cap < APP_SECRET_HEADER_LEN + APP_SECRET_MAC_LEN + 16) return 0;

    uint8_t k_enc[APP_SECRET_KEY_LEN];
    uint8_t k_mac[APP_SECRET_KEY_LEN];
    app_hmac_sha256(key, APP_SECRET_KEY_LEN, (const uint8_t *)ENC_LABEL,
                    sizeof(ENC_LABEL) - 1, k_enc);
    app_hmac_sha256(key, APP_SECRET_KEY_LEN, (const uint8_t *)MAC_LABEL,
                    sizeof(MAC_LABEL) - 1, k_mac);

    out[0] = SECRET_MAGIC_0;
    out[1] = SECRET_MAGIC_1;
    out[2] = SECRET_MAGIC_2;
    out[3] = SECRET_MAGIC_3;
    out[4] = SECRET_VERSION;
    memcpy(out + 5, iv, APP_SECRET_IV_LEN);
    put_le32(out + 21, (uint32_t)plain_len);

    size_t ct_cap = out_cap - APP_SECRET_HEADER_LEN - APP_SECRET_MAC_LEN;
    size_t ct_len = app_aes256_cbc_encrypt(k_enc, iv, plain, plain_len,
                                           out + APP_SECRET_HEADER_LEN, ct_cap);
    app_crypto_zero(k_enc, sizeof(k_enc));
    if (ct_len == 0) {
        app_crypto_zero(k_mac, sizeof(k_mac));
        return 0;
    }

    size_t body = APP_SECRET_HEADER_LEN + ct_len;
    app_hmac_sha256(k_mac, APP_SECRET_KEY_LEN, out, body, out + body);
    app_crypto_zero(k_mac, sizeof(k_mac));
    return body + APP_SECRET_MAC_LEN;
}

bool app_secret_open(const uint8_t key[APP_SECRET_KEY_LEN],
                     const void *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!key || !in || !out || in_len < APP_SECRET_HEADER_LEN + 16 + APP_SECRET_MAC_LEN) {
        return false;
    }

    const uint8_t *buf = (const uint8_t *)in;
    if (buf[0] != SECRET_MAGIC_0 || buf[1] != SECRET_MAGIC_1 ||
        buf[2] != SECRET_MAGIC_2 || buf[3] != SECRET_MAGIC_3 ||
        buf[4] != SECRET_VERSION) {
        return false;
    }

    uint32_t plain_len = get_le32(buf + 21);
    size_t ct_len = in_len - APP_SECRET_HEADER_LEN - APP_SECRET_MAC_LEN;
    if (ct_len == 0 || (ct_len % APP_AES_BLOCK_LEN) != 0) return false;
    // 明文与密文的长度关系必须自洽：PKCS#7 只会多补一个整块。
    if (ct_len < (size_t)plain_len || ct_len > (size_t)plain_len + APP_AES_BLOCK_LEN) {
        return false;
    }

    size_t body = APP_SECRET_HEADER_LEN + ct_len;
    uint8_t k_mac[APP_SECRET_KEY_LEN];
    uint8_t k_enc[APP_SECRET_KEY_LEN];
    uint8_t expect[APP_SECRET_MAC_LEN];

    // 先验签再解密：MAC 覆盖头部与密文，任何一位被改动都会在这里被拦下。
    app_hmac_sha256(key, APP_SECRET_KEY_LEN, (const uint8_t *)MAC_LABEL,
                    sizeof(MAC_LABEL) - 1, k_mac);
    app_hmac_sha256(k_mac, APP_SECRET_KEY_LEN, buf, body, expect);
    bool mac_ok = app_crypto_equal(expect, buf + body, APP_SECRET_MAC_LEN);
    app_crypto_zero(k_mac, sizeof(k_mac));
    app_crypto_zero(expect, sizeof(expect));
    if (!mac_ok) return false;

    app_hmac_sha256(key, APP_SECRET_KEY_LEN, (const uint8_t *)ENC_LABEL,
                    sizeof(ENC_LABEL) - 1, k_enc);
    size_t got = 0;
    bool ok = app_aes256_cbc_decrypt(k_enc, buf + 5, buf + APP_SECRET_HEADER_LEN,
                                     ct_len, out, out_cap, &got);
    app_crypto_zero(k_enc, sizeof(k_enc));
    if (!ok || got != (size_t)plain_len) return false;

    if (out_len) *out_len = got;
    return true;
}
