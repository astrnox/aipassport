// main/logic/app_channel.h —— Wi-Fi 信道体检的纯逻辑：把扫描到的 AP 汇总成每个信道的拥挤度。
//
// 普通用户看不懂"信道 6 上有 7 个 AP、RSSI -52"，只想知道"我家 Wi-Fi 为什么卡、要不要动路由器"。
// 所以这里把一次扫描的原始记录压成一个 0..100 的拥挤度、一个推荐信道和一句人话结论；
// 想深挖的人再看每信道明细，甚至可以展开某一信道看到底有哪些热点。不碰 esp_wifi，
// 输入只是"一条 AP：在哪个信道、多强、叫什么"。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 2.4 GHz 常用信道 1..13。10..13 在国内可用，一并统计；推荐只在 1/6/11 里选，
// 因为这三个互不重叠，是路由器唯三真正"不打架"的选择。
#define APP_CHANNEL_MIN   1
#define APP_CHANNEL_MAX   13
#define APP_CHANNEL_COUNT (APP_CHANNEL_MAX - APP_CHANNEL_MIN + 1)

// 无 AP 时用这个值表示"没信号"。
#define APP_CHANNEL_NO_RSSI (-127)

// 一次扫描最多保留这么多热点明细。典型宿舍/办公楼 2.4G 也就十几个，48 足够；
// 再多只是把同一批 AP 的重复记录算两次，也白白占内存。
#define APP_CHANNEL_MAX_APS 48

// SSID 上限 32 字节（802.11 规定），加结尾 NUL。
#define APP_CHANNEL_SSID_LEN 33

// 热点安全模式。这里刻意不直接用 ESP-IDF 的 wifi_auth_mode_t：本层是可在主机上
// 单测的纯逻辑，不该拖进 esp_wifi 头文件。由 net 层（app_net）把驱动枚举映射过来，
// 映射关系集中在那一处，界面只认这套短标签。
//
// UNKNOWN 放在 0：报告 reset 时 memset 会把该字段清零，默认值落到"未知"比落到
// "开放"更诚实——没读到安全模式时不该对外宣称这是开放网络。
typedef enum {
    APP_CHANNEL_SEC_UNKNOWN = 0,  // 没读到 / 未识别（含 WAPI 等本工具不细分的模式）
    APP_CHANNEL_SEC_OPEN,         // 开放（无密码）
    APP_CHANNEL_SEC_WEP,          // 老式 WEP
    APP_CHANNEL_SEC_WPA,          // WPA
    APP_CHANNEL_SEC_WPA2,         // WPA2
    APP_CHANNEL_SEC_WPA3,         // WPA3
} app_channel_sec_t;

// 单个热点明细。普通用户想知道"这条信道上是哪几家路由器"，所以除了强度还要留名字。
typedef struct {
    uint8_t bssid[6];                    // 全 0 表示扫描没给 BSSID
    char    ssid[APP_CHANNEL_SSID_LEN];  // 空串 = 隐藏 SSID
    int8_t  rssi;                        // dBm，原始值（不做平滑）
    uint8_t channel;                     // 主信道 1..13
    bool    hidden;                      // 扫描结果里没有带 SSID
    app_channel_sec_t sec;               // 安全模式（由 net 层映射后传入）
} app_channel_ap_t;

// 单个信道的汇总。
typedef struct {
    int    aps;         // 该信道上的 AP 数量
    int8_t strongest;   // 该信道最强的 RSSI（dBm），无 AP 时为 APP_CHANNEL_NO_RSSI
    int    score;       // 拥挤度。add_ap 期间按强度累加，finish 后封顶到 0..100
    // 同频强度原始累加（未封顶，等于 finish 之前的 score）。保留它是为了让"邻频重叠"
    // 计算不被 score 的封顶截断：几条强 AP 把 score 顶到 100 后，重叠计算仍需知道这条
    // 信道到底占了多少份量。
    int    weight;
} app_channel_slot_t;

typedef struct {
    app_channel_slot_t ch[APP_CHANNEL_COUNT];   // 下标 0 对应信道 1
    app_channel_ap_t   aps[APP_CHANNEL_MAX_APS];// 热点明细，按扫描到的先后存放
    int    stored;      // aps 中已存条数（<= APP_CHANNEL_MAX_APS）
    int    ap_total;    // 参与统计的 AP 总数（含未存下明细的）
    int    congestion;  // 0..100 整体拥挤度（1/6/11 三个不重叠信道的平均拥挤度）
    // 推荐信道，恒为 1/6/11 之一：取"同频+邻频重叠"最小的候选，重叠相同时取信道号小的。
    int    best_channel;
    // 信道下标（0..12）按拥挤度从低到高排列，同分按信道号升序——即"最空的排最前"。
    // 明细页据此把空信道排在前面，符合用户"想找一条能用的信道"的直觉。
    int    order[APP_CHANNEL_COUNT];
} app_channel_report_t;

