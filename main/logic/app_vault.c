// main/logic/app_vault.c —— 密码保险箱实现。格式与安全定位见 app_vault.h。
#include "app_vault.h"

#include "app_crypto.h"
#include "app_text.h"

#include <stdio.h>
#include <string.h>

#define VAULT_MAGIC   0x31544C56u   // "VLT1"
#define VAULT_VERSION 1

// 由 DEK 派生两个用途分离的子密钥：一个加密正文，一个做 MAC。直接拿 DEK 当 MAC 密钥
// 会让"同一密钥既加密又认证"，派生一次 SHA-256 就避免了这种耦合。
#define DEK_DOMAIN_ENC 0x01
#define DEK_DOMAIN_MAC 0x02

static app_vault_random_fn s_random;
static void          *s_random_ctx;

void app_vault_set_random(app_vault_random_fn fn, void *ctx)
{
    s_random = fn;
    s_random_ctx = ctx;
}

// 取随机字节。没有随机源时返回 false——绝不用可预测字节冒充密钥。
static bool random_bytes(void *out, size_t len)
{
    if (!s_random || !out) return false;
    memset(out, 0, len);
    s_random(s_random_ctx, out, len);
    return true;
}

// ---------------------------------------------------------------------------
// 字节读写游标
// ---------------------------------------------------------------------------
// app_crypto 对外只提供一次性 HMAC，而容器是由写游标分段落盘的；这里用公开的 SHA-256
// 接口拼一个最小的流式 HMAC，让"MAC 覆盖哪些字节"与"实际写了哪些字节"仍由同一段
// 写游标代码决定，也避免为了做一次 MAC 再把整段容器复制进额外缓冲。
#define VAULT_SHA256_BLOCK 64   // SHA-256 分组长度，也是 HMAC 内/外填充长度

typedef struct {
    app_sha256_ctx_t ctx;                       // 已吃进 inner_pad 与已写字节
    uint8_t          outer_pad[VAULT_SHA256_BLOCK];
} vault_hmac_t;

static void vault_hmac_init(vault_hmac_t *h, const uint8_t *key, size_t key_len)
{
    uint8_t block[VAULT_SHA256_BLOCK];
    memset(block, 0, sizeof(block));
    if (key && key_len > sizeof(block)) {
        app_sha256(key, key_len, block);        // 超长密钥先压成摘要（RFC 2104）
    } else if (key && key_len > 0) {
        memcpy(block, key, key_len);
    }

    uint8_t inner_pad[VAULT_SHA256_BLOCK];
    for (size_t i = 0; i < sizeof(block); i++) {
        inner_pad[i]    = (uint8_t)(block[i] ^ 0x36);
        h->outer_pad[i] = (uint8_t)(block[i] ^ 0x5C);
    }

    app_sha256_init(&h->ctx);
    app_sha256_update(&h->ctx, inner_pad, sizeof(inner_pad));

    app_crypto_zero(inner_pad, sizeof(inner_pad));
    app_crypto_zero(block, sizeof(block));
}

static void vault_hmac_update(vault_hmac_t *h, const void *data, size_t len)
{
    app_sha256_update(&h->ctx, data, len);
}

static void vault_hmac_final(vault_hmac_t *h, uint8_t out[APP_SHA256_LEN])
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

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;
    vault_hmac_t *mac;
} writer_t;

static void w_raw(writer_t *w, const void *data, size_t len)
{
    if (len == 0) return;
    if (w->mac) vault_hmac_update(w->mac, data, len);
    // buf 为 NULL 表示"只喂给 MAC、不落盘"（校验与 refresh_mac 都用它），不能算溢出；
    // 只有确实提供了输出缓冲却写不下时才算溢出。
    if (w->buf) {
        if (w->pos + len <= w->cap) memcpy(w->buf + w->pos, data, len);
        else w->overflow = true;
    }
    w->pos += len;
}

static void w_u8(writer_t *w, uint8_t value)
{
    w_raw(w, &value, 1);
}

static void w_u16(writer_t *w, uint16_t value)
{
    const uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    w_raw(w, bytes, sizeof(bytes));
}

static void w_u32(writer_t *w, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    w_raw(w, bytes, sizeof(bytes));
}

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           underflow;
} reader_t;

static void r_raw(reader_t *r, void *out, size_t len)
{
    if (r->pos + len > r->len) {
        r->underflow = true;
        memset(out, 0, len);
        return;
    }
    memcpy(out, r->buf + r->pos, len);
    r->pos += len;
}

static uint8_t r_u8(reader_t *r)
{
    uint8_t value = 0;
    r_raw(r, &value, 1);
    return value;
}

