// main/logic/app_channel.h —— Wi-Fi 信道体检的纯逻辑：把扫描到的 AP 汇总成每个信道的拥挤度。
//
// 普通用户看不懂"信道 6 上有 7 个 AP、RSSI -52"，只想知道"我家 Wi-Fi 为什么卡、要不要动路由器"。
// 所以这里把一次扫描的原始记录压成一个 0..100 的拥挤度、一个推荐信道和一句人话结论；
// 想深挖的人再看每信道明细。不碰 esp_wifi，输入只是"一条 AP：在哪个信道、多强"。
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

typedef struct {
    int    aps;         // 该信道上的 AP 数量
    int8_t strongest;   // 该信道最强的 RSSI（dBm），无 AP 时为 APP_CHANNEL_NO_RSSI
    int    score;       // 拥挤度。add_ap 期间按强度累加，finish 后封顶到 0..100
} app_channel_slot_t;

typedef struct {
    app_channel_slot_t ch[APP_CHANNEL_COUNT];   // 下标 0 对应信道 1
    int ap_total;      // 参与统计的 AP 总数
    int congestion;    // 0..100 整体拥挤度（1/6/11 三个不重叠信道的平均拥挤度）
    int best_channel;  // 推荐信道，恒为 1/6/11 之一
} app_channel_report_t;

// 复位为"刚扫描前"的空报告：各信道无 AP、拥挤度 0，推荐信道默认 1。
void app_channel_reset(app_channel_report_t *r);

// 记一个 AP。信道超出 1..13 直接忽略（5G 的 AP 不在本工具的关注范围）。
void app_channel_add_ap(app_channel_report_t *r, int channel, int rssi);

// 所有 AP 记完后调用：算出各信道拥挤度、整体拥挤度与推荐信道。
void app_channel_finish(app_channel_report_t *r);

// 查某信道的拥挤度；信道越界返回 0。
int app_channel_score(const app_channel_report_t *r, int channel);

// 拥挤度对应的人话结论（"很通畅" 等）。
const char *app_channel_verdict(int congestion);

// 生成一句给普通用户的建议，如"2.4G 比较挤，建议把路由器改到 6 信道"。
void app_channel_advice(const app_channel_report_t *r, char *out, size_t cap);