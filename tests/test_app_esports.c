// tests/test_app_esports.c —— 赛事优先级、排序与关注战队的主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_esports.h"

static app_esport_match_t make_match(const char *id, app_match_state_t state, int start_utc)
{
    app_esport_match_t m;
    memset(&m, 0, sizeof(m));
    strncpy(m.id, id, sizeof(m.id) - 1);
    m.state = state;
    m.start_utc = start_utc;
    return m;
}

static void test_priority(void)
{
    const int now = 1000000;
    app_esport_match_t m;

    // 1) 直播 -> 0。
    m = make_match("live", APP_MATCH_LIVE, now - 100);
    assert(app_esport_priority(&m, now) == 0);

    // 2) 关注战队且 30 分钟内开赛 -> 1；边界 1800 秒仍为 1。
    m = make_match("soon", APP_MATCH_UPCOMING, now + 1800);
    m.follow_a = true;
    assert(app_esport_priority(&m, now) == 1);
    m = make_match("soon_b", APP_MATCH_UPCOMING, now + 60);
    m.follow_b = true;
    assert(app_esport_priority(&m, now) == 1);

    // 超过 30 分钟则降为 2。
    m = make_match("soon_edge", APP_MATCH_UPCOMING, now + 1801);
    m.follow_a = true;
    assert(app_esport_priority(&m, now) == 2);

    // 未关注但 30 分钟内 -> 2。
    m = make_match("soon_nf", APP_MATCH_UPCOMING, now + 1800);
    assert(app_esport_priority(&m, now) == 2);

    // 3) 未来 24 小时内 -> 2；边界 24h-1 仍为 2。
    m = make_match("later", APP_MATCH_UPCOMING, now + 3600);
    assert(app_esport_priority(&m, now) == 2);
    m = make_match("day_edge", APP_MATCH_UPCOMING, now + 24 * 3600 - 1);
    assert(app_esport_priority(&m, now) == 2);

    // 恰好 24 小时外 -> 4。
    m = make_match("tomorrow", APP_MATCH_UPCOMING, now + 24 * 3600);
    assert(app_esport_priority(&m, now) == 4);

    // 4) 最近 24 小时内结束 -> 3；边界 24h 仍为 3。
    m = make_match("fin", APP_MATCH_FINISHED, now - 100);
    assert(app_esport_priority(&m, now) == 3);
    m = make_match("fin_edge", APP_MATCH_FINISHED, now - 24 * 3600);
    assert(app_esport_priority(&m, now) == 3);

    // 超过 24 小时 -> 4。
    m = make_match("fin_old", APP_MATCH_FINISHED, now - 24 * 3600 - 1);
    assert(app_esport_priority(&m, now) == 4);

    // 5) 其余情况 -> 4。
    m = make_match("unknown", APP_MATCH_UNKNOWN, now);
    assert(app_esport_priority(&m, now) == 4);
}

static void test_sort_stable(void)
{
    const int now = 1000000;
    app_esport_match_t ms[3];
    ms[0] = make_match("A", APP_MATCH_UPCOMING, now + 7200);   // 优先级 2
    ms[1] = make_match("B", APP_MATCH_LIVE, now - 60);         // 优先级 0
    ms[2] = make_match("C", APP_MATCH_UPCOMING, now + 3600);   // 优先级 2

    app_esport_sort(ms, 3, now);
    assert(strcmp(ms[0].id, "B") == 0);
    assert(strcmp(ms[1].id, "A") == 0);   // 同优先级保持原顺序
    assert(strcmp(ms[2].id, "C") == 0);
}

static void test_home_pick(void)
{
    const int now = 1000000;
    app_esport_match_t ms[4];

    // 有直播 -> 第一场直播。
    ms[0] = make_match("u1", APP_MATCH_UPCOMING, now + 1000);
    ms[1] = make_match("l1", APP_MATCH_LIVE, now - 10);
    ms[2] = make_match("u2", APP_MATCH_UPCOMING, now + 2000);
    assert(app_esport_home_pick(ms, 3, now) == 1);

    // 无直播 -> 30 分钟内最早开赛的关注比赛。
    memset(ms, 0, sizeof(ms));
    ms[0] = make_match("f1", APP_MATCH_UPCOMING, now + 1700);
    ms[0].follow_b = true;
    ms[1] = make_match("f2", APP_MATCH_UPCOMING, now + 600);
    ms[1].follow_a = true;
    ms[2] = make_match("u", APP_MATCH_UPCOMING, now + 100);
    assert(app_esport_home_pick(ms, 3, now) == 1);

    // 无直播、无 30 分钟内关注 -> 未来 24 小时内最早开赛的一场。
    memset(ms, 0, sizeof(ms));
    ms[0] = make_match("x1", APP_MATCH_UPCOMING, now + 5000);
    ms[1] = make_match("x2", APP_MATCH_UPCOMING, now + 2000);
    assert(app_esport_home_pick(ms, 2, now) == 1);

    // 都不满足 -> -1。
    memset(ms, 0, sizeof(ms));
    ms[0] = make_match("e1", APP_MATCH_FINISHED, now - 100);
    ms[1] = make_match("e2", APP_MATCH_UPCOMING, now + 48 * 3600);
    assert(app_esport_home_pick(ms, 2, now) == -1);

    assert(app_esport_home_pick(NULL, 0, now) == -1);
}

