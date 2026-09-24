// main/logic/app_esports.c —— 赛事优先级、排序与关注战队的实现（与硬件无关）。
#include "app_esports.h"

#include <string.h>

// 大小写不敏感的字符串比较，用于解析 Riot 返回的状态文本。
static bool text_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int app_esport_priority(const app_esport_match_t *m, int now_utc)
{
    if (!m) return 4;

    if (m->state == APP_MATCH_LIVE) return 0;

    if (m->state == APP_MATCH_UPCOMING) {
        int delta = m->start_utc - now_utc;
        bool followed = m->follow_a || m->follow_b;
        if (followed && delta >= 0 && delta <= 30 * 60) return 1;   // 30 分钟内且有关注战队
        if (delta >= 0 && delta < 24 * 3600) return 2;             // 未来 24 小时内
        return 4;
    }

    if (m->state == APP_MATCH_FINISHED) {
        int since = now_utc - m->start_utc;
        if (since >= 0 && since <= 24 * 3600) return 3;            // 最近 24 小时内结束
        return 4;
    }

    return 4;
}

void app_esport_sort(app_esport_match_t *matches, int count, int now_utc)
{
    if (!matches) return;
    // 稳定插入排序：相同优先级保持输入顺序。
    for (int i = 1; i < count; i++) {
        app_esport_match_t key = matches[i];
        int key_prio = app_esport_priority(&key, now_utc);
        int j = i - 1;
        while (j >= 0 && app_esport_priority(&matches[j], now_utc) > key_prio) {
            matches[j + 1] = matches[j];
            j--;
        }
        matches[j + 1] = key;
    }
}

int app_esport_home_pick(const app_esport_match_t *matches, int count, int now_utc)
{
    if (!matches || count <= 0) return -1;

    // 1) 有直播优先展示第一场直播。
    for (int i = 0; i < count; i++) {
        if (matches[i].state == APP_MATCH_LIVE) return i;
    }

    // 2) 关注的战队在 30 分钟内开赛，取最早的一场。
    int best = -1;
    for (int i = 0; i < count; i++) {
        const app_esport_match_t *m = &matches[i];
        if (m->state != APP_MATCH_UPCOMING) continue;
        if (!(m->follow_a || m->follow_b)) continue;
        int delta = m->start_utc - now_utc;
        if (delta < 0 || delta > 30 * 60) continue;
        if (best < 0 || m->start_utc < matches[best].start_utc) best = i;
    }
    if (best >= 0) return best;

    // 3) 未来 24 小时内最早开赛的一场。
    for (int i = 0; i < count; i++) {
        const app_esport_match_t *m = &matches[i];
        if (m->state != APP_MATCH_UPCOMING) continue;
        int delta = m->start_utc - now_utc;
        if (delta < 0 || delta >= 24 * 3600) continue;
        if (best < 0 || m->start_utc < matches[best].start_utc) best = i;
    }
    return best;
}

bool app_esport_follow_set(app_esport_cache_t *c, const char *team, bool on)
{
    if (!c || !team || team[0] == '\0') return false;

    int found = -1;
    for (int i = 0; i < c->follow_count; i++) {
        if (strcmp(c->followed[i], team) == 0) {
            found = i;
            break;
        }
    }

    if (on) {
        if (found >= 0) return true;                       // 已关注，幂等
        if (c->follow_count >= APP_ESPORT_MAX_FOLLOWS) return false;
        strncpy(c->followed[c->follow_count], team, sizeof(c->followed[0]) - 1);
        c->followed[c->follow_count][sizeof(c->followed[0]) - 1] = '\0';
        c->follow_count++;
        return true;
    }

    if (found < 0) return true;                            // 本就不在列表里，目标状态已达成
    for (int i = found; i < c->follow_count - 1; i++) {
        memcpy(c->followed[i], c->followed[i + 1], sizeof(c->followed[0]));
    }
    c->follow_count--;
    memset(c->followed[c->follow_count], 0, sizeof(c->followed[0]));
    return true;
}

bool app_esport_is_followed(const app_esport_cache_t *c, const char *team)
{
    if (!c || !team || team[0] == '\0') return false;
    for (int i = 0; i < c->follow_count; i++) {
        if (strcmp(c->followed[i], team) == 0) return true;
    }
    return false;
}

void app_esport_apply_follows(app_esport_cache_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->match_count; i++) {
        c->matches[i].follow_a = app_esport_is_followed(c, c->matches[i].team_a);
        c->matches[i].follow_b = app_esport_is_followed(c, c->matches[i].team_b);
    }
}

const char *app_match_state_name(app_match_state_t state)
{
    switch (state) {
    case APP_MATCH_UPCOMING: return "未开始";
    case APP_MATCH_LIVE:     return "进行中";
    case APP_MATCH_FINISHED: return "已结束";
    default:                 return "未知";
    }
}

app_match_state_t app_esport_state_from_text(const char *text)
{
    if (!text) return APP_MATCH_UNKNOWN;
    if (text_ieq(text, "inProgress")) return APP_MATCH_LIVE;
    if (text_ieq(text, "completed")) return APP_MATCH_FINISHED;
    if (text_ieq(text, "unstarted")) return APP_MATCH_UPCOMING;
    return APP_MATCH_UNKNOWN;
}
