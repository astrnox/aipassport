// main/logic/app_crypto.c —— app_crypto.h 的实现：SHA-256 / HMAC / PBKDF2 / AES-256-CBC。
//
// 全部按 FIPS 180-4、RFC 2104、RFC 8018 与 FIPS 197 直写，不使用查表加速（T-table）。
// 密码本一次最多处理几 KB 数据，AES 每块 14 轮的字节实现已经足够快，而字节实现更容易
// 对照标准逐行核对。所有算法都在 tests/test_app_crypto.c 里对着标准测试向量校验。
#include "app_crypto.h"

#include <string.h>

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
static const uint32_t SHA256_K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u,
    0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u,
    0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
    0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au,
    0x5B9CCA4Fu, 0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

static uint32_t ror32(uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void app_sha256_init(app_sha256_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->h[0] = 0x6A09E667u;
    ctx->h[1] = 0xBB67AE85u;
    ctx->h[2] = 0x3C6EF372u;
    ctx->h[3] = 0xA54FF53Au;
    ctx->h[4] = 0x510E527Fu;
    ctx->h[5] = 0x9B05688Cu;
    ctx->h[6] = 0x1F83D9ABu;
    ctx->h[7] = 0x5BE0CD19u;
    ctx->total = 0;
    ctx->buf_len = 0;
}

static void sha256_block(app_sha256_ctx_t *ctx, const uint8_t *block)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];
    uint32_t f = ctx->h[5];
    uint32_t g = ctx->h[6];
    uint32_t h = ctx->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void app_sha256_update(app_sha256_ctx_t *ctx, const void *data, size_t len)
{
    if (!ctx || (!data && len > 0)) return;

    const uint8_t *bytes = (const uint8_t *)data;
    ctx->total += len;
    while (len > 0) {
        size_t space = sizeof(ctx->buf) - ctx->buf_len;
        size_t take = len < space ? len : space;
        memcpy(ctx->buf + ctx->buf_len, bytes, take);
        ctx->buf_len += take;
        bytes += take;
        len -= take;
        if (ctx->buf_len == sizeof(ctx->buf)) {
            sha256_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void app_sha256_final(app_sha256_ctx_t *ctx, uint8_t out[APP_SHA256_LEN])
{
    if (!ctx || !out) return;

    uint64_t bits = ctx->total * 8;
    uint8_t pad = 0x80;
    app_sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    // 补零到当前分组只剩 8 字节，留给长度字段。
    while (ctx->buf_len != 56) {
        app_sha256_update(ctx, &zero, 1);
    }
    uint8_t length_be[8];
    for (int i = 0; i < 8; i++) {
        length_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    app_sha256_update(ctx, length_be, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void app_sha256(const void *data, size_t len, uint8_t out[APP_SHA256_LEN])
{
    app_sha256_ctx_t ctx;
    app_sha256_init(&ctx);
    app_sha256_update(&ctx, data, len);
    app_sha256_final(&ctx, out);
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------
// 流式上下文：PBKDF2 需要把"长 salt || 块序号"直接喂进去，不能先拼进定长缓冲，
// 否则 salt 超长时只能截断，算出的是另一种算法。
#define SHA256_BLOCK_LEN 64   // SHA-256 分组长度，也是 HMAC 的内/外填充长度

typedef struct {
    app_sha256_ctx_t ctx;                          // 已经吃进 inner_pad 与已有消息
    uint8_t          outer_pad[SHA256_BLOCK_LEN];
} hmac_ctx_t;

static void hmac_init(hmac_ctx_t *h, const uint8_t *key, size_t key_len)
{
    uint8_t block[SHA256_BLOCK_LEN];
    memset(block, 0, sizeof(block));
    if (key && key_len > sizeof(block)) {
        // 超长密钥先压缩成 32 字节摘要，再当作密钥使用（RFC 2104）。
        app_sha256(key, key_len, block);
    } else if (key && key_len > 0) {
        memcpy(block, key, key_len);
    }

    uint8_t inner_pad[SHA256_BLOCK_LEN];
    for (size_t i = 0; i < sizeof(block); i++) {
        inner_pad[i]      = (uint8_t)(block[i] ^ 0x36);
        h->outer_pad[i]   = (uint8_t)(block[i] ^ 0x5C);
    }

    app_sha256_init(&h->ctx);
    app_sha256_update(&h->ctx, inner_pad, sizeof(inner_pad));

    app_crypto_zero(inner_pad, sizeof(inner_pad));
    app_crypto_zero(block, sizeof(block));
}

static void hmac_update(hmac_ctx_t *h, const void *data, size_t len)
{
    app_sha256_update(&h->ctx, data, len);
}

static void hmac_final(hmac_ctx_t *h, uint8_t out[APP_SHA256_LEN])
{
    uint8_t inner[APP_SHA256_LEN];
    app_sha256_final(&h->ctx, inner);

    app_sha256_ctx_t outer;
    app_sha256_init(&outer);
    app_sha256_update(&outer, h->outer_pad, sizeof(h->outer_pad));
    app_sha256_update(&outer, inner, sizeof(inner));
    app_sha256_final(&outer, out);

    app_crypto_zero(inner, sizeof(inner));
    app_crypto_zero(h->outer_pad, sizeof(h->outer_pad));
}

void app_hmac_sha256(const uint8_t *key, size_t key_len, const void *msg, size_t msg_len,
                     uint8_t out[APP_SHA256_LEN])
{
    if (!out) return;

    hmac_ctx_t h;
    hmac_init(&h, key, key ? key_len : 0);
    hmac_update(&h, msg, msg_len);
    hmac_final(&h, out);
}

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256
// ---------------------------------------------------------------------------
void app_pbkdf2_hmac_sha256(const uint8_t *pass, size_t pass_len,
                            const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (iterations == 0) iterations = 1;

    // U1 = HMAC(pass, salt || INT_32_BE(块序号))；Un = HMAC(pass, U(n-1))；
    // T = U1 xor U2 xor ... xor Uc。密钥只有 32 字节，即只需要第一个块（块序号 = 1）。
    if (out_len > APP_SHA256_LEN) out_len = APP_SHA256_LEN;

    const uint8_t index_be[4] = { 0, 0, 0, 1 };
    uint8_t u[APP_SHA256_LEN];
    uint8_t t[APP_SHA256_LEN];

    hmac_ctx_t h;
    hmac_init(&h, pass, pass ? pass_len : 0);
    hmac_update(&h, salt, salt_len);
    hmac_update(&h, index_be, sizeof(index_be));
    hmac_final(&h, u);
    memcpy(t, u, sizeof(t));

    for (uint32_t i = 1; i < iterations; i++) {
        hmac_init(&h, pass, pass ? pass_len : 0);
        hmac_update(&h, u, sizeof(u));
        hmac_final(&h, u);
        for (size_t k = 0; k < sizeof(t); k++) t[k] ^= u[k];
    }

    memcpy(out, t, out_len);
    app_crypto_zero(u, sizeof(u));
    app_crypto_zero(t, sizeof(t));
}

// ---------------------------------------------------------------------------
// AES：S 盒与 GF(2^8) 运算
// ---------------------------------------------------------------------------
static const uint8_t SBOX[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

// 轮常量 RCON：AES-256 的密钥扩展需要前 7 个（Nk=8 时 i/8 取到 7）。
static const uint8_t RCON[8] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

// 在 GF(2^8) 上乘 x（模 x^8+x^4+x^3+x+1）。
static uint8_t xtime(uint8_t value)
{
    return (uint8_t)((value << 1) ^ ((value & 0x80) ? 0x1B : 0x00));
}

// GF(2^8) 一般乘法，只用于逆列混合（系数 0x09/0x0B/0x0D/0x0E）。
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) result ^= a;
        b >>= 1;
        a = xtime(a);
    }
    return result;
}

// 轮密钥扩展：AES-256 为 Nk=8 / Nr=14，共 60 个 32 位字，输出 15 组 16 字节轮密钥。
static void aes256_expand_key(const uint8_t key[APP_AES256_KEY_LEN],
                              uint8_t round_keys[15][APP_AES_BLOCK_LEN])
{
    uint8_t w[60][4];

    for (int i = 0; i < 8; i++) {
        memcpy(w[i], key + i * 4, 4);
    }
    for (int i = 8; i < 60; i++) {
        uint8_t t[4];
        memcpy(t, w[i - 1], 4);
        if (i % 8 == 0) {
            // RotWord -> SubWord -> 异或轮常量。
            uint8_t first = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ RCON[i / 8 - 1]);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[first];
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++) w[i][j] = (uint8_t)(w[i - 8][j] ^ t[j]);
    }

    for (int i = 0; i < 60; i++) {
        memcpy(&round_keys[i / 4][(i % 4) * 4], w[i], 4);
    }
    app_crypto_zero(w, sizeof(w));
}

static void aes_add_round_key(uint8_t state[APP_AES_BLOCK_LEN],
                              const uint8_t round_key[APP_AES_BLOCK_LEN])
{
    for (int i = 0; i < APP_AES_BLOCK_LEN; i++) state[i] ^= round_key[i];
}

static void aes_sub_bytes(uint8_t state[APP_AES_BLOCK_LEN])
{
    for (int i = 0; i < APP_AES_BLOCK_LEN; i++) state[i] = SBOX[state[i]];
}

// 逆 S 盒不单独写一张表：从 SBOX 现算反查表，避免两张表不一致这类难以发现的错误。
// 一次 256 次赋值，相对一次 AES 解密可忽略。
static void aes_inv_sub_bytes(uint8_t state[APP_AES_BLOCK_LEN])
{
    uint8_t inv[256];
    for (int i = 0; i < 256; i++) inv[SBOX[i]] = (uint8_t)i;
    for (int i = 0; i < APP_AES_BLOCK_LEN; i++) state[i] = inv[state[i]];
}

// 状态按列优先存放：state[r + 4c] 是第 r 行第 c 列。
static void aes_shift_rows(uint8_t state[APP_AES_BLOCK_LEN])
{
    uint8_t t;

    t = state[1]; state[1] = state[5]; state[5] = state[9];
    state[9] = state[13]; state[13] = t;

    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;

    t = state[15]; state[15] = state[11]; state[11] = state[7];
    state[7] = state[3]; state[3] = t;
}

static void aes_inv_shift_rows(uint8_t state[APP_AES_BLOCK_LEN])
{
    uint8_t t;

    t = state[13]; state[13] = state[9]; state[9] = state[5];
    state[5] = state[1]; state[1] = t;

    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;

    t = state[3]; state[3] = state[7]; state[7] = state[11];
    state[11] = state[15]; state[15] = t;
}

static void aes_mix_columns(uint8_t state[APP_AES_BLOCK_LEN])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *p = state + c * 4;
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
        p[1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
        p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
        p[3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
    }
}

static void aes_inv_mix_columns(uint8_t state[APP_AES_BLOCK_LEN])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *p = state + c * 4;
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(gmul(a0, 0x0E) ^ gmul(a1, 0x0B) ^ gmul(a2, 0x0D) ^ gmul(a3, 0x09));
        p[1] = (uint8_t)(gmul(a0, 0x09) ^ gmul(a1, 0x0E) ^ gmul(a2, 0x0B) ^ gmul(a3, 0x0D));
        p[2] = (uint8_t)(gmul(a0, 0x0D) ^ gmul(a1, 0x09) ^ gmul(a2, 0x0E) ^ gmul(a3, 0x0B));
        p[3] = (uint8_t)(gmul(a0, 0x0B) ^ gmul(a1, 0x0D) ^ gmul(a2, 0x09) ^ gmul(a3, 0x0E));
    }
}

void app_aes256_encrypt_block(const uint8_t key[APP_AES256_KEY_LEN],
                              const uint8_t in[APP_AES_BLOCK_LEN],
                              uint8_t out[APP_AES_BLOCK_LEN])
{
    if (!key || !in || !out) return;

    uint8_t round_keys[15][APP_AES_BLOCK_LEN];
    aes256_expand_key(key, round_keys);

    uint8_t state[APP_AES_BLOCK_LEN];
    memcpy(state, in, sizeof(state));

    aes_add_round_key(state, round_keys[0]);
    for (int round = 1; round < 14; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, round_keys[round]);
    }
    // 最后一轮不做列混合。
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, round_keys[14]);

    memcpy(out, state, sizeof(state));
    app_crypto_zero(round_keys, sizeof(round_keys));
    app_crypto_zero(state, sizeof(state));
}

