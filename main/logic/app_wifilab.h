// main/logic/app_wifilab.h —— Wi-Fi 实验（Wi-Fi lab / CTF）纯逻辑层。
//
// 本文件是用户既有 GhostESP 代码的忠实移植：把"给定模式与目标，拼出一包可在空中发射的
// 原始 802.11 字节"做成普通函数，因此整套报文构造可以在主机上验证，不需要真去打扰任何
// 网络。真正的发射由 net/app_net 的"Wi-Fi 实验"角色完成，本文件只负责"算出该发什么"。
//
// 移植来源（用户自有、公开的 GhostESP）：main/managers/wifi_manager.c 的
//   wifi_manager_broadcast_deauth / 反方向的 deauth+disassoc、
//   eapol_logoff_frame_template + eapol_logoff_task、
//   SAE_COMMIT_TEMPLATE + inject_sae_commit_frame、
//   wifi_manager_broadcast_ap（beacon）。
// 字节布局、模板与随机化方式都按原样复刻，没有"改进"任何射程 / 隐蔽 / 选靶逻辑。
//
// 不碰 ESP-IDF / LVGL：只依赖 <stdbool.h> <stddef.h> <stdint.h> <string.h>。
// 随机化全部走确定性 xorshift32（由调用方用 app_wifilab_set_seed 设定种子），不依赖
// esp_random，便于主机复现与单测。
//
// 重要：本模块只构造报文，不决定"对谁发"。是否合法、是否经过授权，由上层界面与用户负责。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi 实验的四种模式。枚举值本身没有顺序含义，仅用于 switch。
typedef enum {
    APP_WIFILAB_DEAUTH = 0,        // 解除认证 / 解除关联洪泛（AP -> 站点 或 STA -> AP）
    APP_WIFILAB_EAPOL_LOGOFF,      // EAPOL Logoff 洪泛（让已关联站点以为自己被踢下线）
    APP_WIFILAB_SAE_FLOOD,         // SAE（WPA3）握手提交帧洪泛
    APP_WIFILAB_BEACON_SPAM,       // 信标帧洪泛（随机 SSID / Rickroll / 扫描到的 AP 列表）
    APP_WIFILAB_MODE_COUNT,
} app_wifilab_mode_t;

// BEACON_SPAM 的三种子模式。
typedef enum {
    APP_WIFILAB_BEACON_RANDOM = 0,   // 随机 SSID + 随机 MAC，逐信道广播
    APP_WIFILAB_BEACON_RICKROLL,     // 用 Rickroll 歌词作为 SSID 逐信道广播
    APP_WIFILAB_BEACON_AP_LIST,      // 把扫描到的 AP 的 SSID 逐信道广播
    APP_WIFILAB_BEACON_COUNT,
} app_wifilab_beacon_t;

// 构造返回值。OK 为 0，负数表示各类失败。
typedef enum {
    APP_WIFILAB_OK = 0,
    APP_WIFILAB_ERR_INVALID_ARG = -1,   // buf / out_len 为 NULL，或必填地址为 NULL
    APP_WIFILAB_ERR_BUF_TOO_SMALL = -2, // cap 不足以容纳整包；此时 *out_len 写出所需长度
} app_wifilab_err_t;

// 与 GhostESP 一致的常量。
#define APP_WIFILAB_RANDOM_SSID_LEN 8      // generate_random_ssid 的默认长度
#define APP_WIFILAB_BEACON_CHANNEL_MIN 1   // 信标洪泛覆盖的信道范围（与 broadcast_ap 一致）
#define APP_WIFILAB_BEACON_CHANNEL_MAX 11

// ---------------------------------------------------------------------------
// 确定性 PRNG 与随机化（不依赖 esp_random，便于主机复现）
// ---------------------------------------------------------------------------
// 设定 PRNG 种子；0 视为"不改动当前状态"（与 BLE 实验的 set_seed 约定一致）。
void app_wifilab_set_seed(uint32_t seed);

// xorshift32：state 须为非零；返回下一个 32 位伪随机值并回写 state。同一 seed 必得同一序列。
uint32_t app_wifilab_xorshift32(uint32_t *state);

