// main/logic/app_channel.c —— Wi-Fi 信道体检纯逻辑实现（详见 app_channel.h）。
#include "app_channel.h"

#include <stdio.h>
#include <string.h>

// 单个 AP 对所在信道的"压强"权重：把 RSSI(-100..-40 dBm) 映射成 0..60，再折半成 0..30。
// 折不半的原因：原先一个贴墙的强 AP 就有 60 分，两个就顶满 100，导致"附近其实没几台
// 路由器，却显示 100 拥挤度"。折半后要 4 个强 AP 才把一条信道压满，100 分才真正代表
// "这条信道没法用了"，读数更符合直觉。
// 用强度加权而不是只数个数，是因为隔壁一个贴墙的强 AP 比远处十个弱 AP 更能把信道压死。
#define CH_WEIGHT_RAW_MAX 60
#define CH_SCORE_CAP      100

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

static int ap_weight(int rssi)
{
    int w = rssi + 100;              // -100..-40 dBm -> 0..60
    if (w < 0) w = 0;
    if (w > CH_WEIGHT_RAW_MAX) w = CH_WEIGHT_RAW_MAX;
    return w / 2;                    // -> 0..30
}

static void copy_ssid(char *dst, const char *ssid, int ssid_len)
{
    dst[0] = '\0';
    if (!ssid || ssid_len <= 0) return;
    size_t n = (size_t)ssid_len;
    if (n > APP_CHANNEL_SSID_LEN - 1) n = APP_CHANNEL_SSID_LEN - 1;
    memcpy(dst, ssid, n);
    dst[n] = '\0';
}

void app_channel_reset(app_channel_report_t *r)
{
    if (!r) return;
    for (int i = 0; i < APP_CHANNEL_COUNT; i++) {
        r->ch[i].aps = 0;
        r->ch[i].strongest = (int8_t)APP_CHANNEL_NO_RSSI;
        r->ch[i].score = 0;
        r->order[i] = i;
    }
    memset(r->aps, 0, sizeof(r->aps));
    r->stored = 0;
    r->ap_total = 0;
    r->congestion = 0;
    r->best_channel = 1;
}

void app_channel_add_ap(app_channel_report_t *r, int channel, int rssi,
                        const char *ssid, int ssid_len, const uint8_t bssid[6])
{
    if (!r || !channel_valid(channel)) return;

    app_channel_slot_t *s = &r->ch[slot_index(channel)];
    s->aps++;
    if (rssi > s->strongest) {
        int strong = rssi;
        if (strong > 127) strong = 127;
        if (strong < -128) strong = -128;
        s->strongest = (int8_t)strong;
    }

    // 每个 AP 按自身强度加权累加，finish() 再统一封顶到 100。
    s->score += ap_weight(rssi);
    r->ap_total++;

    if (r->stored >= APP_CHANNEL_MAX_APS) return;   // 统计照算，只是不再单独列出
    app_channel_ap_t *ap = &r->aps[r->stored++];
    memset(ap, 0, sizeof(*ap));
    if (bssid) memcpy(ap->bssid, bssid, 6);
    copy_ssid(ap->ssid, ssid, ssid_len);
    ap->hidden = (ap->ssid[0] == '\0');
    int clamped = rssi;
    if (clamped > 127) clamped = 127;
    if (clamped < -128) clamped = -128;
    ap->rssi = (int8_t)clamped;
    ap->channel = (uint8_t)channel;
}

// 把 13 条信道按拥挤度从低到高排序；同分时信道号小的在前。插入排序，
// 13 个元素，简单直接。排完 order[0] 就是"最空的一条信道"。
static void sort_order(app_channel_report_t *r)
{
    for (int i = 1; i < APP_CHANNEL_COUNT; i++) {
        int key = r->order[i];
        int key_score = r->ch[key].score;
        int j = i - 1;
        while (j >= 0) {
            int cur = r->order[j];
            int cur_score = r->ch[cur].score;
            if (cur_score < key_score) break;
            if (cur_score == key_score && cur <= key) break;
            r->order[j + 1] = r->order[j];
            j--;
        }
        r->order[j + 1] = key;
    }
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

    sort_order(r);
}

int app_channel_score(const app_channel_report_t *r, int channel)
{
    if (!r || !channel_valid(channel)) return 0;
    return r->ch[slot_index(channel)].score;
}

int app_channel_aps_on(const app_channel_report_t *r, int channel, int *out_idx, int max)
{
    if (!r || !out_idx || max <= 0 || !channel_valid(channel)) return 0;

    int found = 0;
    for (int i = 0; i < r->stored && found < max; i++) {
        if (r->aps[i].channel == (uint8_t)channel) out_idx[found++] = i;
    }
    // 按信号强度从强到弱排序：用户最关心"哪台离我最近"。
    for (int i = 1; i < found; i++) {
        int key = out_idx[i];
        int j = i - 1;
        while (j >= 0 && r->aps[out_idx[j]].rssi < r->aps[key].rssi) {
            out_idx[j + 1] = out_idx[j];
            j--;
        }
        out_idx[j + 1] = key;
    }
    return found;
}

int app_channel_order_channel(const app_channel_report_t *r, int pos)
{
    if (!r || pos < 0 || pos >= APP_CHANNEL_COUNT) return 0;
    return r->order[pos] + APP_CHANNEL_MIN;
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