void app_aes256_decrypt_block(const uint8_t key[APP_AES256_KEY_LEN],
                              const uint8_t in[APP_AES_BLOCK_LEN],
                              uint8_t out[APP_AES_BLOCK_LEN])
{
    if (!key || !in || !out) return;

    uint8_t round_keys[15][APP_AES_BLOCK_LEN];
    aes256_expand_key(key, round_keys);

    uint8_t state[APP_AES_BLOCK_LEN];
    memcpy(state, in, sizeof(state));

    aes_add_round_key(state, round_keys[14]);
    for (int round = 13; round >= 1; round--) {
        aes_inv_shift_rows(state);
        aes_inv_sub_bytes(state);
        aes_add_round_key(state, round_keys[round]);
        aes_inv_mix_columns(state);
    }
    aes_inv_shift_rows(state);
    aes_inv_sub_bytes(state);
    aes_add_round_key(state, round_keys[0]);

    memcpy(out, state, sizeof(state));
    app_crypto_zero(round_keys, sizeof(round_keys));
    app_crypto_zero(state, sizeof(state));
}

// ---------------------------------------------------------------------------
// AES-256-CBC + PKCS#7
// ---------------------------------------------------------------------------
size_t app_aes256_cbc_encrypt(const uint8_t key[APP_AES256_KEY_LEN],
                              const uint8_t iv[APP_AES256_IV_LEN],
                              const void *in, size_t len,
                              uint8_t *out, size_t out_cap)
{
    if (!key || !iv || !out) return 0;
    if (!in && len > 0) return 0;

    // PKCS#7 一定补一个 1..16 字节的填充块，即使明文正好是分组长度。
    size_t pad = APP_AES_BLOCK_LEN - (len % APP_AES_BLOCK_LEN);
    size_t total = len + pad;
    if (total > out_cap) return 0;

    uint8_t chain[APP_AES_BLOCK_LEN];
    memcpy(chain, iv, sizeof(chain));

    const uint8_t *plain = (const uint8_t *)in;
    for (size_t offset = 0; offset < total; offset += APP_AES_BLOCK_LEN) {
        uint8_t block[APP_AES_BLOCK_LEN];
        for (size_t i = 0; i < APP_AES_BLOCK_LEN; i++) {
            size_t index = offset + i;
            uint8_t byte = (index < len) ? plain[index] : (uint8_t)pad;
            block[i] = (uint8_t)(byte ^ chain[i]);
        }
        app_aes256_encrypt_block(key, block, out + offset);
        memcpy(chain, out + offset, APP_AES_BLOCK_LEN);
    }
    app_crypto_zero(chain, sizeof(chain));
    return total;
}

