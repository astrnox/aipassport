// main/logic/app_totp.c —— RFC 6238 TOTP 的纯逻辑实现。
//
// SHA-256 与 HMAC 由 logic/app_crypto 提供（密码本也用同一份实现），本文件只保留
// TOTP 特有的部分：SHA-1（RFC 6238 的默认算法，密码本用不到）、HMAC 的算法表、
// Base32 解码与 otpauth:// 解析。全部自实现而不依赖 mbedtls，是为了让这一层能在
// 主机上脱离 ESP-IDF 直接编译与单测。
#include "app_totp.h"

#include "app_crypto.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 32 位循环左移（SHA-1 用；SHA-256 的循环右移在 app_crypto 内）。
// ---------------------------------------------------------------------------
static uint32_t rol32(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

// ---------------------------------------------------------------------------
// SHA1
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t h[5];      // 中间状态（链变量）
    uint64_t total;     // 已喂入的字节数，用于填充长度
    uint8_t  buf[64];   // 未满一个分组的残留数据
    size_t   buf_len;   // buf 中有效字节数
} sha1_ctx_t;

static void sha1_init(sha1_ctx_t *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total = 0;
    ctx->buf_len = 0;
}

static void sha1_block(sha1_ctx_t *ctx, const uint8_t *block)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = tmp;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->total += len;
    while (len > 0) {
        size_t space = sizeof(ctx->buf) - ctx->buf_len;
        size_t take = len < space ? len : space;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == sizeof(ctx->buf)) {
            sha1_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t *out)
{
    uint64_t bits = ctx->total * 8;
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);
    uint8_t zero = 0;
    // 补零到当前分组还剩 8 字节（留出长度字段）。
    while (ctx->buf_len != 56) {
        sha1_update(ctx, &zero, 1);
    }
    uint8_t length_be[8];
    for (int i = 0; i < 8; i++) {
        length_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(ctx, length_be, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

// ---------------------------------------------------------------------------
// HMAC：用函数表抹平 SHA1 / SHA256 的差异。
// ---------------------------------------------------------------------------
typedef struct {
    void (*init)(void *ctx);
    void (*update)(void *ctx, const uint8_t *data, size_t len);
    void (*final)(void *ctx, uint8_t *out);
    size_t digest_len;   // 输出摘要长度（SHA1=20，SHA256=32）
    size_t block_len;    // 分组长度（两者都是 64）
} hash_ops_t;

typedef union {
    sha1_ctx_t        sha1;
    app_sha256_ctx_t  sha256;
} hash_ctx_t;

static void sha1_init_v(void *ctx) { sha1_init((sha1_ctx_t *)ctx); }
static void sha1_update_v(void *ctx, const uint8_t *data, size_t len)
{
    sha1_update((sha1_ctx_t *)ctx, data, len);
}
static void sha1_final_v(void *ctx, uint8_t *out) { sha1_final((sha1_ctx_t *)ctx, out); }

static void sha256_init_v(void *ctx) { app_sha256_init((app_sha256_ctx_t *)ctx); }
static void sha256_update_v(void *ctx, const uint8_t *data, size_t len)
{
    app_sha256_update((app_sha256_ctx_t *)ctx, data, len);
}
static void sha256_final_v(void *ctx, uint8_t *out)
{
    app_sha256_final((app_sha256_ctx_t *)ctx, out);
}

static const hash_ops_t HASH_SHA1 = { sha1_init_v, sha1_update_v, sha1_final_v, 20, 64 };
static const hash_ops_t HASH_SHA256 = { sha256_init_v, sha256_update_v, sha256_final_v, 32, 64 };

// 计算 HMAC(key, msg) 写入 out（至少 digest_len 字节）。key 超过分组长度时先哈希压缩。
static void hmac_compute(const hash_ops_t *ops, const uint8_t *key, size_t key_len,
                         const uint8_t *msg, size_t msg_len, uint8_t *out)
{
    uint8_t block[64];
    memset(block, 0, sizeof(block));
    if (key_len > ops->block_len) {
        hash_ctx_t tmp;
        ops->init(&tmp);
        ops->update(&tmp, key, key_len);
        ops->final(&tmp, block);
    } else {
        memcpy(block, key, key_len);
    }

    uint8_t inner_pad[64];
    uint8_t outer_pad[64];
    for (size_t i = 0; i < ops->block_len; i++) {
        inner_pad[i] = (uint8_t)(block[i] ^ 0x36);
        outer_pad[i] = (uint8_t)(block[i] ^ 0x5C);
    }

    hash_ctx_t ctx;
    uint8_t inner_digest[32];
    ops->init(&ctx);
    ops->update(&ctx, inner_pad, ops->block_len);
    ops->update(&ctx, msg, msg_len);
    ops->final(&ctx, inner_digest);

    ops->init(&ctx);
    ops->update(&ctx, outer_pad, ops->block_len);
    ops->update(&ctx, inner_digest, ops->digest_len);
    ops->final(&ctx, out);
}

// ---------------------------------------------------------------------------
// Base32 解码
// ---------------------------------------------------------------------------
static int base32_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

bool app_totp_base32_decode(const char *text, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!text || !out || !out_len) return false;
    *out_len = 0;

    uint32_t buffer = 0;   // 位缓冲区
    int bits = 0;          // buffer 中有效位数
    size_t written = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        char c = *cursor;
        // 容忍空格、分隔符与填充。
        if (c == ' ' || c == '-' || c == '=') continue;
        int value = base32_value(c);
        if (value < 0) return false;
        buffer = (buffer << 5) | (uint32_t)value;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (written >= out_cap) return false;
            out[written++] = (uint8_t)((buffer >> bits) & 0xFF);
        }
    }
    if (written == 0) return false;   // 空输入或只有分隔符
    *out_len = written;
    return true;
}