static uint16_t r_u16(reader_t *r)
{
    uint8_t bytes[2];
    r_raw(r, bytes, sizeof(bytes));
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t r_u32(reader_t *r)
{
    uint8_t bytes[4];
    r_raw(r, bytes, sizeof(bytes));
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

// ---------------------------------------------------------------------------
// 条目文本
// ---------------------------------------------------------------------------

// 把用户输入拷进定长字段：非法 UTF-8 一律拒绝，超长按字符数截断。
static bool copy_field(const char *src, char *out, size_t cap, size_t max_chars)
{
    if (cap == 0) return false;
    out[0] = '\0';
    if (!src || !src[0]) return true;
    if (!app_utf8_valid(src)) return false;
    app_utf8_copy_prefix(src, max_chars, out, cap);
    return true;
}

static void entry_write(writer_t *w, const app_vault_entry_t *e)
{
    size_t label_len = strlen(e->label);
    size_t acct_len = strlen(e->account);
    size_t pass_len = strlen(e->password);

    if (label_len >= APP_VAULT_LABEL_LEN) label_len = APP_VAULT_LABEL_LEN - 1;
    if (acct_len >= APP_VAULT_ACCOUNT_LEN) acct_len = APP_VAULT_ACCOUNT_LEN - 1;
    if (pass_len >= APP_VAULT_PASSWORD_LEN) pass_len = APP_VAULT_PASSWORD_LEN - 1;

    w_u8(w, (uint8_t)label_len);
    w_raw(w, e->label, label_len);
    w_u8(w, (uint8_t)acct_len);
    w_raw(w, e->account, acct_len);
    w_u8(w, (uint8_t)pass_len);
    w_raw(w, e->password, pass_len);
}

static bool entry_read(reader_t *r, app_vault_entry_t *e)
{
    memset(e, 0, sizeof(*e));

    uint8_t label_len = r_u8(r);
    if (label_len >= APP_VAULT_LABEL_LEN) return false;
    r_raw(r, e->label, label_len);

    uint8_t acct_len = r_u8(r);
    if (acct_len >= APP_VAULT_ACCOUNT_LEN) return false;
    r_raw(r, e->account, acct_len);

    uint8_t pass_len = r_u8(r);
    if (pass_len >= APP_VAULT_PASSWORD_LEN) return false;
    r_raw(r, e->password, pass_len);

    if (r->underflow) return false;
    e->used = true;
    return true;
}

// 把当前全部条目写成明文记录区，返回字节数；提供空指针时只计算所需长度。
static size_t records_build(const app_vault_t *v, uint8_t *out, size_t cap)
{
    writer_t w = { out, cap, 0, false, NULL };
    for (int i = 0; i < v->count && i < APP_VAULT_MAX; i++) {
        if (!v->entries[i].used) continue;
        entry_write(&w, &v->entries[i]);
    }
    return w.overflow ? 0 : w.pos;
}

static app_vault_status_t records_parse(app_vault_t *v, const uint8_t *data, size_t len, int count)
{
    reader_t r = { data, len, 0, false };

    if (count < 0 || count > APP_VAULT_MAX) return APP_VAULT_ERR_FORMAT;
    memset(v->entries, 0, sizeof(v->entries));
    for (int i = 0; i < count; i++) {
        if (!entry_read(&r, &v->entries[i])) return APP_VAULT_ERR_FORMAT;
    }
    if (r.pos != len) return APP_VAULT_ERR_FORMAT;   // 尾部有多余字节，说明容器被改过

    v->count = count;
    if (v->selected >= count) v->selected = count > 0 ? count - 1 : 0;
    if (v->selected < 0) v->selected = 0;
    return APP_VAULT_OK;
}

// 写容器头与正文（magic 到 payload 结束）。序列化与"仅刷新 MAC"共用这一处，
// 保证 MAC 覆盖的字节范围只有一份定义，不会两处顺序写岔。
static void container_write(writer_t *w, const app_vault_t *v)
{
    w_u32(w, VAULT_MAGIC);
    w_u8(w, VAULT_VERSION);
    w_u8(w, (uint8_t)v->mode);
    w_u16(w, (uint16_t)v->count);

    if (v->mode == APP_VAULT_ENCRYPTED) {
        w_raw(w, v->salt_knock, sizeof(v->salt_knock));
        w_raw(w, v->salt_recovery, sizeof(v->salt_recovery));
        w_u32(w, v->iterations);
        w_raw(w, v->iv_wrap_knock, sizeof(v->iv_wrap_knock));
        w_raw(w, v->wrap_knock, sizeof(v->wrap_knock));
        w_raw(w, v->iv_wrap_recovery, sizeof(v->iv_wrap_recovery));
        w_raw(w, v->wrap_recovery, sizeof(v->wrap_recovery));
        w_u32(w, (uint32_t)v->payload_len);
        w_raw(w, v->iv_payload, sizeof(v->iv_payload));
        w_raw(w, v->payload, v->payload_len);
    } else {
        for (int i = 0; i < v->count && i < APP_VAULT_MAX; i++) {
            if (!v->entries[i].used) continue;
            entry_write(w, &v->entries[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// 密钥派生与封装
// ---------------------------------------------------------------------------
static void derive_subkeys(const uint8_t dek[32], uint8_t enc_key[32], uint8_t mac_key[32])
{
    uint8_t material[33];
    memcpy(material, dek, 32);

    material[32] = DEK_DOMAIN_ENC;
    app_sha256(material, sizeof(material), enc_key);
    material[32] = DEK_DOMAIN_MAC;
    app_sha256(material, sizeof(material), mac_key);

    app_crypto_zero(material, sizeof(material));
}

// 重新计算并写回 v->mac。MAC 覆盖整段容器（含两份 DEK 封装），改手势或换恢复码都会
// 改动其中的字段；若不在这里同步刷新，紧接着的"上锁 → 序列化"会把旧 MAC 原样写回，
// 下次解锁会因校验失败而永久打不开。不落盘，只把字节喂给 HMAC。
static void refresh_mac(app_vault_t *v)
{
    if (!v->has_dek) return;

    uint8_t enc_key[32];
    uint8_t mac_key[32];
    derive_subkeys(v->dek, enc_key, mac_key);

    vault_hmac_t hmac;
    vault_hmac_init(&hmac, mac_key, APP_SHA256_LEN);
    writer_t w = { NULL, 0, 0, false, &hmac };
    container_write(&w, v);
    vault_hmac_final(&hmac, v->mac);

    app_crypto_zero(enc_key, sizeof(enc_key));
    app_crypto_zero(mac_key, sizeof(mac_key));
}

// 用手势派生 KEK。taps 直接作为口令字节（0/1），长度参与推导，因此"上上"和"上上上"
// 不会派生出同一个密钥。
static void knock_derive_kek(const app_vault_knock_t *knock, const uint8_t salt[16],
                             uint32_t iterations, uint8_t kek[32])
{
    app_pbkdf2_hmac_sha256(knock->taps, (size_t)knock->len, salt, 16, iterations, kek, 32);
}

// 用恢复码的 16 字节原文派生 KEK。
static void recovery_derive_kek(const uint8_t secret[16], const uint8_t salt[16],
                                uint32_t iterations, uint8_t kek[32])
{
    app_pbkdf2_hmac_sha256(secret, 16, salt, 16, iterations, kek, 32);
}

// 用 KEK 把 DEK 封装成 48 字节（AES-256-CBC 对 32 字节明文一定补出第二个分组）。
static bool wrap_dek(const uint8_t dek[32], const uint8_t kek[32], uint8_t iv[16],
                     uint8_t out[APP_VAULT_WRAP_LEN])
{
    if (!random_bytes(iv, 16)) return false;
    return app_aes256_cbc_encrypt(kek, iv, dek, 32, out, APP_VAULT_WRAP_LEN) ==
           APP_VAULT_WRAP_LEN;
}

static bool unwrap_dek(const uint8_t kek[32], const uint8_t iv[16],
                       const uint8_t wrapped[APP_VAULT_WRAP_LEN], uint8_t dek[32])
{
    // 解密会先把整段明文（含 PKCS#7 填充）写进输出缓冲，再按填充裁剪长度；若直接把
    // 32 字节的 dek 当输出（小于 48 字节密文）传进去，会被判成缓冲不足而永远解封失败。
    // 这里用一个 48 字节的临时缓冲接结果，只把裁剪后的 32 字节取走。
    uint8_t plaintext[APP_VAULT_WRAP_LEN];
    size_t len = 0;
    bool ok = app_aes256_cbc_decrypt(kek, iv, wrapped, APP_VAULT_WRAP_LEN,
                                     plaintext, sizeof(plaintext), &len);
    // 只有"解出正好 32 字节"才算封装可信；长度不符说明 KEK 不对或填充是偶然合法。
    if (ok && len == 32) {
        memcpy(dek, plaintext, 32);
    } else {
        ok = false;
    }
    app_crypto_zero(plaintext, sizeof(plaintext));
    return ok;
}

// ---------------------------------------------------------------------------
// 恢复码
// ---------------------------------------------------------------------------
// Crockford Base32：去掉 I/L/O/U，避免与 1/0 混淆。
static const char RECOVERY_ALPHABET[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// 128 位 -> 26 个字符（130 位，末位低 2 位补零），按 5 位一组插入 '-'。
static void recovery_encode(const uint8_t secret[16], char out[APP_VAULT_RECOVERY_LEN + 1])
{
    char raw[APP_VAULT_RECOVERY_CHARS + 1];
    uint32_t acc = 0;
    int acc_bits = 0;
    int byte_index = 0;

    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        while (acc_bits < 5) {
            acc = (acc << 8) | (byte_index < 16 ? secret[byte_index] : 0);
            byte_index++;
            acc_bits += 8;
        }
        acc_bits -= 5;
        raw[i] = RECOVERY_ALPHABET[(acc >> acc_bits) & 0x1F];
    }
    raw[APP_VAULT_RECOVERY_CHARS] = '\0';

    int pos = 0;
    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        if (i > 0 && i % 5 == 0) out[pos++] = '-';
        out[pos++] = raw[i];
    }
    out[pos] = '\0';

    app_crypto_zero(raw, sizeof(raw));
}

// 表里的值；非法字符返回 -1。
static int recovery_value(char c)
{
    for (int i = 0; i < 32; i++) {
        if (RECOVERY_ALPHABET[i] == c) return i;
    }
    return -1;
}

// 26 个字符 -> 16 字节。末位多出的 2 位必须是 0，否则说明这不是本设备生成的码。
static bool recovery_decode(const char *normalized, uint8_t secret[16])
{
    memset(secret, 0, 16);
    uint32_t acc = 0;
    int acc_bits = 0;
    int out_index = 0;

    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        int value = recovery_value(normalized[i]);
        if (value < 0) return false;

        acc = (acc << 5) | (uint32_t)value;
        acc_bits += 5;
        if (acc_bits >= 8) {
            acc_bits -= 8;
            if (out_index < 16) secret[out_index++] = (uint8_t)((acc >> acc_bits) & 0xFF);
        }
    }
    if (out_index != 16) return false;
    // 剩余 acc_bits(2) 位是编码时补的零。
    return (acc & ((1u << acc_bits) - 1u)) == 0;
}

bool app_vault_recovery_normalize(const char *code, char *out, size_t cap)
{
    if (!code || !out) return false;
    if (cap < APP_VAULT_RECOVERY_CHARS + 1) return false;

    int written = 0;
    for (const char *p = code; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        // 易混字符折回标准表，用户少写错一位不至于直接失败：
        // I/L 与 1、O 与 0 形状接近；U 在 Crockford 表里被剔除，读成 V 更合理。
        if (c == 'I' || c == 'L') c = '1';
        else if (c == 'O') c = '0';
        else if (c == 'U') c = 'V';

        if (recovery_value(c) < 0) return false;
        if (written >= APP_VAULT_RECOVERY_CHARS) return false;
        out[written++] = c;
    }
    if (written != APP_VAULT_RECOVERY_CHARS) return false;
    out[written] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// 容器组装
// ---------------------------------------------------------------------------
typedef struct {
    size_t             len;
    app_vault_status_t status;
} assemble_result_t;

// 组装容器（必要时重新加密正文并重算 MAC）。
//   enc_key/mac_key 非空：用 DEK 派生的子密钥刷新正文与 MAC（要求 has_dek）。
//   verify_mac 为 true：只校验 MAC，不把 MAC 写进输出（解锁路径用）。
// 返回的 len 为 0 时 status 一定不是 OK。
static assemble_result_t assemble(app_vault_t *v, const uint8_t *enc_key,
                                  const uint8_t *mac_key, bool verify_mac,
                                  uint8_t *out, size_t cap)
{
    assemble_result_t result = { 0, APP_VAULT_OK };

    if (!v || (!out && cap > 0)) {
        result.status = APP_VAULT_ERR_RANGE;
        return result;
    }

    if (v->mode == APP_VAULT_ENCRYPTED) {
        if (!mac_key) {
            result.status = APP_VAULT_ERR_STATE;
            return result;
        }
        if (v->has_dek && enc_key) {
            // 先取 IV 再写明文：records_build 会就地覆盖 payload（用掉里面缓存的旧密文），
            // 若随机源不可用却在这之后才发现，payload 就只剩明文而 payload_len 仍是旧密文
            // 长度——上锁后原样写回会写出一个 MAC 永远对不上的容器，密码本就此永久打不开。
            // 把所有可能失败的随机数步骤放在覆盖 payload 之前，失败时 payload 保持原样。
            if (!random_bytes(v->iv_payload, sizeof(v->iv_payload))) {
                result.status = APP_VAULT_ERR_ENTROPY;
                return result;
            }
            // 明文记录先写进 payload 缓冲，再原地加密：payload 容量已按"记录上限 + 一整块
            // 填充"预留，因此原地加密不会越界，也省掉一份 3KB 级的临时缓冲。
            size_t records = records_build(v, v->payload, sizeof(v->payload));
            if (records == 0 && v->count > 0) {
                result.status = APP_VAULT_ERR_MEMORY;
                return result;
            }
            size_t cipher_len = app_aes256_cbc_encrypt(enc_key, v->iv_payload, v->payload,
                                                       records, v->payload, sizeof(v->payload));
            if (cipher_len == 0) {
                result.status = APP_VAULT_ERR_MEMORY;
                return result;
            }
            v->payload_len = cipher_len;
        } else if (v->payload_len == 0 || v->payload_len > sizeof(v->payload)) {
            result.status = APP_VAULT_ERR_STATE;
            return result;
        }
    }

    vault_hmac_t mac_ctx;
    vault_hmac_init(&mac_ctx, mac_key, mac_key ? APP_SHA256_LEN : 0);

    writer_t w = { out, cap, 0, false, mac_key ? &mac_ctx : NULL };
    container_write(&w, v);

    if (w.overflow) {
        result.status = APP_VAULT_ERR_MEMORY;
        return result;
    }

    if (mac_key) {
        uint8_t mac[APP_VAULT_MAC_LEN];
        vault_hmac_final(&mac_ctx, mac);

        if (verify_mac) {
            bool same = app_crypto_equal(mac, v->mac, sizeof(mac));
            app_crypto_zero(mac, sizeof(mac));
            if (!same) {
                result.status = APP_VAULT_ERR_AUTH;
                return result;
            }
        } else {
            memcpy(v->mac, mac, sizeof(mac));
            w_raw(&w, mac, sizeof(mac));   // MAC 本身不参与 MAC 计算
            if (w.overflow) {
                app_crypto_zero(mac, sizeof(mac));
                result.status = APP_VAULT_ERR_MEMORY;
                return result;
            }
        }
    }

    result.len = w.pos;
    return result;
}

// 用 DEK 派生的子密钥把当前条目加密进 payload 并重算 MAC。启用加密时必须走一遍，
// 否则容器里只有头、没有密文，MAC 也全是零——一旦在首次保存前上锁，写回的容器会被
// 反序列化判定为损坏，条目全部丢失。
static app_vault_status_t seal_payload(app_vault_t *v)
{
    if (!v->has_dek) return APP_VAULT_ERR_STATE;

    uint8_t enc_key[32];
    uint8_t mac_key[32];
    derive_subkeys(v->dek, enc_key, mac_key);

    assemble_result_t result = assemble(v, enc_key, mac_key, false, NULL, 0);

    app_crypto_zero(enc_key, sizeof(enc_key));
    app_crypto_zero(mac_key, sizeof(mac_key));
    return result.status;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void app_vault_init(app_vault_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->mode = APP_VAULT_PLAIN;
    v->unlocked = true;
    v->selected = 0;
}

// 解析加密容器正文（magic/version/mode/count 已由调用方读出并校验）。失败时不动 v，
// 由 app_vault_deserialize 统一重置，避免这里漏掉一条返回路径就留下半个密码本。
static app_vault_status_t deserialize_encrypted(app_vault_t *v, reader_t *r, size_t len, int count)
{
    if (len < APP_VAULT_ENC_HEADER + APP_VAULT_MAC_LEN) return APP_VAULT_ERR_FORMAT;

    v->mode = APP_VAULT_ENCRYPTED;
    v->count = count;
    v->unlocked = false;
    v->has_dek = false;

    r_raw(r, v->salt_knock, sizeof(v->salt_knock));
    r_raw(r, v->salt_recovery, sizeof(v->salt_recovery));
    v->iterations = r_u32(r);
    r_raw(r, v->iv_wrap_knock, sizeof(v->iv_wrap_knock));
    r_raw(r, v->wrap_knock, sizeof(v->wrap_knock));
    r_raw(r, v->iv_wrap_recovery, sizeof(v->iv_wrap_recovery));
    r_raw(r, v->wrap_recovery, sizeof(v->wrap_recovery));

    uint32_t payload_len = r_u32(r);
    r_raw(r, v->iv_payload, sizeof(v->iv_payload));
    if (r->underflow) return APP_VAULT_ERR_FORMAT;
    if (payload_len == 0 || payload_len > APP_VAULT_PAYLOAD_MAX ||
        payload_len % APP_AES_BLOCK_LEN != 0) {
        return APP_VAULT_ERR_FORMAT;
    }
    if (r->pos + payload_len + APP_VAULT_MAC_LEN != len) return APP_VAULT_ERR_FORMAT;

    // iterations 是设备写入的参数；明显异常的取值按格式错误处理，避免被构造成
    // "0 次迭代"这类弱化设置。
    if (v->iterations < 1 || v->iterations > 1000000) return APP_VAULT_ERR_FORMAT;

    r_raw(r, v->payload, payload_len);
    v->payload_len = payload_len;
    r_raw(r, v->mac, sizeof(v->mac));
    if (r->underflow) return APP_VAULT_ERR_FORMAT;

    return APP_VAULT_OK;
}

app_vault_status_t app_vault_deserialize(app_vault_t *v, const uint8_t *data, size_t len)
{
    if (!v) return APP_VAULT_ERR_RANGE;

    app_vault_init(v);
    if (!data || len < 8) return APP_VAULT_ERR_FORMAT;

    reader_t r = { data, len, 0, false };
    if (r_u32(&r) != VAULT_MAGIC) return APP_VAULT_ERR_FORMAT;
    if (r_u8(&r) != VAULT_VERSION) return APP_VAULT_ERR_FORMAT;

    uint8_t mode = r_u8(&r);
    int count = (int)r_u16(&r);
    if (count > APP_VAULT_MAX) return APP_VAULT_ERR_FORMAT;

    app_vault_status_t status;
    if (mode == APP_VAULT_PLAIN) {
        status = records_parse(v, data + r.pos, len - r.pos, count);
        if (status == APP_VAULT_OK) {
            v->mode = APP_VAULT_PLAIN;
            v->unlocked = true;
            return APP_VAULT_OK;
        }
    } else if (mode == APP_VAULT_ENCRYPTED) {
        status = deserialize_encrypted(v, &r, len, count);
        if (status == APP_VAULT_OK) return APP_VAULT_OK;
    } else {
        status = APP_VAULT_ERR_FORMAT;
    }

    // 头文件契约：解析失败一律恢复初始状态，绝不留下半个可用的密码本。
    app_vault_init(v);
    return status;
}

size_t app_vault_serialize(app_vault_t *v, uint8_t *out, size_t cap)
{
    if (!v) return 0;

    if (v->mode == APP_VAULT_PLAIN) {
        // 明文模式没有 MAC，也不重新派生任何密钥。
        assemble_result_t result = assemble(v, NULL, NULL, false, out, cap);
        return result.status == APP_VAULT_OK ? result.len : 0;
    }

    if (!v->has_dek) {
        // 上锁状态：把手上的封装与密文原样写回，不做任何重算。
        // 用一把"空"MAC 密钥走一遍组装，只是为了复用同一段字段顺序，因此跳过校验。
        assemble_result_t probe = assemble(v, NULL, NULL, false, out, cap);
        if (probe.status == APP_VAULT_ERR_STATE) {
            // 组装要求加密模式必须带 MAC 密钥；这里直接用固定顺序写回。
            writer_t w = { out, cap, 0, false, NULL };
            w_u32(&w, VAULT_MAGIC);
            w_u8(&w, VAULT_VERSION);
            w_u8(&w, (uint8_t)v->mode);
            w_u16(&w, (uint16_t)v->count);
            w_raw(&w, v->salt_knock, sizeof(v->salt_knock));
            w_raw(&w, v->salt_recovery, sizeof(v->salt_recovery));
            w_u32(&w, v->iterations);
            w_raw(&w, v->iv_wrap_knock, sizeof(v->iv_wrap_knock));
            w_raw(&w, v->wrap_knock, sizeof(v->wrap_knock));
            w_raw(&w, v->iv_wrap_recovery, sizeof(v->iv_wrap_recovery));
            w_raw(&w, v->wrap_recovery, sizeof(v->wrap_recovery));
            w_u32(&w, (uint32_t)v->payload_len);
            w_raw(&w, v->iv_payload, sizeof(v->iv_payload));
            w_raw(&w, v->payload, v->payload_len);
            w_raw(&w, v->mac, sizeof(v->mac));
            return w.overflow ? 0 : w.pos;
        }
        return probe.status == APP_VAULT_OK ? probe.len : 0;
    }

    uint8_t enc_key[32];
    uint8_t mac_key[32];
    derive_subkeys(v->dek, enc_key, mac_key);

    assemble_result_t result = assemble(v, enc_key, mac_key, false, out, cap);

    app_crypto_zero(enc_key, sizeof(enc_key));
    app_crypto_zero(mac_key, sizeof(mac_key));
    return result.status == APP_VAULT_OK ? result.len : 0;
}

bool app_vault_is_locked(const app_vault_t *v)
{
    return v && v->mode == APP_VAULT_ENCRYPTED && !v->unlocked;
}

bool app_vault_is_encrypted(const app_vault_t *v)
{
    return v && v->mode == APP_VAULT_ENCRYPTED;
}

// ---------------------------------------------------------------------------
// 条目
// ---------------------------------------------------------------------------
static app_vault_status_t entry_writable(const app_vault_t *v)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (app_vault_is_locked(v)) return APP_VAULT_ERR_LOCKED;
    return APP_VAULT_OK;
}

app_vault_status_t app_vault_add(app_vault_t *v, const char *label, const char *account,
                                 const char *password, int *index_out)
{
    app_vault_status_t status = entry_writable(v);
    if (status != APP_VAULT_OK) return status;
    if (v->count >= APP_VAULT_MAX) return APP_VAULT_ERR_FULL;

    app_vault_entry_t e;
    memset(&e, 0, sizeof(e));
    if (!copy_field(label, e.label, sizeof(e.label), 8) ||
        !copy_field(account, e.account, sizeof(e.account), 16) ||
        !copy_field(password, e.password, sizeof(e.password), 16)) {
        return APP_VAULT_ERR_RANGE;
    }
    e.used = true;

    v->entries[v->count] = e;
    if (index_out) *index_out = v->count;
    v->count++;
    v->selected = v->count - 1;
    return APP_VAULT_OK;
}

app_vault_status_t app_vault_update(app_vault_t *v, int index, const char *label,
                                    const char *account, const char *password)
{
    app_vault_status_t status = entry_writable(v);
    if (status != APP_VAULT_OK) return status;
    if (index < 0 || index >= v->count) return APP_VAULT_ERR_RANGE;

    app_vault_entry_t e = v->entries[index];
    // 空字符串表示"保持原值"，这样界面可以只改其中一项。
    if (label && label[0]) {
        if (!copy_field(label, e.label, sizeof(e.label), 8)) return APP_VAULT_ERR_RANGE;
    }
    if (account && account[0]) {
        if (!copy_field(account, e.account, sizeof(e.account), 16)) return APP_VAULT_ERR_RANGE;
    }
    if (password && password[0]) {
        if (!copy_field(password, e.password, sizeof(e.password), 16)) return APP_VAULT_ERR_RANGE;
    }
    v->entries[index] = e;
    return APP_VAULT_OK;
}

app_vault_status_t app_vault_remove(app_vault_t *v, int index)
{
    app_vault_status_t status = entry_writable(v);
    if (status != APP_VAULT_OK) return status;
    if (index < 0 || index >= v->count) return APP_VAULT_ERR_RANGE;

    for (int i = index; i < v->count - 1; i++) v->entries[i] = v->entries[i + 1];
    v->count--;
    app_crypto_zero(&v->entries[v->count], sizeof(v->entries[0]));

    if (v->selected >= v->count) v->selected = v->count > 0 ? v->count - 1 : 0;
    if (v->selected < 0) v->selected = 0;
    return APP_VAULT_OK;
}

const app_vault_entry_t *app_vault_at(const app_vault_t *v, int index)
{
    if (!v || app_vault_is_locked(v)) return NULL;
    if (index < 0 || index >= v->count) return NULL;
    if (!v->entries[index].used) return NULL;
    return &v->entries[index];
}

int app_vault_selected(const app_vault_t *v)
{
    if (!v || v->count <= 0) return -1;
    if (v->selected < 0 || v->selected >= v->count) return 0;
    return v->selected;
}

void app_vault_select(app_vault_t *v, int index)
{
    if (!v || v->count <= 0) return;
    if (index < 0) index = 0;
    if (index >= v->count) index = v->count - 1;
    v->selected = index;
}

void app_vault_cycle(app_vault_t *v, int delta)
{
    if (!v || v->count <= 0) return;

    int next = app_vault_selected(v) + delta;
    while (next < 0) next += v->count;
    while (next >= v->count) next -= v->count;
    v->selected = next;
}

// ---------------------------------------------------------------------------
// 加密
// ---------------------------------------------------------------------------
// 抹掉全部加密元数据（salt、两份封装、密文、MAC、DEK、恢复码），只保留 mode/unlocked
// 与条目。用于"退回明文"和"启用加密中途失败"两种回滚，避免两处各清一遍漏字段。
static void clear_crypto_state(app_vault_t *v)
{
    app_crypto_zero(v->dek, sizeof(v->dek));
    memset(v->salt_knock, 0, sizeof(v->salt_knock));
    memset(v->salt_recovery, 0, sizeof(v->salt_recovery));
    memset(v->iv_wrap_knock, 0, sizeof(v->iv_wrap_knock));
    memset(v->wrap_knock, 0, sizeof(v->wrap_knock));
    memset(v->iv_wrap_recovery, 0, sizeof(v->iv_wrap_recovery));
    memset(v->wrap_recovery, 0, sizeof(v->wrap_recovery));
    memset(v->iv_payload, 0, sizeof(v->iv_payload));
    memset(v->mac, 0, sizeof(v->mac));
    app_crypto_zero(v->payload, sizeof(v->payload));
    v->payload_len = 0;
    v->iterations = 0;
    memset(v->recovery, 0, sizeof(v->recovery));
    v->has_dek = false;
}

app_vault_status_t app_vault_enable_encryption(app_vault_t *v, const app_vault_knock_t *knock)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (v->mode != APP_VAULT_PLAIN) return APP_VAULT_ERR_STATE;
    if (!app_vault_knock_valid(knock)) return APP_VAULT_ERR_RANGE;

    uint8_t dek[32];
    uint8_t recovery_secret[16];
    uint8_t kek[32];
    if (!random_bytes(dek, sizeof(dek)) || !random_bytes(recovery_secret, sizeof(recovery_secret)) ||
        !random_bytes(v->salt_knock, sizeof(v->salt_knock)) ||
        !random_bytes(v->salt_recovery, sizeof(v->salt_recovery))) {
        app_crypto_zero(dek, sizeof(dek));
        app_crypto_zero(recovery_secret, sizeof(recovery_secret));
        return APP_VAULT_ERR_ENTROPY;
    }

    v->iterations = APP_VAULT_ITERATIONS;

    knock_derive_kek(knock, v->salt_knock, v->iterations, kek);
    bool ok = wrap_dek(dek, kek, v->iv_wrap_knock, v->wrap_knock);
    app_crypto_zero(kek, sizeof(kek));

    if (ok) {
        recovery_derive_kek(recovery_secret, v->salt_recovery, v->iterations, kek);
        ok = wrap_dek(dek, kek, v->iv_wrap_recovery, v->wrap_recovery);
        app_crypto_zero(kek, sizeof(kek));
    }

    if (!ok) {
        app_crypto_zero(dek, sizeof(dek));
        app_crypto_zero(recovery_secret, sizeof(recovery_secret));
        return APP_VAULT_ERR_ENTROPY;
    }

    v->mode = APP_VAULT_ENCRYPTED;
    v->unlocked = true;
    v->has_dek = true;
    memcpy(v->dek, dek, sizeof(v->dek));
    recovery_encode(recovery_secret, v->recovery);

    // 头文件承诺启用时"把当前条目加密写入"。这里必须真的落一份密文 + MAC：否则用户
    // 在首次保存前就上锁，写回的容器只有头、没有密文、MAC 全零，重启后直接被判为损坏。
    app_vault_status_t status = seal_payload(v);
    if (status != APP_VAULT_OK) {
        // 密封失败（随机源中途不可用）：退回明文，条目原样保留，不留下半个加密本。
        clear_crypto_state(v);
        v->mode = APP_VAULT_PLAIN;
        v->unlocked = true;
        v->selected = v->count > 0 ? v->count - 1 : 0;
    }

    app_crypto_zero(dek, sizeof(dek));
    app_crypto_zero(recovery_secret, sizeof(recovery_secret));
    return status;
}

app_vault_status_t app_vault_disable_encryption(app_vault_t *v)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (v->mode != APP_VAULT_ENCRYPTED) return APP_VAULT_ERR_STATE;
    if (!v->unlocked) return APP_VAULT_ERR_LOCKED;

    // 恢复码在明文模式下没有意义，一并清掉。
    clear_crypto_state(v);
    v->mode = APP_VAULT_PLAIN;
    v->unlocked = true;
    return APP_VAULT_OK;
}

// 解密正文用的临时缓冲。app_crypto 的 CBC 解密不支持输入输出重叠：它在本块明文写回
// 之后才从同一位置取前一密文做链接，解到 v->payload 自身会把链值取成明文而整段解错；
// 它同时要求输出缓冲容得下含填充的整段密文。放静态区而不是栈上：上限约 3.4KB，设备端
// 任务栈放不下（与 app_state 把容器缓冲放静态区同理）。
static uint8_t s_payload_plain[APP_VAULT_PAYLOAD_MAX];

// 用派生出的 KEK 尝试解封并校验整本数据。成功则密码本可用。
// iv/wrapped 指定这份 KEK 对应的那一条 DEK 封装：手势解锁用 knock 份，恢复码解锁用
// recovery 份。两份封装各自绑定不同的 salt 与 KEK，混用必然解不出 DEK。
static app_vault_status_t unlock_with_kek(app_vault_t *v, const uint8_t kek[32],
                                          const uint8_t iv[16],
                                          const uint8_t wrapped[APP_VAULT_WRAP_LEN])
{
    uint8_t dek[32];
    if (!unwrap_dek(kek, iv, wrapped, dek)) {
        app_crypto_zero(dek, sizeof(dek));
        return APP_VAULT_ERR_AUTH;
    }

    uint8_t enc_key[32];
    uint8_t mac_key[32];
    derive_subkeys(dek, enc_key, mac_key);

    // MAC 覆盖整段容器（含两份封装），因此换过任何字节都会在这里被发现。
    assemble_result_t verified = assemble(v, NULL, mac_key, true, NULL, 0);
    if (verified.status != APP_VAULT_OK) {
        app_crypto_zero(dek, sizeof(dek));
        app_crypto_zero(enc_key, sizeof(enc_key));
        app_crypto_zero(mac_key, sizeof(mac_key));
        return verified.status;
    }

    // 解到独立缓冲，v->payload 继续保留密文：之后若"解锁后立刻上锁再保存"，锁定态会
    // 原样写回 v->payload，若这里把它改成明文就会写出坏容器。
    size_t plain_len = 0;
    if (!app_aes256_cbc_decrypt(enc_key, v->iv_payload, v->payload, v->payload_len,
                                s_payload_plain, sizeof(s_payload_plain), &plain_len)) {
        app_crypto_zero(s_payload_plain, sizeof(s_payload_plain));
        app_crypto_zero(dek, sizeof(dek));
        app_crypto_zero(enc_key, sizeof(enc_key));
        app_crypto_zero(mac_key, sizeof(mac_key));
        return APP_VAULT_ERR_AUTH;
    }

    app_vault_status_t status;
    if (v->count == 0) {
        // 空本仍要能解锁：没有任何记录，明文为空。
        memset(v->entries, 0, sizeof(v->entries));
        status = plain_len == 0 ? APP_VAULT_OK : APP_VAULT_ERR_FORMAT;
    } else {
        status = records_parse(v, s_payload_plain, plain_len, v->count);
    }
    app_crypto_zero(s_payload_plain, sizeof(s_payload_plain));

    app_crypto_zero(enc_key, sizeof(enc_key));
    app_crypto_zero(mac_key, sizeof(mac_key));

    if (status != APP_VAULT_OK) {
        app_crypto_zero(dek, sizeof(dek));
        return status;
    }

    memcpy(v->dek, dek, sizeof(v->dek));
    app_crypto_zero(dek, sizeof(dek));
    v->has_dek = true;
    v->unlocked = true;
    return APP_VAULT_OK;
}

app_vault_status_t app_vault_unlock_knock(app_vault_t *v, const app_vault_knock_t *knock)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (v->mode != APP_VAULT_ENCRYPTED) return APP_VAULT_ERR_STATE;
    if (!app_vault_knock_valid(knock)) return APP_VAULT_ERR_RANGE;

    // 已经解锁时不重复派生：解锁慢，重复调用会白白卡一次界面。
    if (v->unlocked) return APP_VAULT_OK;

    uint8_t kek[32];
    knock_derive_kek(knock, v->salt_knock, v->iterations, kek);
    app_vault_status_t status = unlock_with_kek(v, kek, v->iv_wrap_knock, v->wrap_knock);
    app_crypto_zero(kek, sizeof(kek));
    return status;
}

app_vault_status_t app_vault_unlock_recovery(app_vault_t *v, const char *code)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (v->mode != APP_VAULT_ENCRYPTED) return APP_VAULT_ERR_STATE;
    if (v->unlocked) return APP_VAULT_OK;

    char normalized[APP_VAULT_RECOVERY_CHARS + 1];
    if (!app_vault_recovery_normalize(code, normalized, sizeof(normalized))) {
        return APP_VAULT_ERR_RANGE;
    }

    uint8_t secret[16];
    if (!recovery_decode(normalized, secret)) {
        app_crypto_zero(secret, sizeof(secret));
        return APP_VAULT_ERR_RANGE;
    }

    uint8_t kek[32];
    recovery_derive_kek(secret, v->salt_recovery, v->iterations, kek);
    app_vault_status_t status =
        unlock_with_kek(v, kek, v->iv_wrap_recovery, v->wrap_recovery);

    app_crypto_zero(secret, sizeof(secret));
    app_crypto_zero(kek, sizeof(kek));
    return status;
}

void app_vault_lock(app_vault_t *v)
{
    if (!v) return;
    if (v->mode != APP_VAULT_ENCRYPTED) {
        // 明文模式本来就没有可上锁的东西，但仍要保证条目可见。
        v->unlocked = true;
        return;
    }

    app_crypto_zero(v->dek, sizeof(v->dek));
    app_crypto_zero(v->entries, sizeof(v->entries));
    memset(v->recovery, 0, sizeof(v->recovery));
    v->has_dek = false;
    v->unlocked = false;
    v->selected = 0;
    v->activity_ms = 0;   // 下次解锁时重新开始计时，避免刚解锁就被判超时
}

// ---------------------------------------------------------------------------
// 安全策略：自动回锁与失败节流
// ---------------------------------------------------------------------------
void app_vault_set_autolock(app_vault_t *v, int seconds)
{
    if (!v) return;
    v->autolock_seconds = seconds > 0 ? seconds : 0;
}

void app_vault_touch(app_vault_t *v, uint32_t now_ms)
{
    if (!v) return;
    v->activity_ms = now_ms;
}

bool app_vault_poll_autolock(app_vault_t *v, uint32_t now_ms)
{
    if (!v || v->mode != APP_VAULT_ENCRYPTED || !v->unlocked) return false;
    if (v->autolock_seconds <= 0 || v->activity_ms == 0) return false;

    // 用无符号差比较，单调时钟回绕时结论依然正确，不需要额外分支。
    uint32_t limit = (uint32_t)v->autolock_seconds * 1000u;
    if ((uint32_t)(now_ms - v->activity_ms) < limit) return false;

    app_vault_lock(v);
    return true;
}

uint32_t app_vault_lockout_remaining_ms(const app_vault_t *v, uint32_t now_ms)
{
    if (!v || v->lockout_until_ms == 0) return 0;
    if ((int32_t)(v->lockout_until_ms - now_ms) <= 0) return 0;
    return v->lockout_until_ms - now_ms;
}

// 记一次失败并进入指数退避。前 APP_VAULT_FAIL_FREE 次只累计不罚等，避免手滑输错就被
// 关在门外；之后每次等待翻倍，上限 APP_VAULT_LOCKOUT_MAX_S。
static void note_unlock_failure(app_vault_t *v, uint32_t now_ms)
{
    if (v->fail_count < 1000) v->fail_count++;

    if (v->fail_count <= APP_VAULT_FAIL_FREE) {
        v->lockout_until_ms = 0;
        return;
    }

    int shift = v->fail_count - APP_VAULT_FAIL_FREE - 1;
    if (shift > 20) shift = 20;                 // 防止移位越界；结果本就会被下面的上限截断
    uint32_t wait_s = 1u << shift;
    if (wait_s > APP_VAULT_LOCKOUT_MAX_S) wait_s = APP_VAULT_LOCKOUT_MAX_S;
    v->lockout_until_ms = now_ms + wait_s * 1000u;
}

static void note_unlock_success(app_vault_t *v, uint32_t now_ms)
{
    v->fail_count = 0;
    v->lockout_until_ms = 0;
    v->activity_ms = now_ms;   // 解锁即一次活动，回锁计时从这里开始
}

app_vault_status_t app_vault_try_unlock_knock(app_vault_t *v, const app_vault_knock_t *knock,
                                              uint32_t now_ms)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    // 冷却中直接返回，连一次 PBKDF2 都不做——否则罚等只是"提示"，暴力尝试仍不受限。
    if (app_vault_lockout_remaining_ms(v, now_ms) > 0) return APP_VAULT_ERR_THROTTLED;

    app_vault_status_t status = app_vault_unlock_knock(v, knock);
    if (status == APP_VAULT_OK) note_unlock_success(v, now_ms);
    else if (status == APP_VAULT_ERR_AUTH) note_unlock_failure(v, now_ms);
    return status;
}

app_vault_status_t app_vault_try_unlock_recovery(app_vault_t *v, const char *code,
                                                 uint32_t now_ms)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (app_vault_lockout_remaining_ms(v, now_ms) > 0) return APP_VAULT_ERR_THROTTLED;

    app_vault_status_t status = app_vault_unlock_recovery(v, code);
    if (status == APP_VAULT_OK) {
        note_unlock_success(v, now_ms);
    } else if (status == APP_VAULT_ERR_AUTH || status == APP_VAULT_ERR_RANGE) {
        // 格式错误也算一次失败：否则可以靠乱填恢复码无限探测而不触发退避。
        note_unlock_failure(v, now_ms);
    }
    return status;
}

