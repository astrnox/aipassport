// tests/test_app_reminder.c —— app_reminder 的主机侧单元测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_reminder.h"
#include "logic/app_time.h"

static app_reminder_t make_weekly(int hour, int minute, uint8_t mask)
{
    app_reminder_t r;
    memset(&r, 0, sizeof(r));
    r.enabled = true;
    r.hour = hour;
    r.minute = minute;
    r.repeat_weekly = true;
    r.weekday_mask = mask;
    memcpy(r.label, "test", 5);
    return r;
}

static app_reminder_t make_onetime(int year, int month, int day, int hour, int minute)
{
    app_reminder_t r;
    memset(&r, 0, sizeof(r));
    r.enabled = true;
    r.hour = hour;
    r.minute = minute;
    r.repeat_weekly = false;
    r.year = year;
    r.month = month;
    r.day = day;
    memcpy(r.label, "once", 5);
    return r;
}

int main(void)
{
    // ---- 列表增删 ----
    app_reminder_list_t list;
    app_reminder_list_init(&list);
    assert(list.count == 0);

    for (int i = 0; i < APP_REMINDER_MAX; i++) {
        assert(app_reminder_add(&list, NULL) == i);
    }
    assert(list.count == APP_REMINDER_MAX);
    assert(app_reminder_add(&list, NULL) == -1);   // 已满

    // 删除中间一项后尾部前移
    app_reminder_t last = list.items[APP_REMINDER_MAX - 1];
    assert(app_reminder_remove(&list, 1));
    assert(list.count == APP_REMINDER_MAX - 1);
    assert(memcmp(&list.items[list.count - 1], &last, sizeof(last)) == 0);
    assert(!app_reminder_remove(&list, -1));
    assert(!app_reminder_remove(&list, list.count));
    app_reminder_list_init(&list);

    // 默认项：禁用、08:00、每天
    int idx = app_reminder_add(&list, NULL);
    assert(idx == 0);
    assert(!list.items[0].enabled);
    assert(list.items[0].hour == 8 && list.items[0].minute == 0);
    assert(list.items[0].repeat_weekly && list.items[0].weekday_mask == 0x7F);

    // ---- 基准时间 ----
    app_datetime_t now = { 2026, 9, 24, 10, 0, 0 };
    int now_weekday = app_time_weekday(now.year, now.month, now.day);

    // 每周提醒：今天稍后
    app_reminder_t later_today = make_weekly(10, 30, (uint8_t)(1u << now_weekday));
    assert(app_reminder_next_seconds(&later_today, &now, now_weekday) == 30 * 60);

    // 每周提醒：今天时刻已过 -> 顺延到下周同一天
    app_reminder_t passed_today = make_weekly(9, 0, (uint8_t)(1u << now_weekday));
    assert(app_reminder_next_seconds(&passed_today, &now, now_weekday) ==
           7 * 86400 - 60 * 60);

    // 每周提醒：明天（用 app_time_add_days 求日期与星期）
    int ty, tm, td;
    assert(app_time_add_days(now.year, now.month, now.day, 1, &ty, &tm, &td));
    int tomorrow_weekday = app_time_weekday(ty, tm, td);
    app_reminder_t tomorrow = make_weekly(8, 0, (uint8_t)(1u << tomorrow_weekday));
    assert(app_reminder_next_seconds(&tomorrow, &now, now_weekday) ==
           86400 + (8 * 60 - 10 * 60) * 60);

    // 禁用的提醒永不触发
    app_reminder_t disabled = later_today;
    disabled.enabled = false;
    assert(app_reminder_next_seconds(&disabled, &now, now_weekday) == -1);

    // 一次性提醒：未来
    app_reminder_t future = make_onetime(2026, 10, 1, 9, 0);
    assert(app_reminder_next_seconds(&future, &now, now_weekday) ==
           7 * 86400 + (9 * 60 - 10 * 60) * 60);

    // 一次性提醒：过去
    app_reminder_t past = make_onetime(2026, 9, 1, 9, 0);
    assert(app_reminder_next_seconds(&past, &now, now_weekday) == -1);

    // 一次性提醒：跨年（12/30 23:00 -> 次年 1/2 01:00 = 2 天 2 小时）
    app_datetime_t yearend = { 2026, 12, 30, 23, 0, 0 };
    int oy, om, od;
    assert(app_time_add_days(yearend.year, yearend.month, yearend.day, 3, &oy, &om, &od));
    app_reminder_t newyear = make_onetime(oy, om, od, 1, 0);
    assert(app_reminder_next_seconds(&newyear, &yearend, app_time_weekday(oy, om, od)) ==
           2 * 86400 + 2 * 3600);

    // ---- due：仅命中所在分钟 ----
    app_reminder_t due = make_weekly(10, 0, (uint8_t)(1u << now_weekday));
    assert(app_reminder_due(&due, &now, now_weekday));
    app_datetime_t next_minute = { 2026, 9, 24, 10, 1, 0 };
    assert(!app_reminder_due(&due, &next_minute, now_weekday));
    // 星期位未命中
    app_reminder_t wrong_day = make_weekly(10, 0, (uint8_t)(1u << ((now_weekday + 1) % 7)));
    assert(!app_reminder_due(&wrong_day, &now, now_weekday));
    // 禁用
    assert(!app_reminder_due(&disabled, &now, now_weekday));
    // 一次性日期匹配
    app_reminder_t once_due = make_onetime(now.year, now.month, now.day, now.hour, now.minute);
    assert(app_reminder_due(&once_due, &now, now_weekday));
    app_reminder_t once_wrong = make_onetime(now.year, now.month, now.day + 1, now.hour, now.minute);
    assert(!app_reminder_due(&once_wrong, &now, now_weekday));

    // ---- schedule_text ----
    char text[64];
    app_reminder_t daily = make_weekly(8, 0, 0x7F);
    assert(app_reminder_schedule_text(&daily, text, sizeof(text)) == (int)strlen("每天"));
    assert(strcmp(text, "每天") == 0);

    app_reminder_t monday_wednesday =
        make_weekly(8, 0, (uint8_t)((1u << 1) | (1u << 3)));   // 周一、周三
    assert(app_reminder_schedule_text(&monday_wednesday, text, sizeof(text)) > 0);
    assert(strcmp(text, "周一 周三") == 0);

    app_reminder_t one = make_onetime(2026, 9, 24, 8, 0);
    assert(app_reminder_schedule_text(&one, text, sizeof(text)) == (int)strlen("2026-09-24"));
    assert(strcmp(text, "2026-09-24") == 0);

    // 小缓冲不越界
    char small[4];
    app_reminder_schedule_text(&daily, small, sizeof(small));
    assert(small[sizeof(small) - 1] == '\0');

    // ---- missed：开机汇总错过的提醒 ----
    {
        int64_t to = app_time_to_unix(2026, 9, 24, 10, 0, 0);
        int wd = app_time_weekday(2026, 9, 24);
        assert(to > 0 && wd >= 0);

        app_reminder_list_t ml;
        app_reminder_list_init(&ml);
        char out[APP_REMINDER_MAX][6];

        // 空列表与非法区间都返回 0
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, APP_REMINDER_MAX) == 0);
        assert(app_reminder_missed(&ml, 0, to, 0, out, APP_REMINDER_MAX) == 0);
        assert(app_reminder_missed(&ml, to, to, 0, out, APP_REMINDER_MAX) == 0);

        // 窗口内命中一次
        ml.items[ml.count++] = make_weekly(9, 30, (uint8_t)(1u << wd));
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, APP_REMINDER_MAX) == 1);
        assert(strcmp(out[0], "09:30") == 0);

        // 每天重复的提醒跨多天也只算一条，且取窗口内最近一次
        ml.items[0] = make_weekly(9, 30, 0x7F);
        assert(app_reminder_missed(&ml, to - 3 * 86400, to, 0, out, APP_REMINDER_MAX) == 1);
        assert(strcmp(out[0], "09:30") == 0);

        // 上界含 to 所在分钟
        ml.items[0] = make_weekly(10, 0, (uint8_t)(1u << wd));
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, APP_REMINDER_MAX) == 1);
        assert(strcmp(out[0], "10:00") == 0);

        // 窗口之外、以及被禁用的提醒都不计入
        ml.items[0] = make_weekly(8, 0, (uint8_t)(1u << wd));
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, APP_REMINDER_MAX) == 0);
        app_reminder_t off = make_weekly(9, 30, (uint8_t)(1u << wd));
        off.enabled = false;
        ml.items[0] = off;
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, APP_REMINDER_MAX) == 0);

        // 超过 24 小时回溯窗口的部分不再计入
        ml.items[0] = make_onetime(2026, 9, 22, 9, 30);
        assert(app_reminder_missed(&ml, to - 30 * 86400, to, 0, out, APP_REMINDER_MAX) == 0);

        // 多条命中按 max 截断，顺序与列表一致
        ml.count = 0;
        ml.items[ml.count++] = make_weekly(9, 10, (uint8_t)(1u << wd));
        ml.items[ml.count++] = make_weekly(9, 20, (uint8_t)(1u << wd));
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, 1) == 1);
        assert(strcmp(out[0], "09:10") == 0);
        assert(app_reminder_missed(&ml, to - 3600, to, 0, out, APP_REMINDER_MAX) == 2);
        assert(strcmp(out[0], "09:10") == 0 && strcmp(out[1], "09:20") == 0);

        // 时区偏移：传入的是 UTC 秒，加偏移后才得到本地时间
        ml.count = 1;
        ml.items[0] = make_weekly(9, 30, (uint8_t)(1u << wd));
        int64_t to_utc = to - 8 * 3600;
        assert(app_reminder_missed(&ml, to_utc - 3600, to_utc, 8 * 60,
                                   out, APP_REMINDER_MAX) == 1);
        assert(strcmp(out[0], "09:30") == 0);
    }

    puts("test_app_reminder: PASS");
    return 0;
}
