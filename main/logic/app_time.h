// main/logic/app_time.h —— 与硬件无关的日历、农历、节气与时间进度计算。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 本地时间。年范围 1970..2099。
typedef struct {
    int year;
    int month;   // 1..12
    int day;     // 1..31
    int hour;    // 0..23
    int minute;  // 0..59
    int second;  // 0..59
} app_datetime_t;

// 农历日期（含干支纪年与生肖）。
typedef struct {
    int  year;           // 农历年
    int  month;          // 1..12
    int  day;            // 1..30
    bool leap;           // 是否为闰月
    char month_name[12]; // "正月".."腊月"，闰月加前缀 "闰"（"闰四月" 为 9 字节 + NUL）
    char day_name[12];   // "初一".."三十"
    char ganzhi[8];      // 干支纪年，如 "甲辰"
    char zodiac[8];      // 生肖，如 "龙"
} app_lunar_t;

typedef enum {
    APP_SCALE_DAY = 0,
    APP_SCALE_WEEK,
    APP_SCALE_MONTH,
    APP_SCALE_YEAR,
    APP_SCALE_LIFE,
    APP_SCALE_COUNT,
} app_scale_t;

typedef enum {
    APP_PREC_COARSE = 0,   // 总览
    APP_PREC_BALANCED,     // 均衡
    APP_PREC_FINE,         // 精细
    APP_PREC_COUNT,
} app_precision_t;

bool app_time_is_leap_year(int year);
int  app_time_days_in_month(int year, int month);
int  app_time_weekday(int year, int month, int day);        // 0=周日 .. 6=周六
int  app_time_day_of_year(int year, int month, int day);    // 1 起
// ISO 8601 周序号（1..53）：周一为一周之始，含 1 月 4 日的那一周为第 1 周。
// 单双周作息用它判断本周是单周还是双周。日期非法时返回 0。
int  app_time_iso_week(int year, int month, int day);
bool app_time_valid(const app_datetime_t *dt);
bool app_time_add_days(int year, int month, int day, int delta, int *oy, int *om, int *od);
bool app_time_add_months(int year, int month, int delta, int *oy, int *om);

// Unix 秒与公历互转。本层不做时区处理：时区偏移由调用方按分钟自行加减，
// 传入 UTC 得到 UTC、传入本地时间得到"本地 Unix 秒"。
int64_t app_time_to_unix(int year, int month, int day, int hour, int minute, int second);
bool    app_time_from_unix(int64_t unix_sec, app_datetime_t *out);

bool app_lunar_from_solar(int year, int month, int day, app_lunar_t *out);
const char *app_solar_term_name(int year, int month, int day);   // 无节气返回 NULL
const char *app_lunar_duty_name(int year, int month, int day);   // 建除十二神
void app_lunar_yi_ji(int year, int month, int day,
                     char *yi, size_t yi_cap, char *ji, size_t ji_cap);

// 时间进度：total 为总格数，filled 为已过格数（含当前格，范围 0..total）。
void app_time_progress(app_scale_t scale, app_precision_t prec,
                       const app_datetime_t *now,
                       int birth_year, int birth_month, int birth_day, int life_expectancy,
                       int *total, int *filled);

const char *app_scale_name(app_scale_t scale);
const char *app_precision_name(app_precision_t prec);
