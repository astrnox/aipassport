// tests/test_app_crypto.c —— app_crypto 的主机测试，对照公开标准测试向量。
//
// 向量来源：
//   SHA-256      FIPS 180-4 示例 + NIST 空串向量
//   HMAC-SHA256  RFC 4231 测试用例 1、2
//   PBKDF2       RFC 7914 §11 与常见 HMAC-SHA256 向量（password/salt）
//   AES-256      FIPS 197 附录 C.3（单块）
//   AES-256-CBC  NIST SP 800-38A F.2.5（首块交叉验证；本实现固定 PKCS#7 填充）
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_crypto.h"

// 把十六进制字符串解成字节，返回字节数。
static size_t unhex(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        assert(n < out_cap);
        int hi = (p[0] >= 'a') ? p[0] - 'a' + 10 : (p[0] >= 'A') ? p[0] - 'A' + 10 : p[0] - '0';
        int lo = (p[1] >= 'a') ? p[1] - 'a' + 10 : (p[1] >= 'A') ? p[1] - 'A' + 10 : p[1] - '0';
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// 把字节转成十六进制字符串，便于断言失败时看清差异。
static void to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_cap)
{
    static const char digits[] = "0123456789abcdef";
    assert(out_cap >= len * 2 + 1);
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void assert_hex(const uint8_t *bytes, size_t len, const char *expect)
{
    char got[256];
    to_hex(bytes, len, got, sizeof(got));
    if (strcmp(got, expect) != 0) {
        fprintf(stderr, "expected %s\n     got %s\n", expect, got);
    }
    assert(strcmp(got, expect) == 0);
}

// ---------------------------------------------------------------------------

static void test_sha256(void)
{
    uint8_t digest[APP_SHA256_LEN];

    app_sha256("", 0, digest);
    assert_hex(digest, sizeof(digest),
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    app_sha256("abc", 3, digest);
    assert_hex(digest, sizeof(digest),
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    app_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, digest);
    assert_hex(digest, sizeof(digest),
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // 流式与一次性必须完全一致，且切分点任意（含跨分组边界与空分段）。
    uint8_t payload[1000];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7 + 3);

    uint8_t whole[APP_SHA256_LEN];
    app_sha256(payload, sizeof(payload), whole);

    for (size_t cut = 0; cut <= sizeof(payload); cut += 61) {
        app_sha256_ctx_t ctx;
        app_sha256_init(&ctx);
        app_sha256_update(&ctx, payload, cut);
        app_sha256_update(&ctx, NULL, 0);
        app_sha256_update(&ctx, payload + cut, sizeof(payload) - cut);
        uint8_t got[APP_SHA256_LEN];
        app_sha256_final(&ctx, got);
        assert(memcmp(got, whole, sizeof(whole)) == 0);
    }

    // 长度字段跨分组：64 字节输入会多补一个分组。
    memset(payload, 0x61, 64);
    app_sha256(payload, 64, digest);
    assert_hex(digest, sizeof(digest),
               "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

    // 参数保护：不因为空指针崩溃。
    app_sha256_init(NULL);
    app_sha256_update(NULL, "x", 1);
    app_sha256_final(NULL, digest);
    app_sha256("x", 1, NULL);
}

static void test_hmac_sha256(void)
{
    uint8_t key[131];
    uint8_t digest[APP_SHA256_LEN];

    // RFC 4231 用例 1：key = 20 字节 0x0b。
    memset(key, 0x0b, 20);
    app_hmac_sha256(key, 20, "Hi There", 8, digest);
    assert_hex(digest, sizeof(digest),
               "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 4231 用例 2："Jefe" / "what do ya want for nothing?"。
    app_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, digest);
    assert_hex(digest, sizeof(digest),
               "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // RFC 4231 用例 6：131 字节超长密钥（触发"先压缩密钥"分支）。
    memset(key, 0xaa, sizeof(key));
    app_hmac_sha256(key, sizeof(key), "Test Using Larger Than Block-Size Key - Hash Key First",
                    54, digest);
    assert_hex(digest, sizeof(digest),
               "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // 空消息与空密钥不能崩溃，且结果稳定。
    uint8_t a[APP_SHA256_LEN], b[APP_SHA256_LEN];
    app_hmac_sha256(NULL, 0, NULL, 0, a);
    app_hmac_sha256((const uint8_t *)"", 0, "", 0, b);
    assert(memcmp(a, b, sizeof(a)) == 0);
    app_hmac_sha256(key, sizeof(key), "x", 1, NULL);
}

static void test_pbkdf2(void)
{
    uint8_t out[32];
    const uint8_t *pass = (const uint8_t *)"password";
    const uint8_t *salt = (const uint8_t *)"salt";

    app_pbkdf2_hmac_sha256(pass, 8, salt, 4, 1, out, sizeof(out));
    assert_hex(out, sizeof(out),
               "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

    app_pbkdf2_hmac_sha256(pass, 8, salt, 4, 2, out, sizeof(out));
    assert_hex(out, sizeof(out),
               "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");

    app_pbkdf2_hmac_sha256(pass, 8, salt, 4, 4096, out, sizeof(out));
    assert_hex(out, sizeof(out),
               "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    // RFC 7914 §11 向量（dkLen=64，这里只取前 32 字节）。
    app_pbkdf2_hmac_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1,
                           out, sizeof(out));
    assert_hex(out, sizeof(out),
               "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");

    // 迭代次数为 0 时按 1 次处理（不允许"零次迭代"产出未加固的密钥）。
    uint8_t zero_iter[32];
    app_pbkdf2_hmac_sha256(pass, 8, salt, 4, 0, zero_iter, sizeof(zero_iter));
    uint8_t one_iter[32];
    app_pbkdf2_hmac_sha256(pass, 8, salt, 4, 1, one_iter, sizeof(one_iter));
    assert(memcmp(zero_iter, one_iter, sizeof(one_iter)) == 0);

    // 空 salt 合法。
    app_pbkdf2_hmac_sha256(pass, 8, NULL, 0, 2, out, sizeof(out));
    app_pbkdf2_hmac_sha256(pass, 8, NULL, 0, 2, NULL, sizeof(out));
    app_pbkdf2_hmac_sha256(pass, 8, NULL, 0, 2, out, 0);
}

static void test_aes_block(void)
{
    // FIPS 197 附录 C.3。
    uint8_t key[APP_AES256_KEY_LEN];
    uint8_t plain[APP_AES_BLOCK_LEN];
    uint8_t cipher[APP_AES_BLOCK_LEN];
    uint8_t back[APP_AES_BLOCK_LEN];

    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, sizeof(key));
    unhex("00112233445566778899aabbccddeeff", plain, sizeof(plain));

    app_aes256_encrypt_block(key, plain, cipher);
    assert_hex(cipher, sizeof(cipher), "8ea2b7ca516745bfeafc49904b496089");

    app_aes256_decrypt_block(key, cipher, back);
    assert(memcmp(back, plain, sizeof(plain)) == 0);

    // 全零密钥/全零明文也是一条有效的分组。
    uint8_t zero_key[APP_AES256_KEY_LEN] = { 0 };
    uint8_t zero_plain[APP_AES_BLOCK_LEN] = { 0 };
    app_aes256_encrypt_block(zero_key, zero_plain, cipher);
    assert_hex(cipher, sizeof(cipher), "dc95c078a2408989ad48a21492842087");
    app_aes256_decrypt_block(zero_key, cipher, back);
    assert(memcmp(back, zero_plain, sizeof(zero_plain)) == 0);
}

static void test_aes_cbc(void)
{
    uint8_t key[APP_AES256_KEY_LEN];
    uint8_t iv[APP_AES256_IV_LEN];
    unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key, sizeof(key));
    unhex("000102030405060708090a0b0c0d0e0f", iv, sizeof(iv));

    uint8_t block1[APP_AES_BLOCK_LEN];
    unhex("6bc1bee22e409f96e93d7e117393172a", block1, sizeof(block1));

    // NIST 向量不带填充，本实现固定 PKCS#7（多补一整块），因此首块密文应当一致。
    uint8_t out[64];
    size_t out_len = app_aes256_cbc_encrypt(key, iv, block1, sizeof(block1), out, sizeof(out));
    assert(out_len == sizeof(block1) + APP_AES_BLOCK_LEN);
    assert_hex(out, APP_AES_BLOCK_LEN, "f58c4c04d6e5f1ba779eabfb5f7bfbd6");

    // 反向：密文解出明文 + 填充。
    uint8_t plain[64];
    size_t plain_len = 0;
    assert(app_aes256_cbc_decrypt(key, iv, out, out_len, plain, sizeof(plain), &plain_len));
    assert(plain_len == sizeof(block1));
    assert(memcmp(plain, block1, plain_len) == 0);
}

static void test_aes_cbc_roundtrip(void)
{
    uint8_t key[APP_AES256_KEY_LEN];
    uint8_t iv[APP_AES256_IV_LEN];
    uint8_t cipher[256];
    uint8_t plain[256];

    for (size_t i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i * 13 + 1);
    for (size_t i = 0; i < sizeof(iv); i++) iv[i] = (uint8_t)(i * 29 + 5);

    // 长度 0..200 全部往返：含 0（只有填充块）、正好一个分组、跨多分组等边界。
    for (size_t len = 0; len <= 200; len++) {
        uint8_t source[200];
        for (size_t i = 0; i < len; i++) source[i] = (uint8_t)(i * 17 + len);

        size_t cipher_len = app_aes256_cbc_encrypt(key, iv, source, len, cipher, sizeof(cipher));
        assert(cipher_len > len);
        assert(cipher_len % APP_AES_BLOCK_LEN == 0);
        assert(cipher_len - len <= APP_AES_CBC_MAX_OVERHEAD);

        size_t plain_len = 0;
        assert(app_aes256_cbc_decrypt(key, iv, cipher, cipher_len, plain, sizeof(plain),
                                      &plain_len));
        assert(plain_len == len);
        assert(len == 0 || memcmp(plain, source, len) == 0);
    }

    // 同一明文同一 IV 结果确定；换 IV 结果不同。
    uint8_t a[32], b[32];
    size_t a_len = app_aes256_cbc_encrypt(key, iv, "hello", 5, a, sizeof(a));
    size_t b_len = app_aes256_cbc_encrypt(key, iv, "hello", 5, b, sizeof(b));
    assert(a_len == b_len && memcmp(a, b, a_len) == 0);
    iv[0] ^= 0xFF;
    (void)app_aes256_cbc_encrypt(key, iv, "hello", 5, b, sizeof(b));
    assert(memcmp(a, b, a_len) != 0);
}

static void test_aes_cbc_rejects(void)
{
    uint8_t key[APP_AES256_KEY_LEN] = { 0 };
    uint8_t iv[APP_AES256_IV_LEN] = { 0 };
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    size_t out_len = 0;

    // 长度非法：0、非 16 倍数。
    assert(!app_aes256_cbc_decrypt(key, iv, buf, 0, buf, sizeof(buf), &out_len));
    assert(!app_aes256_cbc_decrypt(key, iv, buf, 17, buf, sizeof(buf), &out_len));

    // 输出缓冲不足。
    assert(app_aes256_cbc_encrypt(key, iv, buf, 16, buf, 16) == 0);
    assert(!app_aes256_cbc_decrypt(key, iv, buf, 32, buf, 16, &out_len));

    // 用错误的密钥解密：要么填充非法直接失败，要么（极小概率）通过填充检查但明文不同。
    uint8_t cipher[16];
    assert(app_aes256_cbc_encrypt(key, iv, "secret", 6, cipher, sizeof(cipher)) == 16);

    uint8_t wrong_key[APP_AES256_KEY_LEN];
    memcpy(wrong_key, key, sizeof(key));
    wrong_key[0] = 1;
    uint8_t plain[64];
    bool ok = app_aes256_cbc_decrypt(wrong_key, iv, cipher, sizeof(cipher), plain, sizeof(plain),
                                     &out_len);
    assert(!ok || out_len != 6 || memcmp(plain, "secret", 6) != 0);

    // 参数保护。
    assert(app_aes256_cbc_encrypt(NULL, iv, buf, 16, buf, sizeof(buf)) == 0);
    assert(!app_aes256_cbc_decrypt(key, NULL, buf, 16, buf, sizeof(buf), &out_len));
    assert(!app_aes256_cbc_decrypt(key, iv, NULL, 16, buf, sizeof(buf), &out_len));
}

static void test_utils(void)
{
    uint8_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    assert(app_crypto_equal(a, b, sizeof(a)));

    b[7] ^= 1;
    assert(!app_crypto_equal(a, b, sizeof(a)));
    b[7] ^= 1;
    assert(app_crypto_equal(a, b, 0));
    assert(app_crypto_equal(NULL, NULL, 0));
    assert(!app_crypto_equal(NULL, b, sizeof(b)));

    app_crypto_zero(b, sizeof(b));
    for (size_t i = 0; i < sizeof(b); i++) assert(b[i] == 0);
    app_crypto_zero(NULL, 4);
}

int main(void)
{
    test_sha256();
    test_hmac_sha256();
    test_pbkdf2();
    test_aes_block();
    test_aes_cbc();
    test_aes_cbc_roundtrip();
    test_aes_cbc_rejects();
    test_utils();
    puts("test_app_crypto: PASS");
    return 0;
}
