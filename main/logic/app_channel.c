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
        r->ch[i].weight = 0;
        r->order[i] = i;
    }
    memset(r->aps, 0, sizeof(r->aps));
    r->stored = 0;
    r->ap_total = 0;
    r->congestion = 0;
    r->best_channel = 1;
}

void app_channel_add_ap(app_channel_report_t *r, int channel, int rssi,
                        const char *ssid, int ssid_len, const uint8_t bssid[6],
                        app_channel_sec_t sec)
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

    // 每个 AP 按自身强度加权累加。score 与 weight 同步累加：finish() 把 score 封顶到
    // 100 作为对外读数，weight 不封顶，留给"邻频重叠"计算用，避免封顶丢掉真实份量。
    int w = ap_weight(rssi);
    s->score += w;
    s->weight += w;
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
    ap->sec = sec;
}

// 邻频重叠的"亲近度"分子：同频 5、隔 1 条 4、隔 2 条 3、隔 3 条 2、隔 4 条 1，
// 隔 5 条及以上 0（2.4G 相邻信道带宽本就相互交叠，隔得越远影响越小，5 条以外不相干）。
// 用整数分子除以 5 落在最后一次性做，避免每个 AP 提前取整丢精度，也不引入浮点。
static int overlap_factor(int distance)
{
    if (distance < 0) distance = -distance;
    if (distance >= 5) return 0;
    return 5 - distance;
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
    for (size_t i = 0; i < sizeof(CH_CANDIDATES) / sizeof(CH_CANDIDATES[0]); i++) {
        sum += r->ch[slot_index(CH_CANDIDATES[i])].score;
    }
    r->congestion = sum / (int)(sizeof(CH_CANDIDATES) / sizeof(CH_CANDIDATES[0]));
    if (r->congestion > CH_SCORE_CAP) r->congestion = CH_SCORE_CAP;

    // 推荐信道在 1/6/11 里挑"同频+邻频重叠"最小的那条，而不是只挑同频拥挤度最小的。
    // 只看同频会漏掉一个常见坑：信道 3 上一台贴墙的强路由器并不直接落在 1/6/11 上，
    // 却把 1 和 6 都压出一截邻频干扰；重叠一算进来，推荐就会自然避开这一侧。重叠已
    // 包含同频分量，故不再单独叠加 score；同分时按信道号升序取第一个，保持结果稳定。
    int best = CH_CANDIDATES[0];
    int best_overlap = -1;
    for (size_t i = 0; i < sizeof(CH_CANDIDATES) / sizeof(CH_CANDIDATES[0]); i++) {
        int ch = CH_CANDIDATES[i];
        int ov = app_channel_overlap(r, ch);
        if (best_overlap < 0 || ov < best_overlap) {
            best_overlap = ov;
            best = ch;
        }
    }
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

// 把一批明细下标按 RSSI 从强到弱做插入排序（稳定：同强度保持原顺序）。
static void sort_by_rssi(const app_channel_report_t *r, int *idx, int n)
{
    for (int i = 1; i < n; i++) {
        int key = idx[i];
        int j = i - 1;
        while (j >= 0 && r->aps[idx[j]].rssi < r->aps[key].rssi) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
}

int app_channel_aps_sorted(const app_channel_report_t *r, int *out_idx, int max)
{
    if (!r || !out_idx || max <= 0) return 0;

    int found = 0;
    for (int i = 0; i < r->stored && found < max; i++) out_idx[found++] = i;
    sort_by_rssi(r, out_idx, found);
    return found;
}

int app_channel_overlap(const app_channel_report_t *r, int channel)
{
    if (!r || !channel_valid(channel)) return 0;

    // 累加两侧各 4 条信道（含同频）按距离加权后的份量，最后统一除以 5 换回 score 量纲。
    int total = 0;
    for (int ch = APP_CHANNEL_MIN; ch <= APP_CHANNEL_MAX; ch++) {
        int factor = overlap_factor(ch - channel);
        if (factor == 0) continue;
        total += r->ch[slot_index(ch)].weight * factor;
    }
    return total / 5;
}

const char *app_channel_band_text(void)
{
    return "2.4G";
}

const char *app_channel_sec_text(app_channel_sec_t sec)
{
    switch (sec) {
    case APP_CHANNEL_SEC_OPEN: return "开放";
    case APP_CHANNEL_SEC_WEP:  return "WEP";
    case APP_CHANNEL_SEC_WPA:  return "WPA";
    case APP_CHANNEL_SEC_WPA2: return "WPA2";
    case APP_CHANNEL_SEC_WPA3: return "WPA3";
    default:                   return "未知";
    }
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

void app_channel_env_summary(const app_channel_report_t *r, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!r) return;

    if (r->ap_total <= 0) {
        snprintf(out, cap, "附近没扫到 Wi-Fi，暂时不用动路由器");
        return;
    }

    int best = r->best_channel;

    // 找推荐信道两侧 ±4 里、除同频外份量最重的两条信道：它们才是"邻频干扰的来源"。
    // 用户看到"干扰来自哪条信道"比只看到"改用哪条信道"更容易理解为什么。
    int near1 = 0, near2 = 0;
    int w1 = 0, w2 = 0;
    for (int ch = APP_CHANNEL_MIN; ch <= APP_CHANNEL_MAX; ch++) {
        if (ch == best || overlap_factor(ch - best) == 0) continue;
        int w = r->ch[slot_index(ch)].weight;
        if (w <= 0) continue;
        if (w > w1) {
            w2 = w1; near2 = near1;
            w1 = w;  near1 = ch;
        } else if (w > w2) {
            w2 = w;  near2 = ch;
        }
    }

    if (near1 > 0 && near2 > 0) {
        int lo = near1 < near2 ? near1 : near2;
        int hi = near1 < near2 ? near2 : near1;
        snprintf(out, cap, "邻频干扰主要来自 %d/%d 信道的路由器，建议改到 %d 信道",
                 lo, hi, best);
    } else if (near1 > 0) {
        snprintf(out, cap, "邻频干扰主要来自 %d 信道的路由器，建议改到 %d 信道",
                 near1, best);
    } else if (r->congestion < 20) {
        // 没有可辨识的邻频干扰：附近确实很干净，不必为换而换。
        snprintf(out, cap, "2.4G 很通畅，附近干扰很少，路由器可继续用 %d 信道", best);
    } else {
        // 干扰都挤在推荐信道附近但同频为主，没形成明显的邻频来源。
        snprintf(out, cap, "同频热点较多，建议把路由器改到 %d 信道", best);
    }
}

void app_channel_security_counts(const app_channel_report_t *r,
                                 int *open, int *wep, int *hidden)
{
    // 先清零：无论报告是否有效，输出都应该是可用的确定值，调用方无需预先初始化。
    if (open) *open = 0;
    if (wep) *wep = 0;
    if (hidden) *hidden = 0;
    if (!r) return;

    // 只统计已保存的明细（stored），不遍历 ap_total——后者是"参与拥挤度统计的总数"，
    // 超过 APP_CHANNEL_MAX_APS 的部分没有安全模式明细可数。这是只读统计，不碰射频。
    for (int i = 0; i < r->stored; i++) {
        const app_channel_ap_t *ap = &r->aps[i];
        if (open && ap->sec == APP_CHANNEL_SEC_OPEN) (*open)++;
        if (wep && ap->sec == APP_CHANNEL_SEC_WEP) (*wep)++;
        if (hidden && ap->hidden) (*hidden)++;
    }
}
