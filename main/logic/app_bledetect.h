// main/logic/app_bledetect.h —— BLE 被动检测（防御/侦测）纯逻辑层。
//
// 把 GhostESP 的 detect_ble_spam_callback / airtag_scanner_callback 思路移植成"只听不发射"
// 的分类器：输入是一条条观察到的广播（地址、名称、厂商数据/服务数据字节、RSSI、时间戳），
// 输出是当前环境的判定报告。本层不碰 ESP-IDF / LVGL，也不调用 esp_random——全部状态由 feed
// 进来的 (地址, 时间戳) 驱动，因此可以在主机上用确定性输入复现每一个判定。
//
// 这是"被动检测"工具，不是攻击工具：它只读公开广播、做分类并给出人话结论，从不向外发任何
// 一字节。判定都是启发式的，只说明"像什么 / 这附近在有组织地刷屏吗"，不构成身份认定。
//
// 检测的三种结论：
//  1. Apple Continuity 广播轰炸（AppleJuice 一类）：同一窗口内出现大量 Apple 厂商 ID 且厂商
//     数据前缀匹配已知连续广播载荷（1e ff 4c 00 07 19 07 / 16 ff 4c 00 04 04 2a 00）的不同
//     广播源 → 疑似有人在附近刷屏。
//  2. AirTag / Find My 存在：Apple 厂商 ID 且厂商数据匹配 Find My 服务数据 12 19，或 07 19
//     配对模式，或广播了 Find My 网络服务 UUID（0xFD5A）。
//  3. 其它/未知广播源计数：既不属于轰炸也不属于 AirTag 的普通广播源数量。
//
// 关于"速率"：界面从"找设备"的快照里取设备表喂进来，而快照按地址去重、约 800ms 一刷，
// 因此本分类器统计的是"滑动窗口内出现过的不同 Apple 连续广播源数量"而非原始报文数。对
// 随机 MAC 的轰炸来说，每个报文都是新地址，依然会让计数飙升并触发阈值；对少数合法 Apple
// 设备则只是稳定的一两个。阈值取保守值，详见 app_bledetect.c 的常量说明。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 滑动窗口长度（毫秒）：只统计该时间内出现过的广播源。
#define APP_BLEDETECT_WINDOW_MS 5000
// 窗口内"不同 Apple 连续广播源"达到该数量即判为疑似轰炸。8 已明显多于正常环境里同时存在的
// Apple 连续广播设备（AirPods / 手机弹窗等通常 0~3 台），又不会因偶发的几台合法设备误报。
#define APP_BLEDETECT_SPAM_THRESHOLD 8
// 内部观察表容量：同一窗口内最多追踪这么多台（超出只保留最近活动的）。
#define APP_BLEDETECT_OBS_MAX 32
// 报告里附带展示的"样例地址"最大条数（小列表用）。
#define APP_BLEDETECT_EG_MAX 4

// 判定结论。
typedef enum {
    APP_BLEDETECT_NORMAL = 0,   // 正常：没发现轰炸，也没发现 AirTag
    APP_BLEDETECT_SPAM,         // 检测到疑似 BLE 广播轰炸
    APP_BLEDETECT_AIRTAG,       // 发现 AirTag / Find My 设备
} app_bledetect_verdict_t;

// 样例条目里标记该地址属于哪一类。与 verdict 取值错开，便于界面分别着色。
typedef enum {
    APP_BLEDETECT_KIND_OTHER  = 0,
    APP_BLEDETECT_KIND_SPAM   = 1,
    APP_BLEDETECT_KIND_AIRTAG = 2,
} app_bledetect_kind_t;

// 一条观察到的广播。厂商数据 mfg_data 指向 NimBLE 的 fields.mfg_data（含 2 字节小端厂商 ID
// 在前），与 app_finder 设备表里保存的 mfg_data 含义一致。所有指针可为 NULL，对应字段按"无"。
typedef struct {
    const uint8_t *addr;        // 6 字节发送方地址；NULL 表示本次无法定位（尽量提供）
    int      rssi;              // 信号强度 dBm（仅显示用，不影响判定）
    uint64_t now_ms;            // 本机单调毫秒时间戳（用于滑动窗口，必须非递减地喂）
    uint16_t company_id;        // 厂商 ID（小端 16 位）；0 表示本次无厂商数据
    uint8_t  mfg_type;          // 厂商数据里紧跟厂商 ID 之后的第一个字节；0 表示没有
    const uint8_t *mfg_data;    // 厂商数据前导字节（含 2 字节厂商 ID 在前）；NULL 表示没有
    uint8_t  mfg_data_len;      // mfg_data 有效长度
    const uint16_t *svc16;      // 16 位服务 UUID 列表；NULL 表示没有
    int      svc16_count;       // svc16 条数
    const char *name;           // 设备名（UTF-8）；NULL 表示本次无名字
    int      name_len;          // name 字节数
} app_bledetect_ad_t;

// 分类报告。
typedef struct {
    app_bledetect_verdict_t verdict;   // 主导结论（优先级：轰炸 > AirTag > 正常）
    int total_advertisers;            // 窗口内出现过的不同广播源总数
    int apple_continuity_spam;        // 其中匹配 Apple 连续广播轰炸前缀的不同源数量
    int airtag;                       // 其中匹配 AirTag / Find My 的不同源数量
    int other;                        // 其它 / 未知广播源数量

    // 供界面小列表展示的样例地址（优先 AirTag，其次轰炸源，最后其它）。
    uint8_t eg_addr[APP_BLEDETECT_EG_MAX][6];
    uint8_t eg_kind[APP_BLEDETECT_EG_MAX];
    int     example_count;
} app_bledetect_report_t;

// 清空全部内部状态（观察表、计数、时间戳）。每次进入检测页时调用一次，保证从干净状态开始。
void app_bledetect_reset(void);

// 喂入一条观察到的广播。now_ms 必须单调非递减；同一地址重复喂会刷新其窗口内"最后出现时间"
// 并合并类别标记。判定在 report() 时按当前窗口计算。
void app_bledetect_feed(const app_bledetect_ad_t *ad);

// 依据当前滑动窗口计算并写出报告。会先淘汰窗口外的观察项，因此多次调用结果随时间自然回落。
void app_bledetect_report(app_bledetect_report_t *out);

// 主导结论的中文短标签（"正常" / "检测到疑似 BLE 广播轰炸" / "发现 AirTag"）。
const char *app_bledetect_verdict_text(app_bledetect_verdict_t v);
// 样例条目类别的中文短标签（"其它" / "疑似轰炸" / "AirTag"）。
const char *app_bledetect_kind_text(app_bledetect_kind_t k);