// ---------------------------------------------------------------------------
// TOTP 计算
// ---------------------------------------------------------------------------
bool app_totp_code(const app_totp_account_t *acct, uint64_t unix_time, char *out, size_t out_cap)
{
    if (!acct || !out) return false;
    if (acct->digits != 6 && acct->digits != 8) return false;
    if (acct->period <= 0) return false;
    if (acct->secret_len == 0 || acct->secret_len > APP_TOTP_MAX_SECRET_BYTES) return false;
    if (out_cap < (size_t)acct->digits + 1) return false;

    // 计数器 = unix_time / period，按 64 位大端编码。
    uint64_t counter = unix_time / (uint64_t)acct->period;
    uint8_t message[8];
    for (int i = 0; i < 8; i++) {
        message[7 - i] = (uint8_t)((counter >> (8 * i)) & 0xFF);
    }

    const hash_ops_t *ops = (acct->algo == APP_TOTP_ALGO_SHA256) ? &HASH_SHA256 : &HASH_SHA1;
    uint8_t mac[32];
    hmac_compute(ops, acct->secret, acct->secret_len, message, sizeof(message), mac);

    // 动态截断：取最后一字节低 4 位作为偏移，取 4 字节并屏蔽最高位。
    int offset = mac[ops->digest_len - 1] & 0x0F;
    uint32_t binary = ((uint32_t)(mac[offset] & 0x7F) << 24) |
                      ((uint32_t)mac[offset + 1] << 16) |
                      ((uint32_t)mac[offset + 2] << 8) |
                      (uint32_t)mac[offset + 3];

    uint32_t modulus = 1;
    for (int i = 0; i < acct->digits; i++) modulus *= 10;
    uint32_t value = binary % modulus;

    snprintf(out, out_cap, "%0*u", acct->digits, (unsigned)value);
    return true;
}

int app_totp_remaining(const app_totp_account_t *acct, uint64_t unix_time)
{
    if (!acct || acct->period <= 0) return -1;
    return acct->period - (int)(unix_time % (uint64_t)acct->period);
}

