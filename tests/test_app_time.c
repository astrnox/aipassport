#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_time.h"

// 按“加月份并收敛日号”的方式组合公共 API：先得到目标年月，再把日号夹到该月长度。
static void add_months_clamped(int year, int month, int day, int delta,
                               int *oy, int *om, int *od)
{
    assert(app_time_add_months(year, month, delta, oy, om));
    int dim = app_time_days_in_month(*oy, *om);
    *od = day > dim ? dim : day;
}

int main(void)
{
    // ---- 闰年 ----
    assert(app_time_is_leap_year(2000) == true);
    assert(app_time_is_leap_year(1900) == false);
    assert(app_time_is_leap_year(2024) == true);
    assert(app_time_is_leap_year(2023) == false);
    assert(app_time_is_leap_year(2026) == false);

    // ---- 每月天数（含二月）----
    assert(app_time_days_in_month(2026, 1) == 31);
    assert(app_time_days_in_month(2026, 2) == 28);
    assert(app_time_days_in_month(2024, 2) == 29);
    assert(app_time_days_in_month(2000, 2) == 29);
    assert(app_time_days_in_month(1900, 2) == 28);
    assert(app_time_days_in_month(2026, 4) == 30);
    assert(app_time_days_in_month(2026, 12) == 31);

    // ---- 星期 ----
    assert(app_time_weekday(2026, 9, 24) == 4);   // 周四
    assert(app_time_weekday(2000, 1, 1) == 6);    // 周六
    assert(app_time_weekday(2026, 9, 21) == 1);   // 周一

    // ---- 年内第几天 ----
    assert(app_time_day_of_year(2026, 1, 1) == 1);
    assert(app_time_day_of_year(2026, 3, 1) == 60);
    assert(app_time_day_of_year(2024, 3, 1) == 61);
    assert(app_time_day_of_year(2026, 12, 31) == 365);
    assert(app_time_day_of_year(2024, 12, 31) == 366);

    // ---- 时间合法性 ----
    app_datetime_t ok = {2026, 9, 24, 13, 45, 30};
    assert(app_time_valid(&ok) == true);
    app_datetime_t bad_month = {2026, 13, 1, 0, 0, 0};
    assert(app_time_valid(&bad_month) == false);
    app_datetime_t bad_day = {2026, 2, 30, 0, 0, 0};
    assert(app_time_valid(&bad_day) == false);
    app_datetime_t bad_hour = {2026, 9, 24, 24, 0, 0};
    assert(app_time_valid(&bad_hour) == false);
    app_datetime_t bad_year = {1969, 1, 1, 0, 0, 0};
    assert(app_time_valid(&bad_year) == false);
    assert(app_time_valid(NULL) == false);

    // ---- 日期加减 ----
    int y, m, d;
    assert(app_time_add_days(2026, 1, 31, 1, &y, &m, &d));
    assert(y == 2026 && m == 2 && d == 1);
    assert(app_time_add_days(2026, 12, 31, 1, &y, &m, &d));
    assert(y == 2027 && m == 1 && d == 1);
    assert(app_time_add_days(2026, 3, 1, -1, &y, &m, &d));
    assert(y == 2026 && m == 2 && d == 28);
    assert(app_time_add_days(2024, 2, 28, 1, &y, &m, &d));
    assert(y == 2024 && m == 2 && d == 29);
    assert(app_time_add_days(2026, 1, 1, -1, &y, &m, &d));
    assert(y == 2025 && m == 12 && d == 31);
    assert(app_time_add_days(2026, 9, 24, 365, &y, &m, &d));
    assert(y == 2027 && m == 9 && d == 24);

    // 越界：返回 false 且不改动输出。
    int oy = 111, om = 222, od = 333;
    assert(app_time_add_days(2099, 12, 31, 1, &oy, &om, &od) == false);
    assert(oy == 111 && om == 222 && od == 333);
    assert(app_time_add_days(1970, 1, 1, -1, &oy, &om, &od) == false);
    assert(oy == 111 && om == 222 && od == 333);

    // ---- 月份加减 ----
    assert(app_time_add_months(2026, 1, 1, &y, &m));
    assert(y == 2026 && m == 2);
    assert(app_time_add_months(2026, 12, 1, &y, &m));
    assert(y == 2027 && m == 1);
    assert(app_time_add_months(2026, 1, -1, &y, &m));
    assert(y == 2025 && m == 12);
    assert(app_time_add_months(2026, 1, -13, &y, &m));
    assert(y == 2024 && m == 12);
    int my = 111, mm = 222;
    assert(app_time_add_months(2099, 12, 1, &my, &mm) == false);
    assert(my == 111 && mm == 222);
    assert(app_time_add_months(1970, 1, -1, &my, &mm) == false);
    assert(my == 111 && mm == 222);

    // 日号收敛：2026-01-31 + 1 个月 = 2026-02-28。
    add_months_clamped(2026, 1, 31, 1, &y, &m, &d);
    assert(y == 2026 && m == 2 && d == 28);
    add_months_clamped(2024, 1, 31, 1, &y, &m, &d);
    assert(y == 2024 && m == 2 && d == 29);

    // ---- 农历 ----
    app_lunar_t lu;
    assert(app_lunar_from_solar(2026, 2, 17, &lu));
    assert(lu.year == 2026 && lu.month == 1 && lu.day == 1 && lu.leap == false);
    assert(strcmp(lu.month_name, "正月") == 0);
    assert(strcmp(lu.day_name, "初一") == 0);
    assert(strcmp(lu.ganzhi, "丙午") == 0);
    assert(strcmp(lu.zodiac, "马") == 0);

    assert(app_lunar_from_solar(2025, 1, 29, &lu));
    assert(lu.year == 2025 && lu.month == 1 && lu.day == 1 && lu.leap == false);
    assert(strcmp(lu.ganzhi, "乙巳") == 0);
    assert(strcmp(lu.zodiac, "蛇") == 0);

    assert(app_lunar_from_solar(2024, 2, 10, &lu));
    assert(lu.year == 2024 && lu.month == 1 && lu.day == 1 && lu.leap == false);
    assert(strcmp(lu.ganzhi, "甲辰") == 0);
    assert(strcmp(lu.zodiac, "龙") == 0);

    // 2026-09-24 = 农历 八月十四。
    assert(app_lunar_from_solar(2026, 9, 24, &lu));
    assert(lu.year == 2026 && lu.month == 8 && lu.day == 14 && lu.leap == false);
    assert(strcmp(lu.month_name, "八月") == 0);
    assert(strcmp(lu.day_name, "十四") == 0);

    // 2023-03-22 = 农历 闰二月初一。
    assert(app_lunar_from_solar(2023, 3, 22, &lu));
    assert(lu.year == 2023 && lu.month == 2 && lu.day == 1 && lu.leap == true);
    assert(strcmp(lu.month_name, "闰二月") == 0);

    // 非法日期返回 false。
    assert(app_lunar_from_solar(2026, 2, 30, &lu) == false);
    assert(app_lunar_from_solar(1969, 1, 1, &lu) == false);

    // ---- 二十四节气 ----
    assert(strcmp(app_solar_term_name(2026, 10, 8), "寒露") == 0);
    assert(strcmp(app_solar_term_name(2026, 9, 23), "秋分") == 0);
    assert(strcmp(app_solar_term_name(2024, 10, 8), "寒露") == 0);
    assert(strcmp(app_solar_term_name(2025, 10, 8), "寒露") == 0);
    assert(app_solar_term_name(2026, 10, 7) == NULL);
    assert(app_solar_term_name(1969, 10, 8) == NULL);

    // ---- 建除十二神 ----
    assert(strcmp(app_lunar_duty_name(2026, 9, 24), "除") == 0);
    assert(app_lunar_duty_name(2026, 2, 30) == NULL);

    // ---- 宜忌（确定性、容量安全）----
    char yi[64], ji[64];
    app_lunar_yi_ji(2026, 9, 24, yi, sizeof(yi), ji, sizeof(ji));
    assert(strcmp(yi, "沐浴 扫舍 治病") == 0);
    assert(strcmp(ji, "嫁娶 出行 开市") == 0);
    char yi_again[64];
    app_lunar_yi_ji(2026, 9, 24, yi_again, sizeof(yi_again), ji, sizeof(ji));
    assert(strcmp(yi, yi_again) == 0);

    char tiny[7];
    app_lunar_yi_ji(2026, 9, 24, tiny, sizeof(tiny), ji, sizeof(ji));
    assert(strcmp(tiny, "沐浴") == 0);   // 按完整字符截断，不劈开 UTF-8
    char one[1];
    app_lunar_yi_ji(2026, 9, 24, one, sizeof(one), ji, sizeof(ji));
    assert(one[0] == '\0');

    // ---- 时间进度：全部 5 尺度 × 3 精度 ----
    app_datetime_t now = {2026, 9, 24, 13, 45, 30};
    for (int s = 0; s < (int)APP_SCALE_COUNT; s++) {
        for (int p = 0; p < (int)APP_PREC_COUNT; p++) {
            int t = -1, f = -1;
            app_time_progress((app_scale_t)s, (app_precision_t)p, &now,
                              1990, 6, 15, 80, &t, &f);
            assert(t > 0);
            assert(f >= 0 && f <= t);
        }
    }

    int t, f;
    // 日
    app_time_progress(APP_SCALE_DAY, APP_PREC_COARSE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 24 && f == 14);
    app_time_progress(APP_SCALE_DAY, APP_PREC_BALANCED, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 96 && f == 56);
    app_time_progress(APP_SCALE_DAY, APP_PREC_FINE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 288 && f == 166);
    // 周（周一为始）
    app_time_progress(APP_SCALE_WEEK, APP_PREC_COARSE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 7 && f == 4);
    app_time_progress(APP_SCALE_WEEK, APP_PREC_BALANCED, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 56 && f == 29);
    app_time_progress(APP_SCALE_WEEK, APP_PREC_FINE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 168 && f == 86);
    // 月（2026-09 共 30 天）
    app_time_progress(APP_SCALE_MONTH, APP_PREC_COARSE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 30 && f == 24);
    app_time_progress(APP_SCALE_MONTH, APP_PREC_BALANCED, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 120 && f == 95);
    app_time_progress(APP_SCALE_MONTH, APP_PREC_FINE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 240 && f == 189);
    // 年（2026 非闰）
    app_time_progress(APP_SCALE_YEAR, APP_PREC_COARSE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 12 && f == 9);
    app_time_progress(APP_SCALE_YEAR, APP_PREC_BALANCED, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 53 && f == 39);
    app_time_progress(APP_SCALE_YEAR, APP_PREC_FINE, &now, 0, 0, 0, 0, &t, &f);
    assert(t == 365 && f == 267);
    // 人生
    app_time_progress(APP_SCALE_LIFE, APP_PREC_COARSE, &now, 1990, 6, 15, 80, &t, &f);
    assert(t == 80 && f == 37);
    app_time_progress(APP_SCALE_LIFE, APP_PREC_BALANCED, &now, 1990, 6, 15, 80, &t, &f);
    assert(t == 320 && f == 146);
    app_time_progress(APP_SCALE_LIFE, APP_PREC_FINE, &now, 1990, 6, 15, 80, &t, &f);
    assert(t == 960 && f == 436);

    // 生日当天 00:00：第一格。
    app_datetime_t birthday = {1990, 6, 15, 0, 0, 0};
    app_time_progress(APP_SCALE_LIFE, APP_PREC_COARSE, &birthday, 1990, 6, 15, 80, &t, &f);
    assert(t == 80 && f == 1);

    // 非法生日：0/0。
    app_time_progress(APP_SCALE_LIFE, APP_PREC_BALANCED, &now, 1960, 1, 1, 80, &t, &f);
    assert(t == 0 && f == 0);
    app_time_progress(APP_SCALE_LIFE, APP_PREC_BALANCED, &now, 2030, 1, 1, 80, &t, &f);
    assert(t == 0 && f == 0);

    // ---- 名称 ----
    assert(strcmp(app_scale_name(APP_SCALE_DAY), "今日") == 0);
    assert(strcmp(app_scale_name(APP_SCALE_WEEK), "本周") == 0);
    assert(strcmp(app_scale_name(APP_SCALE_MONTH), "本月") == 0);
    assert(strcmp(app_scale_name(APP_SCALE_YEAR), "今年") == 0);
    assert(strcmp(app_scale_name(APP_SCALE_LIFE), "人生") == 0);
    assert(strcmp(app_precision_name(APP_PREC_COARSE), "总览") == 0);
    assert(strcmp(app_precision_name(APP_PREC_BALANCED), "均衡") == 0);
    assert(strcmp(app_precision_name(APP_PREC_FINE), "精细") == 0);
    assert(strcmp(app_scale_name((app_scale_t)99), "??") == 0);
    assert(strcmp(app_scale_name((app_scale_t)-1), "??") == 0);
    assert(strcmp(app_precision_name((app_precision_t)99), "??") == 0);
    assert(strcmp(app_precision_name((app_precision_t)-1), "??") == 0);

    // ---- Unix 秒与公历互转 ----
    // 已知基准：1970-01-01 00:00:00 UTC 为 0。
    assert(app_time_to_unix(1970, 1, 1, 0, 0, 0) == 0);
    assert(app_time_to_unix(1970, 1, 1, 0, 0, 1) == 1);
    assert(app_time_to_unix(1970, 1, 1, 0, 1, 0) == 60);
    assert(app_time_to_unix(1970, 1, 2, 0, 0, 0) == 86400);
    // 2000-03-01 是闰年之后，跨过 2000-02-29。
    assert(app_time_to_unix(2000, 3, 1, 0, 0, 0) -
           app_time_to_unix(2000, 2, 28, 0, 0, 0) == 2 * 86400);
    // 非法日期/越界时间返回 0。
    assert(app_time_to_unix(2026, 2, 30, 0, 0, 0) == 0);
    assert(app_time_to_unix(2026, 1, 1, 24, 0, 0) == 0);
    assert(app_time_to_unix(2026, 1, 1, 0, 60, 0) == 0);

    // 往返一致：公历 -> Unix -> 公历。
    {
        static const app_datetime_t samples[] = {
            { 1970, 1, 1, 0, 0, 0 },
            { 2026, 9, 24, 10, 30, 59 },
            { 2000, 2, 29, 23, 59, 59 },
            { 2099, 12, 31, 12, 0, 0 },
        };
        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            int64_t unix_sec = app_time_to_unix(samples[i].year, samples[i].month,
                                                samples[i].day, samples[i].hour,
                                                samples[i].minute, samples[i].second);
            assert(unix_sec >= 0);
            app_datetime_t back;
            assert(app_time_from_unix(unix_sec, &back));
            assert(memcmp(&back, &samples[i], sizeof(back)) == 0);
        }
        // 同一时刻的星期在两种表示下一致。
        app_datetime_t dt;
        assert(app_time_from_unix(app_time_to_unix(2026, 9, 24, 10, 0, 0), &dt));
        assert(app_time_weekday(dt.year, dt.month, dt.day) ==
               app_time_weekday(2026, 9, 24));
    }

    puts("test_app_time: PASS");
    return 0;
}