app_vault_status_t app_vault_change_knock(app_vault_t *v, const app_vault_knock_t *knock)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (v->mode != APP_VAULT_ENCRYPTED) return APP_VAULT_ERR_STATE;
    if (!v->unlocked || !v->has_dek) return APP_VAULT_ERR_LOCKED;
    if (!app_vault_knock_valid(knock)) return APP_VAULT_ERR_RANGE;

    uint8_t salt[16];
    uint8_t iv[16];
    uint8_t wrapped[APP_VAULT_WRAP_LEN];
    uint8_t kek[32];

    // 先在新 salt 下把封装算完，全部成功后才替换旧值：中途失败不会让密码本变成
    // "手势是新的、封装是旧的"这种打不开的状态。
    if (!random_bytes(salt, sizeof(salt))) return APP_VAULT_ERR_ENTROPY;
    knock_derive_kek(knock, salt, v->iterations, kek);
    bool ok = wrap_dek(v->dek, kek, iv, wrapped);
    app_crypto_zero(kek, sizeof(kek));
    if (!ok) return APP_VAULT_ERR_ENTROPY;

    memcpy(v->salt_knock, salt, sizeof(salt));
    memcpy(v->iv_wrap_knock, iv, sizeof(iv));
    memcpy(v->wrap_knock, wrapped, sizeof(wrapped));
    // 封装字段变了，MAC 必须跟着变，否则上锁后原样写回旧 MAC 就再也解不开了。
    refresh_mac(v);
    return APP_VAULT_OK;
}