int app_totp_format(const char *code, char *out, size_t out_cap)
{
    if (!code || !out || out_cap < 12) return -1;
    size_t len = strlen(code);
    if (len == 6) {
        return snprintf(out, out_cap, "%.3s %.3s", code, code + 3);
    }
    if (len == 8) {
        return snprintf(out, out_cap, "%.4s %.4s", code, code + 4);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// otpauth URI 解析
// ---------------------------------------------------------------------------
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 把 src 的 src_len 字节做 URL 解码写入 dst（始终 NUL 结尾），返回写入字节数。
static size_t url_decode(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
    size_t written = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                if (written + 1 < dst_cap) dst[written++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (written + 1 < dst_cap) dst[written++] = c;
    }
    if (dst_cap > 0) dst[written] = '\0';
    return written;
}

// 返回该字节在 UTF-8 序列中的长度；非法起始字节按 1 处理。
static size_t utf8_seq_len(unsigned char byte)
{
    if (byte < 0x80) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;
}

// 复制 UTF-8 前缀到 dst，最多占用 cap - 1 字节，且不在字符中间截断。
static void copy_utf8_prefix(const char *src, char *dst, size_t cap)
{
    if (cap == 0) return;
    size_t limit = cap - 1;
    size_t n = 0;
    while (src[n] != '\0' && n < limit) {
        size_t seq = utf8_seq_len((unsigned char)src[n]);
        if (n + seq > limit) break;
        n += seq;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// 大小写不敏感地比较定长 token 与名字。
static bool token_equals_ci(const char *token, size_t len, const char *name)
{
    size_t name_len = strlen(name);
    if (len != name_len) return false;
    for (size_t i = 0; i < len; i++) {
        char a = token[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

// 从定长数字串解析整数，无数字返回 -1。
static int parse_uint(const char *text, size_t len)
{
    int value = 0;
    size_t i = 0;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (text[i] - '0');
        if (value > 1000000) return 1000000;
        i++;
    }
    return i == 0 ? -1 : value;
}

bool app_totp_parse_uri(const char *uri, app_totp_account_t *out)
{
    if (!uri || !out) return false;

    const char *prefix = "otpauth://totp/";
    size_t prefix_len = strlen(prefix);
    if (strlen(uri) < prefix_len) return false;
    if (!token_equals_ci(uri, prefix_len, prefix)) return false;

    const char *label_start = uri + prefix_len;
    const char *query = strchr(label_start, '?');
    const char *label_end = query ? query : label_start + strlen(label_start);

    // 先 URL 解码，再取最后一个 ':' 之后的账户名（Issuer:Account）。
    char decoded_label[160];
    url_decode(label_start, (size_t)(label_end - label_start), decoded_label, sizeof(decoded_label));
    const char *account = decoded_label;
    const char *colon = strrchr(decoded_label, ':');
    if (colon) account = colon + 1;

    memset(out, 0, sizeof(*out));
    out->digits = 6;
    out->period = 30;
    out->algo = APP_TOTP_ALGO_SHA1;
    copy_utf8_prefix(account, out->label, sizeof(out->label));

    char secret_text[128];
    secret_text[0] = '\0';
    bool have_secret = false;

    if (query) {
        const char *cursor = query + 1;
        while (*cursor) {
            const char *amp = strchr(cursor, '&');
            const char *end = amp ? amp : cursor + strlen(cursor);
            const char *eq = memchr(cursor, '=', (size_t)(end - cursor));
            if (eq) {
                size_t key_len = (size_t)(eq - cursor);
                const char *value = eq + 1;
                size_t value_len = (size_t)(end - value);
                if (token_equals_ci(cursor, key_len, "secret")) {
                    url_decode(value, value_len, secret_text, sizeof(secret_text));
                    have_secret = secret_text[0] != '\0';
                } else if (token_equals_ci(cursor, key_len, "digits")) {
                    int digits = parse_uint(value, value_len);
                    if (digits == 6 || digits == 8) out->digits = digits;
                } else if (token_equals_ci(cursor, key_len, "period")) {
                    int period = parse_uint(value, value_len);
                    if (period > 0) out->period = period;
                } else if (token_equals_ci(cursor, key_len, "algorithm")) {
                    out->algo = token_equals_ci(value, value_len, "SHA256")
                                    ? APP_TOTP_ALGO_SHA256
                                    : APP_TOTP_ALGO_SHA1;
                }
            }
            if (!amp) break;
            cursor = amp + 1;
        }
    }

    if (!have_secret) return false;

    size_t secret_len = 0;
    if (!app_totp_base32_decode(secret_text, out->secret, sizeof(out->secret), &secret_len)) {
        return false;
    }
    out->secret_len = secret_len;
    return true;
}
