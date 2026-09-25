// main/logic/app_totp.h —— RFC 6238 TOTP 动态口令（完全离线，与硬件无关）。
//
// 本层不依赖 ESP-IDF、LVGL 与网络，可在主机上直接用 cc 编译测试。SHA1/SHA256 与
// HMAC 都在 .c 内自带实现，避免引入 mbedtls 等重量级依赖，也便于离线校验。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_TOTP_MAX_SECRET_BYTES 64
// 账户上限 10：设备端按列表翻页展示，10 个正好覆盖"校园邮箱 + 若干平台"的常见组合，
// 再多的条目在只有三键的设备上翻找反而更慢。
#define APP_TOTP_MAX_ACCOUNTS     10
#define APP_TOTP_ALGO_SHA1   0
#define APP_TOTP_ALGO_SHA256 1

// 单个 TOTP 账户。label 用于界面显示，secret 为 Base32 解码后的原始密钥。
typedef struct {
    char    label[24];                                  // 账户名（UTF-8）
    uint8_t secret[APP_TOTP_MAX_SECRET_BYTES];          // Base32 解码后的原始密钥
    size_t  secret_len;
    int     digits;                                     // 6 或 8
    int     period;                                     // 秒，默认 30
    uint8_t algo;                                       // APP_TOTP_ALGO_*
} app_totp_account_t;

// Base32 解码（RFC 4648，无填充）。忽略空格、'-' 与 '='；非法字符或超出 out_cap 返回 false。
bool app_totp_base32_decode(const char *text, uint8_t *out, size_t out_cap, size_t *out_len);

// 计算 unix_time 时刻的动态口令，写入 out（需 digits + 1 字节）。失败返回 false。
bool app_totp_code(const app_totp_account_t *acct, uint64_t unix_time, char *out, size_t out_cap);

// 当前口令剩余有效秒数 = period - (unix_time % period)。参数非法返回 -1。
int  app_totp_remaining(const app_totp_account_t *acct, uint64_t unix_time);

// 把口令分组显示：6 位 "482 915"，8 位 "4829 1537"。out_cap 需 >= 12。
// 返回写入字节数（不含结尾 NUL），失败返回 -1。
int  app_totp_format(const char *code, char *out, size_t out_cap);

// 解析 otpauth://totp/LABEL?secret=...&digits=6&period=30&algorithm=SHA1。
bool app_totp_parse_uri(const char *uri, app_totp_account_t *out);
