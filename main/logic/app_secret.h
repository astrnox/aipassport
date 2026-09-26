// main/logic/app_secret.h —— 设备绑定的小段敏感数据密封容器（不依赖 ESP-IDF，可主机测试）。
//
// 用途：动态口令（TOTP）密钥这类"必须在设备上还原成明文才能使用、但不应以明文留在
// Flash 里"的数据。写入时加密并附加认证标签，读取时先验签再解密。
//
// 为什么是"设备绑定"而不是口令加密：TOTP 密钥要在每次开机后自动可用（用户不可能每次
// 开机都先敲一遍手势才能看口令），因此密钥不能来自用户口令，只能来自设备自身。本模块
// 把设备唯一标识（eFuse MAC）与一份仅存在于本机 NVS 的随机种子一起喂进 KDF 得到主密钥，
// 于是同一份密文换到另一台设备上会因为 MAC 不同而无法解开。
//
// 安全边界（必须如实告知，不能含糊）：
//   没有安全元件、没有开启 Flash 加密/安全启动时，本方案挡住的是"密文被单独抄走或从
//   备份里被读到"——攻击者拿到 totp 密文却拿不到另一处保存的随机种子，解不开；也挡住
//   "把 NVS 原样烧到另一台机器"，因为 MAC 不同。但它挡不住能完整读取整片 Flash 与
//   eFuse 的攻击者。真正的强保护需要出厂时烧录 Flash 加密密钥并启用安全启动，超出本
//   固件可达范围。
//
// 容器格式（小端，显式按字节读写，不依赖结构体对齐）：
//   magic(4)="PSEC" ver(1)=1 iv(16) plain_len(4) ciphertext(n, 16 的倍数) mac(32)
//   mac = HMAC-SHA256(k_mac, 从 magic 到密文的全部字节)
//   k_enc / k_mac 由主密钥经 HMAC-SHA256 以不同标签派生，二者互不相关。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_SECRET_KEY_LEN    32
#define APP_SECRET_IV_LEN     16
#define APP_SECRET_MAC_LEN    32
#define APP_SECRET_HEADER_LEN 25   // magic(4) + ver(1) + iv(16) + plain_len(4)
// 容器最大开销：头部 + 至多一个 PKCS#7 整块 + MAC。
#define APP_SECRET_OVERHEAD   (APP_SECRET_HEADER_LEN + 16 + APP_SECRET_MAC_LEN)
#define APP_SECRET_BLOB_MAX(plain_len) ((plain_len) + APP_SECRET_OVERHEAD)

// 由设备唯一标识与一份本机随机种子派生主密钥。两者都为空时返回的密钥不具备设备绑定，
// 调用方应避免这种情况（设备端两者都必然存在）。
void app_secret_device_key(const void *binding, size_t binding_len,
                           const void *seed, size_t seed_len,
                           uint8_t out[APP_SECRET_KEY_LEN]);

// 加密并认证。iv 由调用方提供（设备端用硬件 RNG 生成，绝不复用）。
// 返回写入字节数；plain 为空、iv 为 NULL 或 out_cap 不足返回 0。
size_t app_secret_seal(const uint8_t key[APP_SECRET_KEY_LEN],
                       const uint8_t iv[APP_SECRET_IV_LEN],
                       const void *plain, size_t plain_len,
                       uint8_t *out, size_t out_cap);

// 验签并解密。任何一步失败（magic/版本不符、长度自相矛盾、MAC 不匹配、填充错误）都
// 返回 false，不区分原因——区分会让攻击者据此判断"接近猜中"。成功时写 *out_len。
bool app_secret_open(const uint8_t key[APP_SECRET_KEY_LEN],
                     const void *in, size_t in_len,
                     uint8_t *out, size_t out_cap, size_t *out_len);
