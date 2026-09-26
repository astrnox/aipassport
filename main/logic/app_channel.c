// main/logic/app_channel.c —— Wi-Fi 信道体检纯逻辑实现（详见 app_channel.h）。
#include "app_channel.h"

#include <stdio.h>

// 单个 AP 对所在信道的"压强"权重：-40 dBm 贡献 60，往下线性递减，-100 dBm 贡献 0。
// 用强度加权而不是只数个数，是因为隔壁一个贴墙的强 AP 比远处十个弱 AP 更能把信道压死。
#define CH_WEIGHT_MAX 60
#define CH_SCORE_CAP  100

// 1/6/11 是 2.4G 唯三互不重叠的信道，推荐只在它们之间选。
static const int CH_CANDIDATES[3] = { 1, 6, 11 };

static int slot_index(int channel)
{
    return channel - APP_CHANNEL_MIN;
}

static bool channel_valid(int channel)
{
    return channel >= APP_CHANNEL_MIN && channel <= APP_CHANNEL_MAX;
}

void app_channel_reset(app_channel_report_t *r)
{
    if (!r) return;
    for (int i = 0; i < APP_CHANNEL_COUNT; i++) {
        r->ch[i].aps = 0;
        r->ch[i].strongest = (int8_t)APP_CHANNEL_NO_RSSI;
        r->ch[i].score = 0;
    }
    r->ap_total = 0;
    r->congestion = 0;
    r->best_channel = 1;
}

void app_channel_add_ap(app_channel_report_t *r, int channel, int rssi)
{
    if (!r || !channel_valid(channel)) return;

    app_channel_slot_t *s = &r->ch[slot_index(channel)];
    s->aps++;
    if (rssi > s->strongest) {
        if (rssi > 127) rssi = 127;
        if (rssi < -128) rssi = -128;
        s->strongest = (int8_t)rssi;
    }

    // 每个 AP 按自身强度加权累加，finish() 再统一封顶到 100。
    int w = rssi + 100;
    if (w < 0) w = 0;
    if (w > CH_WEIGHT_MAX) w = CH_WEIGHT_MAX;
    s->score += w;

    r->ap_total++;
}

void app_channel_finish(app_channel_report_t *r)
{
    if (!r) return;

    for (int i = 0; i < APP_CHANNEL_COUNT; i++) {
        int sc = r->ch[i].score;
        if (sc > CH_SCORE_CAP) sc = CH_SCORE_CAP;
        r->ch[i].score = sc;
    }

    // 整体拥挤度 = 三个不重叠信道的平均拥挤度。只取这三条而不是全 13 条，
    // 是因为 2..5、7..10 这些信道的拥挤本质上和它重叠的那条是同一份干扰。
    int sum = 0;
    int best = CH_CANDIDATES[0];
    int best_score = -1;
    for (size_t i = 0; i < sizeof(CH_CANDIDATES) / sizeof(CH_CANDIDATES[0]); i++) {
        int ch = CH_CANDIDATES[i];
        int sc = r->ch[slot_index(ch)].score;
        sum += sc;
        if (best_score < 0 || sc < best_score) {
            best_score = sc;
            best = ch;
        }
    }
    r->congestion = sum / (int)(sizeof(CH_CANDIDATES) / sizeof(CH_CANDIDATES[0]));
    if (r->congestion > CH_SCORE_CAP) r->congestion = CH_SCORE_CAP;
    r->best_channel = best;
}

int app_channel_score(const app_channel_report_t *r, int channel)
{
    if (!r || !channel_valid(channel)) return 0;
    return r->ch[slot_index(channel)].score;
}

const char *app_channel_verdict(int congestion)
{
    if (congestion < 20) return "很通畅";
    if (congestion < 45) return "还行";
    if (congestion < 70) return "比较挤";
    return "非常拥挤";
}

void app_channel_advice(const app_channel_report_t *r, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!r) return;

    if (r->ap_total <= 0) {
        snprintf(out, cap, "附近没扫到 Wi-Fi，可能不在路由器覆盖范围内");
        return;
    }
    if (r->congestion < 20) {
        snprintf(out, cap, "2.4G 很通畅，不需要调整路由器");
        return;
    }
    snprintf(out, cap, "2.4G %s，建议把路由器改到 %d 信道",
             app_channel_verdict(r->congestion), r->best_channel);
}