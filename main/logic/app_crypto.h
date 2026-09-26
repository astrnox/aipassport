// main/logic/app_crypto.h —— 密码本所需的密码学原语（不依赖 ESP-IDF，可主机测试）。
//
// 只提供密码本真正用到的四种能力：SHA-256、HMAC-SHA256、PBKDF2-HMAC-SHA256 与
// AES-256-CBC。全部自实现，不引入 mbedtls：一是这一层要能在主机上脱离 ESP-IDF 直接
// 编译并对着标准测试向量单测，二是密码本的密钥派生与解密都在按键任务之外的路径上
// 一次完成，性能不是瓶颈。
//
// 安全边界：本文件只做"按标准算法搬运字节"，不负责密钥管理、随机数与任何策略。
// 随机数由调用方注入（设备端 esp_fill_random），密钥的来去由 app_vault 决定。
//
// 分组模式选择：只实现 CBC，不实现 GCM。密码本用"先加密后认证"（encrypt-then-MAC）
// 把 HMAC-SHA256 盖在整段密文上，完整性与真实性由 MAC 保证，因此不需要 AEAD；
// 而 GCM 需要 GF(2^128) 乘法与计数器细节，代码量与出错面都远超收益。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_SHA256_LEN        32
#define APP_AES256_KEY_LEN    32
#define APP_AES256_IV_LEN     16
#define APP_AES_BLOCK_LEN     16
// PKCS#7 最多补一个整块，因此密文最长比明文多 16 字节。
#define APP_AES_CBC_MAX_OVERHEAD APP_AES_BLOCK_LEN

// ---------------------------------------------------------------------------
// SHA-256（FIPS 180-4）
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t h[8];
    uint64_t total;      // 已喂入字节数，用于填充长度
    uint8_t  buf[64];
    size_t   buf_len;
} app_sha256_ctx_t;

void app_sha256_init(app_sha256_ctx_t *ctx);
void app_sha256_update(app_sha256_ctx_t *ctx, const void *data, size_t len);
// 写完摘要后 ctx 不可再复用，需重新 init。
void app_sha256_final(app_sha256_ctx_t *ctx, uint8_t out[APP_SHA256_LEN]);
// 一次性版本，等价于 init + update + final。
void app_sha256(const void *data, size_t len, uint8_t out[APP_SHA256_LEN]);

// ---------------------------------------------------------------------------
// HMAC-SHA256（RFC 2104）。key 任意长度，超长会先压缩。
// ---------------------------------------------------------------------------
void app_hmac_sha256(const uint8_t *key, size_t key_len, const void *msg, size_t msg_len,
                     uint8_t out[APP_SHA256_LEN]);

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256（RFC 8018）。
// iterations 为 0 时按 1 处理；out_len 为 0 时不做任何事。
// 敲击手势这类低熵口令靠 iterations 抬高每次猜测的代价，见 app_vault 的取值说明。
// ---------------------------------------------------------------------------
void app_pbkdf2_hmac_sha256(const uint8_t *pass, size_t pass_len,
                            const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t *out, size_t out_len);

// ---------------------------------------------------------------------------
// AES-256-CBC + PKCS#7
// ---------------------------------------------------------------------------
// 加密。out 需至少 len + APP_AES_CBC_MAX_OVERHEAD 字节。成功返回密文长度（16 的倍数），
// 参数非法或 out_cap 不足返回 0。
size_t app_aes256_cbc_encrypt(const uint8_t key[APP_AES256_KEY_LEN],
                              const uint8_t iv[APP_AES256_IV_LEN],
                              const void *in, size_t len,
                              uint8_t *out, size_t out_cap);

// 解密。in_len 必须是 16 的非零倍数，且填充合法。成功时写入明文长度到 *out_len 并
// 返回 true；长度非法或填充错误返回 false（调用方应把它当作"认证失败"，不要区分原因）。
bool app_aes256_cbc_decrypt(const uint8_t key[APP_AES256_KEY_LEN],
                            const uint8_t iv[APP_AES256_IV_LEN],
                            const void *in, size_t in_len,
                            uint8_t *out, size_t out_cap, size_t *out_len);

// 单块 AES-256 加/解密，供需要自己拼接模式时使用。in/out 各 16 字节。
void app_aes256_encrypt_block(const uint8_t key[APP_AES256_KEY_LEN],
                              const uint8_t in[APP_AES_BLOCK_LEN],
                              uint8_t out[APP_AES_BLOCK_LEN]);
void app_aes256_decrypt_block(const uint8_t key[APP_AES256_KEY_LEN],
                              const uint8_t in[APP_AES_BLOCK_LEN],
                              uint8_t out[APP_AES_BLOCK_LEN]);

// 恒时比较：长度相同且内容一致返回 true。用于校验 MAC 与恢复码，避免按字节提前返回
// 泄漏"前几位猜对了"。
bool app_crypto_equal(const void *a, const void *b, size_t len);

// 把字节串清零。用它而不是 memset，避免编译器把"写完就丢弃的缓冲"优化掉。
void app_crypto_zero(void *buf, size_t len);
