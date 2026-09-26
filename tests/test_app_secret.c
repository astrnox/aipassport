// tests/test_app_secret.c —— 设备绑定密封容器的主机侧单元测试。
//
// 覆盖：KDF 的确定性与设备绑定、封/解往返、错密钥与逐位篡改（头部/密文/MAC）都被拒、
// 截断与容量不足的边界。密码学原语由 app_crypto 提供，这里只验证容器的组合与校验逻辑。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_crypto.h"
#include "logic/app_secret.h"

static void fill(uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i);
}

static void test_device_key(void)
{
    uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0x01, 0x02, 0x03 };
    uint8_t seed[32];
    fill(seed, sizeof(seed), 1);

    uint8_t a[APP_SECRET_KEY_LEN];
    uint8_t b[APP_SECRET_KEY_LEN];
    uint8_t c[APP_SECRET_KEY_LEN];

    // 同一输入必须得到同一密钥，否则重启后就解不开自己写的密文。
    app_secret_device_key(mac, sizeof(mac), seed, sizeof(seed), a);
    app_secret_device_key(mac, sizeof(mac), seed, sizeof(seed), b);
    assert(memcmp(a, b, APP_SECRET_KEY_LEN) == 0);

    // 换设备（MAC 变一位）或换种子，密钥都必须改变。
    uint8_t mac2[6];
    memcpy(mac2, mac, sizeof(mac2));
    mac2[5] ^= 0x01;
    app_secret_device_key(mac2, sizeof(mac2), seed, sizeof(seed), c);
    assert(memcmp(a, c, APP_SECRET_KEY_LEN) != 0);

    uint8_t seed2[32];
    fill(seed2, sizeof(seed2), 9);
    app_secret_device_key(mac, sizeof(mac), seed2, sizeof(seed2), c);
    assert(memcmp(a, c, APP_SECRET_KEY_LEN) != 0);
}

static void test_roundtrip(void)
{
    uint8_t key[APP_SECRET_KEY_LEN];
    uint8_t iv[APP_SECRET_IV_LEN];
    fill(key, sizeof(key), 7);
    fill(iv, sizeof(iv), 0x40);

    const char *plain = "JBSWY3DPEHPK3PXP";   // 16 字节，正好整块，PKCS#7 会补一整块
    size_t plen = strlen(plain);

    uint8_t blob[APP_SECRET_BLOB_MAX(64)];
    size_t n = app_secret_seal(key, iv, plain, plen, blob, sizeof(blob));
    assert(n == APP_SECRET_HEADER_LEN + 32 + APP_SECRET_MAC_LEN);

    uint8_t out[64];
    size_t out_len = 0;
    assert(app_secret_open(key, blob, n, out, sizeof(out), &out_len));
    assert(out_len == plen);
    assert(memcmp(out, plain, plen) == 0);

    // 解密后再用同一 IV 封一次，密文应当逐字节一致（CBC 确定性）。
    uint8_t blob2[APP_SECRET_BLOB_MAX(64)];
    size_t n2 = app_secret_seal(key, iv, plain, plen, blob2, sizeof(blob2));
    assert(n2 == n);
    assert(memcmp(blob, blob2, n) == 0);
}

static void test_device_binding(void)
{
    uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0x01, 0x02, 0x03 };
    uint8_t seed[32];
    fill(seed, sizeof(seed), 3);
    uint8_t key_a[APP_SECRET_KEY_LEN];
    app_secret_device_key(mac, sizeof(mac), seed, sizeof(seed), key_a);

    uint8_t mac_b[6];
    memcpy(mac_b, mac, sizeof(mac_b));
    mac_b[3] ^= 0x80;
    uint8_t key_b[APP_SECRET_KEY_LEN];
    app_secret_device_key(mac_b, sizeof(mac_b), seed, sizeof(seed), key_b);

    uint8_t iv[APP_SECRET_IV_LEN];
    fill(iv, sizeof(iv), 0x11);
    const char *plain = "seed-1234567890";

    uint8_t blob[APP_SECRET_BLOB_MAX(32)];
    size_t n = app_secret_seal(key_a, iv, plain, strlen(plain), blob, sizeof(blob));
    assert(n > 0);

    // 同一份密文拿到"另一台设备"（MAC 不同 → 密钥不同）必须解不开。
    uint8_t out[64];
    size_t out_len = 123;
    assert(!app_secret_open(key_b, blob, n, out, sizeof(out), &out_len));
    assert(out_len == 0);
}

