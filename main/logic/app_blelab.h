// main/logic/app_blelab.h —— BLE 实验（BLE advertising lab）纯逻辑层。
//
// 不碰 ESP-IDF / LVGL：把"给定模式与索引，拼出一包可在空中广播的原始字节"当成普通函数，
// 因此整套报文构造可以在主机上验证，不需要真去打扰任何蓝牙设备。真正的发射由 net/app_ble
// 的 advertiser 角色完成，本文件只负责"算出该发什么"。
//
// 四种模式都复刻自公开的工具仓库（见 docs/development/engineering/ble-lab.md 的来源说明）：
//  - Apple audio / setup：EvilAppleJuice-ESP32 的苹果连续弹窗载荷；
//  - Swift Pair：ESP32-SwiftSpam 的 Windows 快速配对载荷；
//  - iBeacon：ESP32-BLEBeaconSpam 的 iBeacon 服务数据载荷。
//
// 重要：本模块只构造载荷，不决定"对谁发"。是否合法、是否经过授权，由上层界面与用户负责。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 四种广播模式。枚举值本身没有顺序含义，仅用于 switch。
typedef enum {
    APP_BLELAB_MODE_APPLE_AUDIO = 0,   // 苹果音频配件连续弹窗（31 字节）
    APP_BLELAB_MODE_APPLE_SETUP,       // 苹果设置/配对连续弹窗（23 字节）
    APP_BLELAB_MODE_SWIFT_PAIR,        // Windows Swift Pair 快速配对（10 字节 + 可选名字）
    APP_BLELAB_MODE_IBEACON,           // iBeacon（27 字节服务数据）
    APP_BLELAB_MODE_COUNT,
} app_blelab_mode_t;

// 构造返回值。OK 为 0，负数表示各类失败。
typedef enum {
    APP_BLELAB_OK = 0,
    APP_BLELAB_ERR_INVALID_ARG = -1,   // buf / out_len 为 NULL，或模式/索引越界
    APP_BLELAB_ERR_BUF_TOO_SMALL = -2, // cap 不足以容纳整包；此时 *out_len 写出所需长度
    APP_BLELAB_ERR_INVALID_MODE = -3,  // 模式不在 APP_BLELAB_MODE_COUNT 之内
} app_blelab_err_t;

// 构造一包原始广播数据，写入 buf（最多 cap 字节），并把实际长度写入 *out_len。
//
// 参数：
//  - mode：上述四种模式之一。
//  - index：苹果模式下选择第几个设备（见 app_blelab_apple_* 访问器）；非苹果模式忽略。
//  - buf / cap：输出缓冲与容量。cap 不足时返回 ERR_BUF_TOO_SMALL，并保证 *out_len 等于
//    该模式所需长度，便于调用方按需扩容。
//  - out_len：成功或"容量不足"时都写出实际/所需长度；为 NULL 时直接返回 ERR_INVALID_ARG。
//
// 苹果设备的 modelId、iBeacon 的 UUID/major/minor/tx、Swift Pair 的 CoD/名字，都通过下方
// setter 提前设定；未设定时使用模块内默认值（见各 setter 注释）。
app_blelab_err_t app_blelab_build_packet(app_blelab_mode_t mode, uint32_t index,
                                         uint8_t *buf, size_t cap, size_t *out_len);

// 该模式一包所需的字节数（不含长度前缀之外的额外内容；Swift Pair 含可选名字时另算）。
size_t app_blelab_packet_size(app_blelab_mode_t mode);

// ---------------------------------------------------------------------------
// 苹果设备表（来自 EvilAppleJuice-ESP32/src/devices.hpp）
// ---------------------------------------------------------------------------
// 返回某苹果模式下已知设备数量。非苹果模式返回 0。index 取 [0, count) 区间。
int app_blelab_apple_count(app_blelab_mode_t mode);
// 读取第 index 台设备的名字与 modelId。越界或传入 NULL 时返回 false，且不写输出。
bool app_blelab_apple_dev(app_blelab_mode_t mode, int index,
                          const char **out_name, uint8_t *out_model_id);

// ---------------------------------------------------------------------------
// iBeacon 身份设定
// ---------------------------------------------------------------------------
// 设定 iBeacon 的 Proximity UUID（16 字节）、major、minor 与测量发射功率（有符号）。
// 任一指针为 NULL 时忽略对应字段；未调用则使用模块默认（全零 UUID、major/minor=0、tx=-59）。
void app_blelab_set_ibeacon(const uint8_t uuid[16], uint16_t major,
                            uint16_t minor, int8_t tx_power);

// ---------------------------------------------------------------------------
// Swift Pair 设定
// ---------------------------------------------------------------------------
// 设定 Class of Device（24 位，小端 3 字节写入载荷）。传 0xFFFFFFFF 表示"用默认"。
void app_blelab_set_swift_cod(uint32_t cod);
// 设定 Swift Pair 广播里附加的 Complete Local Name（最长 19 字节，超长截断）。
// name 为 NULL 或空串表示不附加名字（典型 10 字节载荷）。
void app_blelab_set_swift_name(const char *name);

// ---------------------------------------------------------------------------
// 确定性 PRNG 与地址生成（不依赖 esp_random，便于主机复现）
// ---------------------------------------------------------------------------
// xorshift32：state 须为非零；返回下一个 32 位伪随机值并回写 state。同一 seed 必得同一序列。
uint32_t app_blelab_xorshift32(uint32_t *state);
// 从 state 派生一个随机静态地址：6 字节，首字节"或"上 0xF0（保证是随机静态地址的高两位 11）。
// 调用方应保证 state 非零，否则地址不可预期。
void app_blelab_random_addr(uint32_t *state, uint8_t out[6]);