// 从模块 PRNG 派生一个随机 MAC（首字节高两位 11，本地管理单播地址），写入 out[6]。
// 与 GhostESP 的 generate_random_mac 一致：esp_fill_random 后 mac[0] &= 0xFE; |= 0x02。
void app_wifilab_random_mac(uint8_t out[6]);

// 从模块 PRNG 派生一个随机 SSID（ASCII 字母数字，长度 len 字节），写入 out（不强制 NUL）。
// 字符集与 GhostESP 的 generate_random_ssid 一致。
void app_wifilab_random_ssid(char *out, size_t len);

// ---------------------------------------------------------------------------
// 报文构造
// ---------------------------------------------------------------------------
// 以下构造函数在 out_len 指向缓冲写入整包，并把实际（或所需）长度写入 *out_len。
// 任一 buf / out_len 为 NULL，或必填地址为 NULL，返回 ERR_INVALID_ARG。
// cap 不足时返回 ERR_BUF_TOO_SMALL，并保证 *out_len 等于该模式所需长度，便于调用方扩容。

// 解除认证帧（AP -> 站点方向：目的=sta，源=ap_bssid，BSSID=ap_bssid）。
app_wifilab_err_t app_wifilab_build_deauth(uint8_t *buf, size_t cap,
                                           const uint8_t bssid[6], const uint8_t sta[6],
                                           uint16_t reason, size_t *out_len);
// 解除关联帧（同方向、同布局，仅帧控制首字节为 0xA0）。
app_wifilab_err_t app_wifilab_build_disassoc(uint8_t *buf, size_t cap,
                                             const uint8_t bssid[6], const uint8_t sta[6],
                                             uint16_t reason, size_t *out_len);
// 解除认证帧的"反向"（站点 -> AP：目的=ap_bssid，源=sta，BSSID=sta），用于针对单站点。
app_wifilab_err_t app_wifilab_build_deauth_rev(uint8_t *buf, size_t cap,
                                              const uint8_t bssid[6], const uint8_t sta[6],
                                              uint16_t reason, size_t *out_len);
// 解除关联帧的反向（同上）。
app_wifilab_err_t app_wifilab_build_disassoc_rev(uint8_t *buf, size_t cap,
                                                const uint8_t bssid[6], const uint8_t sta[6],
                                                uint16_t reason, size_t *out_len);

// EAPOL Logoff 帧（目的=ap_bssid，源=sta_mac，BSSID=ap_bssid），固定 36 字节，无随机化。
app_wifilab_err_t app_wifilab_build_eapol_logoff(uint8_t *buf, size_t cap,
                                                 const uint8_t ap_bssid[6],
                                                 const uint8_t sta_mac[6], size_t *out_len);

// SAE Commit 帧（目的=bssid，源=src_mac，BSSID=bssid）。scalar + element 区域完全随机化，
// 序列号按 frame_counter 低 4 位 + 高 12 位随机（与 GhostESP inject_sae_commit_frame 一致），
// 固定 128 字节。
app_wifilab_err_t app_wifilab_build_sae_commit(uint8_t *buf, size_t cap,
                                               const uint8_t target_bssid[6],
                                               const uint8_t src_mac[6],
                                               uint16_t frame_counter, size_t *out_len);

// 信标帧（beacon）。sub 决定 SSID/MAC 是否随机化：
//   - RANDOM：ssid 为 NULL 或长度为 0 时生成随机 SSID；bssid 为 NULL 时生成随机 MAC；
//   - RICKROLL：由调用方传入 Rickroll 歌词作为 ssid，bssid 为 NULL 时随机化；
//   - AP_LIST：由调用方传入扫描到的 AP 的 ssid / bssid / channel。
// channel 写入 DS Parameter Set IE。长度按 GhostESP 的 38 + ssid_len + 12 + 3 + 13 计算
// （其源码在 supported_rates 处按 12 而非 10 计，会多带 2 字节 0x00，此处复刻同一行为）。
app_wifilab_err_t app_wifilab_build_beacon(uint8_t *buf, size_t cap,
                                          app_wifilab_beacon_t sub,
                                          const char *ssid, size_t ssid_len,
                                          const uint8_t bssid[6], int channel,
                                          size_t *out_len);

// Rickroll 歌词：与 GhostESP 一致，活动集合为前 5 句（循环取模）。
int app_wifilab_rickroll_count(void);
const char *app_wifilab_rickroll_line(int index);