bool app_aes256_cbc_decrypt(const uint8_t key[APP_AES256_KEY_LEN],
                            const uint8_t iv[APP_AES256_IV_LEN],
                            const void *in, size_t in_len,
                            uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!key || !iv || !in || !out || in_len == 0) return false;
    if (in_len % APP_AES_BLOCK_LEN != 0) return false;
    if (in_len > out_cap) return false;

    uint8_t chain[APP_AES_BLOCK_LEN];
    memcpy(chain, iv, sizeof(chain));

    const uint8_t *cipher = (const uint8_t *)in;
    for (size_t offset = 0; offset < in_len; offset += APP_AES_BLOCK_LEN) {
        uint8_t plain_block[APP_AES_BLOCK_LEN];
        app_aes256_decrypt_block(key, cipher + offset, plain_block);
        for (size_t i = 0; i < APP_AES_BLOCK_LEN; i++) {
            out[offset + i] = (uint8_t)(plain_block[i] ^ chain[i]);
        }
        memcpy(chain, cipher + offset, APP_AES_BLOCK_LEN);
    }
    app_crypto_zero(chain, sizeof(chain));

    // 去掉 PKCS#7 填充。填充长度必须落在 1..16，且所有填充字节一致。
    uint8_t pad = out[in_len - 1];
    if (pad == 0 || pad > APP_AES_BLOCK_LEN || pad > in_len) return false;
    for (size_t i = 0; i < pad; i++) {
        if (out[in_len - 1 - i] != pad) return false;
    }

    size_t plain_len = in_len - pad;
    // 填充字节只留在调用方缓冲的尾部，不属于明文，但也不清（调用方按 plain_len 使用）。
    if (out_len) *out_len = plain_len;
    return true;
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
bool app_crypto_equal(const void *a, const void *b, size_t len)
{
    if (!a || !b) return len == 0;

    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}

void app_crypto_zero(void *buf, size_t len)
{
    if (!buf) return;

    // volatile 指针防止编译器判定"写完不再读取"而整段删掉。
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (len--) *p++ = 0;
}
