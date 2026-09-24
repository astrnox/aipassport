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

    puts("test_app_reminder: PASS");
    return 0;
}