app_vault_status_t app_vault_new_recovery_code(app_vault_t *v)
{
    if (!v) return APP_VAULT_ERR_RANGE;
    if (v->mode != APP_VAULT_ENCRYPTED) return APP_VAULT_ERR_STATE;
    if (!v->unlocked || !v->has_dek) return APP_VAULT_ERR_LOCKED;

    uint8_t secret[16];
    uint8_t salt[16];
    uint8_t iv[16];
    uint8_t wrapped[APP_VAULT_WRAP_LEN];
    uint8_t kek[32];

    if (!random_bytes(secret, sizeof(secret)) || !random_bytes(salt, sizeof(salt))) {
        app_crypto_zero(secret, sizeof(secret));
        return APP_VAULT_ERR_ENTROPY;
    }

    recovery_derive_kek(secret, salt, v->iterations, kek);
    bool ok = wrap_dek(v->dek, kek, iv, wrapped);
    app_crypto_zero(kek, sizeof(kek));
    if (!ok) {
        app_crypto_zero(secret, sizeof(secret));
        return APP_VAULT_ERR_ENTROPY;
    }

    memcpy(v->salt_recovery, salt, sizeof(salt));
    memcpy(v->iv_wrap_recovery, iv, sizeof(iv));
    memcpy(v->wrap_recovery, wrapped, sizeof(wrapped));
    recovery_encode(secret, v->recovery);
    // 恢复码封装同样在 MAC 覆盖范围内，换完必须刷新。
    refresh_mac(v);

    app_crypto_zero(secret, sizeof(secret));
    return APP_VAULT_OK;
}