static void test_follows(void)
{
    app_esport_cache_t c;
    memset(&c, 0, sizeof(c));

    assert(!app_esport_follow_set(&c, NULL, true));
    assert(!app_esport_follow_set(&c, "", true));
    assert(!app_esport_is_followed(&c, NULL));

    assert(app_esport_follow_set(&c, "T1", true));
    assert(app_esport_is_followed(&c, "T1"));
    assert(!app_esport_is_followed(&c, "T2"));
    assert(app_esport_follow_set(&c, "T1", true));   // 幂等
    assert(c.follow_count == 1);

    // 上限 5。
    assert(app_esport_follow_set(&c, "T2", true));
    assert(app_esport_follow_set(&c, "T3", true));
    assert(app_esport_follow_set(&c, "T4", true));
    assert(app_esport_follow_set(&c, "T5", true));
    assert(c.follow_count == APP_ESPORT_MAX_FOLLOWS);
    assert(!app_esport_follow_set(&c, "T6", true));
    assert(c.follow_count == APP_ESPORT_MAX_FOLLOWS);

    // 移除后可再添加。
    assert(app_esport_follow_set(&c, "T3", false));
    assert(!app_esport_is_followed(&c, "T3"));
    assert(c.follow_count == 4);
    assert(app_esport_follow_set(&c, "T6", true));
    assert(c.follow_count == 5);

    // 依据 followed[] 刷新 follow_a / follow_b。
    c.match_count = 2;
    memset(&c.matches[0], 0, sizeof(c.matches[0]));
    strcpy(c.matches[0].team_a, "T1");
    strcpy(c.matches[0].team_b, "T9");
    c.matches[0].follow_a = false;
    c.matches[0].follow_b = true;    // 脏值，应被刷新
    memset(&c.matches[1], 0, sizeof(c.matches[1]));
    strcpy(c.matches[1].team_a, "T9");
    strcpy(c.matches[1].team_b, "T2");
    c.matches[1].follow_a = true;    // 脏值，应被刷新

    app_esport_apply_follows(&c);
    assert(c.matches[0].follow_a);
    assert(!c.matches[0].follow_b);
    assert(!c.matches[1].follow_a);
    assert(c.matches[1].follow_b);
}

static void test_state(void)
{
    assert(app_esport_state_from_text("inProgress") == APP_MATCH_LIVE);
    assert(app_esport_state_from_text("INPROGRESS") == APP_MATCH_LIVE);
    assert(app_esport_state_from_text("Inprogress") == APP_MATCH_LIVE);
    assert(app_esport_state_from_text("completed") == APP_MATCH_FINISHED);
    assert(app_esport_state_from_text("Completed") == APP_MATCH_FINISHED);
    assert(app_esport_state_from_text("unstarted") == APP_MATCH_UPCOMING);
    assert(app_esport_state_from_text("UNSTARTED") == APP_MATCH_UPCOMING);
    assert(app_esport_state_from_text("weird") == APP_MATCH_UNKNOWN);
    assert(app_esport_state_from_text("") == APP_MATCH_UNKNOWN);
    assert(app_esport_state_from_text(NULL) == APP_MATCH_UNKNOWN);

    assert(strcmp(app_match_state_name(APP_MATCH_UPCOMING), "未开始") == 0);
    assert(strcmp(app_match_state_name(APP_MATCH_LIVE), "进行中") == 0);
    assert(strcmp(app_match_state_name(APP_MATCH_FINISHED), "已结束") == 0);
    assert(strcmp(app_match_state_name(APP_MATCH_UNKNOWN), "未知") == 0);
    assert(strcmp(app_match_state_name((app_match_state_t)99), "未知") == 0);
}

int main(void)
{
    test_priority();
    test_sort_stable();
    test_home_pick();
    test_follows();
    test_state();
    puts("test_app_esports: PASS");
    return 0;
}
