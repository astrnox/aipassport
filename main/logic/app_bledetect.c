// main/logic/app_bledetect.c —— BLE 被动检测（防御/侦测）纯逻辑实现（详见 app_bledetect.h）。
//
// 移植自 GhostESP 的 detect_ble_spam_callback / airtag_scanner_callback，但只做"被动监听 +
// 分类 + 给结论"，绝不发射。所有判定只依赖 feed 进来的广播内容与时间戳，因此可在主机上
// 用确定性输入复现——本文件不引用任何 ESP-IDF / LVGL，也不调用 esp_random。
#include "app_bledetect.h"

#include <string.h>

// Apple 厂商 ID（小端）：蓝牙 SIG 公开分配，所有要识别的载荷都以它开头。
#define BLEDETECT_CO_APPLE 0x004C
// Find My 网络服务 UUID（Apple），出现在广播的服务列表里时几乎可以认定是 Find My 设备。
#define BLEDETECT_UUID_FINDMY 0xFD5A

// 一条观察记录：一台广播源在滑动窗口内的活动。
typedef struct {
    uint8_t  addr[6];
    uint64_t last_ms;     // 最近一次出现的时间
    bool     used;
    bool     spam;        // 匹配 Apple 连续广播轰炸前缀
    bool     airtag;      // 匹配 AirTag / Find My
    bool     other;       // 其它 / 未知广播源
} obs_t;

static struct {
    obs_t    obs[APP_BLEDETECT_OBS_MAX];
    uint64_t now_ms;      // 最近一次 feed 的时间戳，用于 report 时的窗口淘汰
} s;

void app_bledetect_reset(void)
{
    memset(&s, 0, sizeof(s));
}

// 把 [0, WINDOW_MS) 之外、或从未出现过的观察项标记为空闲。
static void prune_locked(uint64_t now)
{
    for (int i = 0; i < APP_BLEDETECT_OBS_MAX; i++) {
        if (!s.obs[i].used) continue;
        // now 单调非递减；地址可能一直在线，last_ms 会被持续刷新，不会误淘汰。
        if (now >= s.obs[i].last_ms &&
            now - s.obs[i].last_ms > (uint64_t)APP_BLEDETECT_WINDOW_MS) {
            s.obs[i].used = false;
        }
    }
}

// 厂商数据（含 2 字节小端厂商 ID 在前）是否匹配已知 Apple 连续广播轰炸前缀：
//   A: 4C 00 07 19 07 …   （对应空中载荷 1e ff 4c 00 07 19 07）
//   B: 4C 00 04 04 2a 00 …（对应空中载荷 16 ff 4c 00 04 04 2a 00）
static bool match_apple_continuity_spam(const uint8_t *m, uint8_t len)
{
    if (!m) return false;
    if (len >= 5 &&
        m[0] == 0x4C && m[1] == 0x00 &&
        m[2] == 0x07 && m[3] == 0x19 && m[4] == 0x07) {
        return true;
    }
    if (len >= 6 &&
        m[0] == 0x4C && m[1] == 0x00 &&
        m[2] == 0x04 && m[3] == 0x04 && m[4] == 0x2A && m[5] == 0x00) {
        return true;
    }
    return false;
}

// 厂商数据 / 服务 UUID 是否指向 AirTag / Find My：
//   - 厂商数据匹配 Find My 服务数据 12 19，或 07 19 配对模式（都以 Apple 厂商 ID 开头）；
//   - 广播了 Find My 网络服务 UUID（0xFD5A）。
// 注意 07 19 与轰炸前缀 A（07 19 07）前两个字节重合，两类标志会同时置位——由 verdict 优先级
// 与样例列表的展示优先级分别处理。
static bool match_airtag(const uint8_t *m, uint8_t len,
                         const uint16_t *svc16, int svc16_count)
{
    if (m && len >= 4 &&
        m[0] == 0x4C && m[1] == 0x00 &&
        ((m[2] == 0x12 && m[3] == 0x19) ||
         (m[2] == 0x07 && m[3] == 0x19))) {
        return true;
    }
    if (svc16 && svc16_count > 0) {
        for (int i = 0; i < svc16_count; i++) {
            if (svc16[i] == BLEDETECT_UUID_FINDMY) return true;
        }
    }
    return false;
}