// 复位为"刚扫描前"的空报告：各信道无 AP、拥挤度 0，推荐信道默认 1（order 为 1..13）。
void app_channel_reset(app_channel_report_t *r);

// 记一个 AP。信道超出 1..13 直接忽略（5G 的 AP 不在本工具的关注范围）。
// ssid 可为 NULL / ssid_len<=0 表示隐藏 SSID；bssid 可为 NULL（全 0 处理）。
// sec 由 net 层从驱动的 authmode 映射而来，界面据此显示 "WPA2" 之类的短标签。
// 明细存满（APP_CHANNEL_MAX_APS）后仍参与拥挤度统计，只是不再单独列出。
void app_channel_add_ap(app_channel_report_t *r, int channel, int rssi,
                        const char *ssid, int ssid_len, const uint8_t bssid[6],
                        app_channel_sec_t sec);

// 所有 AP 记完后调用：算出各信道拥挤度、整体拥挤度、推荐信道与排序。
void app_channel_finish(app_channel_report_t *r);

// 查某信道的拥挤度；信道越界返回 0。
int app_channel_score(const app_channel_report_t *r, int channel);

// 取出某信道上所有热点的明细下标（指向 r->aps[]），按信号强度从强到弱排序。
// out_idx 至少能放 max 个；返回实际写入条数（0 表示该信道没有热点，或报告为空）。
int app_channel_aps_on(const app_channel_report_t *r, int channel, int *out_idx, int max);

// 取出全部热点明细下标（指向 r->aps[]），按信号强度从强到弱排序（同强度保持登记顺序）。
// out_idx 至少能放 max 个；返回实际写入条数（<= r->stored，上限 max）。
// 供"热点总览"一次看完本次扫描到的所有 AP；与"逐信道"不同，它不按信道过滤。
int app_channel_aps_sorted(const app_channel_report_t *r, int *out_idx, int max);

// 某信道的"同频 + 邻频"总重叠强度。2.4G 上信道 n 会与 n±1..±4 互相干扰（带宽相互交叠），
// n±5 起完全不相干；因此把两侧各 4 条信道的强度按距离加权计入：同频 5/5、隔 1 条 4/5、
// 隔 2 条 3/5、隔 3 条 2/5、隔 4 条 1/5，最后除以 5 换回与 score 同一量纲（同一种
// ap_weight 累加出的"份量"）。注意：它把 9 条信道的份量加在一起，取值可以超过 100，
// 不做封顶——调用方只用来做候选间的相对比较，越大越挤。信道越界返回 0。
int app_channel_overlap(const app_channel_report_t *r, int channel);

// 本设备只扫 2.4GHz（ESP32-C3 单射频，硬件上就没有 5GHz 接收链路），文档里的
// "band" 在这台机器上恒为 2.4G。界面统一用这个字符串呈现频段，避免各页硬编码，
// 也避免让用户误以为能扫 5G。返回常量字符串，不需要释放。
const char *app_channel_band_text(void);

// 安全模式的中文短标签："开放""WEP""WPA""WPA2""WPA3""未知"。越界值一律返回"未知"。
const char *app_channel_sec_text(app_channel_sec_t sec);

// 按拥挤度排序后的第 pos 位对应的信道号（1..13）；pos 越界返回 0。
int app_channel_order_channel(const app_channel_report_t *r, int pos);

// 拥挤度对应的人话结论（"很通畅" 等）。
const char *app_channel_verdict(int congestion);

// 生成一句给普通用户的建议，如"2.4G 比较挤，建议把路由器改到 6 信道"。
void app_channel_advice(const app_channel_report_t *r, char *out, size_t cap);

// 生成一句"环境报告"级的大白话结论，比 advice 更具体地点出干扰来源，如
// "邻频干扰主要来自 3 信道的路由器，建议改到 1 信道"。普通用户据此能理解"为什么换"。
// r 为 NULL 时写空串；报告里一个 AP 都没有时也给一句可读结论，不返回空串。
void app_channel_env_summary(const app_channel_report_t *r, char *out, size_t cap);
