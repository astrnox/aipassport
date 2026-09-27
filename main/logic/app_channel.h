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

// 单个热点明细。普通用户想知道"这条信道上是哪几家路由器"，所以除了强度还要留名字。
typedef struct {
    uint8_t bssid[6];                    // 全 0 表示扫描没给 BSSID
    char    ssid[APP_CHANNEL_SSID_LEN];  // 空串 = 隐藏 SSID
    int8_t  rssi;                        // dBm，原始值（不做平滑）
    uint8_t channel;                     // 主信道 1..13
    bool    hidden;                      // 扫描结果里没有带 SSID
} app_channel_ap_t;

// 单个信道的汇总。
typedef struct {
    int    aps;         // 该信道上的 AP 数量
    int8_t strongest;   // 该信道最强的 RSSI（dBm），无 AP 时为 APP_CHANNEL_NO_RSSI
    int    score;       // 拥挤度。add_ap 期间按强度累加，finish 后封顶到 0..100
} app_channel_slot_t;

typedef struct {
    app_channel_slot_t ch[APP_CHANNEL_COUNT];   // 下标 0 对应信道 1
    app_channel_ap_t   aps[APP_CHANNEL_MAX_APS];// 热点明细，按扫描到的先后存放
    int    stored;      // aps 中已存条数（<= APP_CHANNEL_MAX_APS）
    int    ap_total;    // 参与统计的 AP 总数（含未存下明细的）
    int    congestion;  // 0..100 整体拥挤度（1/6/11 三个不重叠信道的平均拥挤度）
    int    best_channel;// 推荐信道，恒为 1/6/11 之一
    // 信道下标（0..12）按拥挤度从低到高排列，同分按信道号升序——即"最空的排最前"。
    // 明细页据此把空信道排在前面，符合用户"想找一条能用的信道"的直觉。
    int    order[APP_CHANNEL_COUNT];
} app_channel_report_t;

// 复位为"刚扫描前"的空报告：各信道无 AP、拥挤度 0，推荐信道默认 1（order 为 1..13）。
void app_channel_reset(app_channel_report_t *r);

// 记一个 AP。信道超出 1..13 直接忽略（5G 的 AP 不在本工具的关注范围）。
// ssid 可为 NULL / ssid_len<=0 表示隐藏 SSID；bssid 可为 NULL（全 0 处理）。
// 明细存满（APP_CHANNEL_MAX_APS）后仍参与拥挤度统计，只是不再单独列出。
void app_channel_add_ap(app_channel_report_t *r, int channel, int rssi,
                        const char *ssid, int ssid_len, const uint8_t bssid[6]);

// 所有 AP 记完后调用：算出各信道拥挤度、整体拥挤度、推荐信道与排序。
void app_channel_finish(app_channel_report_t *r);

// 查某信道的拥挤度；信道越界返回 0。
int app_channel_score(const app_channel_report_t *r, int channel);

// 取出某信道上所有热点的明细下标（指向 r->aps[]），按信号强度从强到弱排序。
// out_idx 至少能放 max 个；返回实际写入条数（0 表示该信道没有热点，或报告为空）。
int app_channel_aps_on(const app_channel_report_t *r, int channel, int *out_idx, int max);

// 按拥挤度排序后的第 pos 位对应的信道号（1..13）；pos 越界返回 0。
int app_channel_order_channel(const app_channel_report_t *r, int pos);

// 拥挤度对应的人话结论（"很通畅" 等）。
const char *app_channel_verdict(int congestion);

// 生成一句给普通用户的建议，如"2.4G 比较挤，建议把路由器改到 6 信道"。
void app_channel_advice(const app_channel_report_t *r, char *out, size_t cap);