const char *app_vault_pending_recovery(const app_vault_t *v)
{
    if (!v) return "";
    return v->recovery;
}

void app_vault_clear_pending_recovery(app_vault_t *v)
{
    if (!v) return;
    memset(v->recovery, 0, sizeof(v->recovery));
}

// ---------------------------------------------------------------------------
// 敲击手势工具
// ---------------------------------------------------------------------------
bool app_vault_knock_push(app_vault_knock_t *k, int tap)
{
    if (!k) return false;
    if (tap != APP_VAULT_TAP_UP && tap != APP_VAULT_TAP_DOWN) return false;
    if (k->len >= APP_VAULT_KNOCK_MAX) return false;

    k->taps[k->len++] = (uint8_t)tap;
    return true;
}

bool app_vault_knock_pop(app_vault_knock_t *k)
{
    if (!k || k->len <= 0) return false;
    k->taps[--k->len] = 0;
    return true;
}

void app_vault_knock_clear(app_vault_knock_t *k)
{
    if (!k) return;
    memset(k, 0, sizeof(*k));
}

bool app_vault_knock_valid(const app_vault_knock_t *k)
{
    if (!k) return false;
    if (k->len < APP_VAULT_KNOCK_MIN || k->len > APP_VAULT_KNOCK_MAX) return false;

    for (int i = 0; i < k->len; i++) {
        if (k->taps[i] != APP_VAULT_TAP_UP && k->taps[i] != APP_VAULT_TAP_DOWN) return false;
    }
    return true;
}