void app_bledetect_feed(const app_bledetect_ad_t *ad)
{
    if (!ad) return;
    if (ad->now_ms > s.now_ms) s.now_ms = ad->now_ms;  // 跟踪最新时间戳，供 report 淘汰

    prune_locked(s.now_ms);

    bool spam   = match_apple_continuity_spam(ad->mfg_data, ad->mfg_data_len);
    // 轰炸优先：命中轰炸前缀（07 19 07 / 04 04 2A 00）的源不再计为 AirTag。07 19 既是 AirTag
    // 配对模式又是轰炸前缀 A 的前缀，若不互斥，同一条会被同时计入两类，导致样例分类与计数重复。
    bool airtag = !spam && match_airtag(ad->mfg_data, ad->mfg_data_len, ad->svc16, ad->svc16_count);
    // 既非轰炸也非 AirTag（含无厂商数据的普通广播源）归入"其它/未知"。
    bool other  = !spam && !airtag;

    // 没有地址就无从去重与追踪，直接丢弃（界面与测试都会提供地址）。
    if (!ad->addr) return;

    // 找已有记录：同一地址只刷新时间并合并类别标记。
    int idx = -1;
    for (int i = 0; i < APP_BLEDETECT_OBS_MAX; i++) {
        if (s.obs[i].used && memcmp(s.obs[i].addr, ad->addr, 6) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        // 找空闲槽；没有就淘汰 last_ms 最旧的一台（它已最久没出现）。
        int free = -1;
        for (int i = 0; i < APP_BLEDETECT_OBS_MAX; i++) {
            if (!s.obs[i].used) { free = i; break; }
        }
        if (free < 0) {
            uint64_t oldest = s.now_ms;
            for (int i = 0; i < APP_BLEDETECT_OBS_MAX; i++) {
                if (s.obs[i].last_ms < oldest) { oldest = s.obs[i].last_ms; free = i; }
            }
        }
        if (free < 0) return;   // 极端情况下（全满且时间戳无法比较），丢弃本帧
        idx = free;
        memset(&s.obs[idx], 0, sizeof(s.obs[idx]));
        memcpy(s.obs[idx].addr, ad->addr, 6);
        s.obs[idx].used = true;
    }

    s.obs[idx].last_ms = s.now_ms;
    if (spam)   s.obs[idx].spam = true;
    if (airtag) s.obs[idx].airtag = true;
    if (other)  s.obs[idx].other = true;
}

// 把地址填入样例数组（按 kind 优先级收集，最多 EG_MAX 条）。
static void push_example(app_bledetect_report_t *r, const uint8_t addr[6],
                         app_bledetect_kind_t kind)
{
    if (r->example_count >= APP_BLEDETECT_EG_MAX) return;
    int i = r->example_count++;
    memcpy(r->eg_addr[i], addr, 6);
    r->eg_kind[i] = (uint8_t)kind;
}

void app_bledetect_report(app_bledetect_report_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    prune_locked(s.now_ms);

    int total = 0, spam = 0, airtag = 0, other = 0;
    for (int i = 0; i < APP_BLEDETECT_OBS_MAX; i++) {
        if (!s.obs[i].used) continue;
        total++;
        if (s.obs[i].spam)   spam++;
        if (s.obs[i].airtag) airtag++;
        if (s.obs[i].other)  other++;
    }
    out->total_advertisers     = total;
    out->apple_continuity_spam = spam;
    out->airtag                = airtag;
    out->other                 = other;

    // 主导结论：轰炸优先于 AirTag（都被刷屏时先告诉用户最紧迫的）。
    if (spam >= APP_BLEDETECT_SPAM_THRESHOLD) {
        out->verdict = APP_BLEDETECT_SPAM;
    } else if (airtag > 0) {
        out->verdict = APP_BLEDETECT_AIRTAG;
    } else {
        out->verdict = APP_BLEDETECT_NORMAL;
    }

    // 样例列表：先 AirTag，再轰炸源，最后其它（都在窗口内），最多 EG_MAX 条。
    for (int i = 0; i < APP_BLEDETECT_OBS_MAX && out->example_count < APP_BLEDETECT_EG_MAX; i++) {
        if (s.obs[i].used && s.obs[i].airtag) {
            push_example(out, s.obs[i].addr, APP_BLEDETECT_KIND_AIRTAG);
        }
    }
    for (int i = 0; i < APP_BLEDETECT_OBS_MAX && out->example_count < APP_BLEDETECT_EG_MAX; i++) {
        if (s.obs[i].used && s.obs[i].spam && !s.obs[i].airtag) {
            push_example(out, s.obs[i].addr, APP_BLEDETECT_KIND_SPAM);
        }
    }
    for (int i = 0; i < APP_BLEDETECT_OBS_MAX && out->example_count < APP_BLEDETECT_EG_MAX; i++) {
        if (s.obs[i].used && s.obs[i].other) {
            push_example(out, s.obs[i].addr, APP_BLEDETECT_KIND_OTHER);
        }
    }
}

const char *app_bledetect_verdict_text(app_bledetect_verdict_t v)
{
    switch (v) {
    case APP_BLEDETECT_SPAM:   return "检测到疑似 BLE 广播轰炸";
    case APP_BLEDETECT_AIRTAG: return "发现 AirTag / Find My 设备";
    case APP_BLEDETECT_NORMAL:
    default:                   return "正常";
    }
}

const char *app_bledetect_kind_text(app_bledetect_kind_t k)
{
    switch (k) {
    case APP_BLEDETECT_KIND_SPAM:   return "疑似轰炸";
    case APP_BLEDETECT_KIND_AIRTAG: return "AirTag";
    case APP_BLEDETECT_KIND_OTHER:
    default:                        return "其它";
    }
}