static void test_wrong_key_and_tamper(void)
{
    uint8_t key[APP_SECRET_KEY_LEN];
    uint8_t iv[APP_SECRET_IV_LEN];
    fill(key, sizeof(key), 0x21);
    fill(iv, sizeof(iv), 0x55);

    const char *plain = "otp-secret-value";
    uint8_t blob[APP_SECRET_BLOB_MAX(32)];
    size_t n = app_secret_seal(key, iv, plain, strlen(plain), blob, sizeof(blob));
    assert(n > 0);

    uint8_t out[64];
    size_t out_len = 0;

    uint8_t wrong[APP_SECRET_KEY_LEN];
    memcpy(wrong, key, sizeof(wrong));
    wrong[0] ^= 0x01;
    assert(!app_secret_open(wrong, blob, n, out, sizeof(out), &out_len));

    // 逐区篡改：魔数/版本、IV、明文长度、密文、MAC 各自单独翻一位都必须被拒绝。
    uint8_t copy[APP_SECRET_BLOB_MAX(32)];
    const size_t positions[] = { 0, 4, 8, 21, APP_SECRET_HEADER_LEN + 3, n - 1 };
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        memcpy(copy, blob, n);
        copy[positions[i]] ^= 0x01;
        out_len = 99;
        assert(!app_secret_open(key, copy, n, out, sizeof(out), &out_len));
        assert(out_len == 0);
    }
}

static void test_bad_lengths(void)
{
    uint8_t key[APP_SECRET_KEY_LEN];
    uint8_t iv[APP_SECRET_IV_LEN];
    fill(key, sizeof(key), 0x33);
    fill(iv, sizeof(iv), 0x66);

    const char *plain = "abc";
    uint8_t blob[APP_SECRET_BLOB_MAX(32)];
    size_t n = app_secret_seal(key, iv, plain, strlen(plain), blob, sizeof(blob));
    assert(n > 0);

    uint8_t out[64];
    size_t out_len = 0;

    // 截断：去掉 MAC 的尾部若干字节后长度不再自洽。
    assert(!app_secret_open(key, blob, n - 1, out, sizeof(out), &out_len));
    assert(!app_secret_open(key, blob, 0, out, sizeof(out), &out_len));
    assert(!app_secret_open(key, blob, APP_SECRET_HEADER_LEN + 15, out, sizeof(out), &out_len));

    // 输出缓冲不足：即使 MAC 通过，也拒绝写出可能被截断的明文。
    uint8_t tiny[2];
    assert(!app_secret_open(key, blob, n, tiny, sizeof(tiny), &out_len));

    // 封装的容量与参数边界。
    assert(app_secret_seal(key, iv, plain, strlen(plain), blob,
                           APP_SECRET_HEADER_LEN + APP_SECRET_MAC_LEN + 15) == 0);
    assert(app_secret_seal(key, iv, plain, 0, blob, sizeof(blob)) == 0);
    assert(app_secret_seal(key, NULL, plain, strlen(plain), blob, sizeof(blob)) == 0);
    assert(app_secret_seal(NULL, iv, plain, strlen(plain), blob, sizeof(blob)) == 0);
}

int main(void)
{
    test_device_key();
    test_roundtrip();
    test_device_binding();
    test_wrong_key_and_tamper();
    test_bad_lengths();

    puts("test_app_secret: PASS");
    return 0;
}