bool app_vault_knock_same(const app_vault_knock_t *a, const app_vault_knock_t *b)
{
    if (!a || !b) return false;
    if (a->len != b->len) return false;

    // 长度已经公开，逐字节比较的时序差异不构成信息泄漏；用 memcmp 更直观。
    return memcmp(a->taps, b->taps, (size_t)a->len) == 0;
}

const char *app_vault_status_text(app_vault_status_t status)
{
    switch (status) {
    case APP_VAULT_OK:           return "成功";
    case APP_VAULT_ERR_FORMAT:   return "数据已损坏，请重置密码本";
    case APP_VAULT_ERR_LOCKED:   return "请先解锁";
    case APP_VAULT_ERR_AUTH:     return "手势或恢复码不对";
    case APP_VAULT_ERR_STATE:    return "当前不能执行这个操作";
    case APP_VAULT_ERR_FULL:     return "条目已满，先删掉一条";
    case APP_VAULT_ERR_RANGE:    return "输入不合法";
    case APP_VAULT_ERR_MEMORY:   return "空间不足";
    case APP_VAULT_ERR_ENTROPY:  return "设备随机数异常，请重启后再试";
    case APP_VAULT_ERR_THROTTLED:return "试错太多，请稍后再试";
    default:                     return "未知错误";
    }
}

const char *app_vault_mode_name(app_vault_mode_t mode)
{
    switch (mode) {
    case APP_VAULT_PLAIN:     return "明文保存";
    case APP_VAULT_ENCRYPTED: return "加密保护";
    default:                  return "??";
    }
}
