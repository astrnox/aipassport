#include "app_time.h"

#include <stdio.h>
#include <string.h>

// 农历数据表，覆盖 1900..2100 共 201 年。
// 编码：bit0..3 为闰月月份（0 表示无闰月）；bit16 为闰月是否 30 天；
// bit15..4 依次对应 1..12 月，置位表示该月 30 天，清零表示 29 天。
static const unsigned int LUNAR_INFO[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
    0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
    0x0d520
};

// ---------------------------------------------------------------------------
// 通用小工具
// ---------------------------------------------------------------------------

// 把 src 按完整 UTF-8 字符复制进 dst，绝不在字符中间截断，并始终以 NUL 结尾。
static void copy_utf8_fit(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t written = 0;
    for (const char *cursor = src; *cursor; ) {
        unsigned char byte = (unsigned char)*cursor;
        size_t len;
        if (byte < 0x80) {
            len = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            len = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            len = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            len = 4;
        } else {
            len = 1;
        }
        if (written + len + 1 > cap) break;
        memcpy(dst + written, cursor, len);
        written += len;
        cursor += len;
    }
    dst[written] = '\0';
}

// 自 1970-01-01 起的天数（可为负）。Howard Hinnant 的 civil 算法。
static long days_from_civil(int year, int month, int day)
{
    long y = year;
    y -= (month <= 2) ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// days_from_civil 的逆运算。
static void civil_from_days(long z, int *out_year, int *out_month, int *out_day)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp + (mp < 10 ? 3 : -9);
    *out_year = (int)(y + (m <= 2 ? 1 : 0));
    *out_month = (int)m;
    *out_day = (int)d;
}

static bool date_valid(int year, int month, int day)
{
    if (year < 1970 || year > 2099) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > app_time_days_in_month(year, month)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// 公历
// ---------------------------------------------------------------------------

bool app_time_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int app_time_days_in_month(int year, int month)
{
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && app_time_is_leap_year(year)) return 29;
    return DAYS[month - 1];
}

int app_time_weekday(int year, int month, int day)
{
    // Sakamoto 算法：0=周日 .. 6=周六。
    static const int OFFSET[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 1 || month > 12) return -1;
    int y = year;
    if (month < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + OFFSET[month - 1] + day) % 7;
    if (w < 0) w += 7;
    return w;
}

int app_time_day_of_year(int year, int month, int day)
{
    static const int CUMULATIVE[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (month < 1 || month > 12) return 0;
    int doy = CUMULATIVE[month - 1] + day;
    if (month > 2 && app_time_is_leap_year(year)) doy++;
    return doy;
}

bool app_time_valid(const app_datetime_t *dt)
{
    if (!dt) return false;
    if (!date_valid(dt->year, dt->month, dt->day)) return false;
    if (dt->hour < 0 || dt->hour > 23) return false;
    if (dt->minute < 0 || dt->minute > 59) return false;
    if (dt->second < 0 || dt->second > 59) return false;
    return true;
}

bool app_time_add_days(int year, int month, int day, int delta, int *oy, int *om, int *od)
{
    if (!oy || !om || !od) return false;
    if (!date_valid(year, month, day)) return false;

    long shifted = days_from_civil(year, month, day) + delta;
    int ry, rm, rd;
    civil_from_days(shifted, &ry, &rm, &rd);
    if (ry < 1970 || ry > 2099) return false;

    *oy = ry;
    *om = rm;
    *od = rd;
    return true;
}

bool app_time_add_months(int year, int month, int delta, int *oy, int *om)
{
    if (!oy || !om) return false;
    if (month < 1 || month > 12) return false;

    long total = (long)(year - 1970) * 12 + (month - 1) + delta;
    if (total < 0) return false;
    long ry = 1970 + total / 12;
    int rm = (int)(total % 12) + 1;
    if (ry < 1970 || ry > 2099) return false;

    *oy = (int)ry;
    *om = rm;
    return true;
}

// ---------------------------------------------------------------------------
// 农历
// ---------------------------------------------------------------------------

static int lunar_leap_month(int year)
{
    return (int)(LUNAR_INFO[year - 1900] & 0xf);
}

static int lunar_leap_days(int year)
{
    if (lunar_leap_month(year) == 0) return 0;
    return (LUNAR_INFO[year - 1900] & 0x10000) ? 30 : 29;
}

static int lunar_month_days(int year, int month)
{
    return (LUNAR_INFO[year - 1900] & (0x10000u >> month)) ? 30 : 29;
}

static int lunar_year_days(int year)
{
    int sum = 348;  // 12 个月各 29 天
    for (unsigned int mask = 0x8000; mask > 0x8; mask >>= 1) {
        if (LUNAR_INFO[year - 1900] & mask) sum++;
    }
    return sum + lunar_leap_days(year);
}

bool app_lunar_from_solar(int year, int month, int day, app_lunar_t *out)
{
    if (!out) return false;
    if (!date_valid(year, month, day)) return false;

    // 基准：1900-01-31 = 农历 1900 年正月初一。
    long offset = days_from_civil(year, month, day) - days_from_civil(1900, 1, 31);

    int ly = 1900;
    int temp = 0;
    while (ly < 2101 && offset > 0) {
        temp = lunar_year_days(ly);
        offset -= temp;
        ly++;
    }
    if (offset < 0) {
        offset += temp;
        ly--;
    }

    int leap = lunar_leap_month(ly);
    bool is_leap = false;
    int i;
    for (i = 1; i < 13 && offset > 0; i++) {
        if (leap > 0 && i == (leap + 1) && !is_leap) {
            --i;
            is_leap = true;
            temp = lunar_leap_days(ly);
        } else {
            temp = lunar_month_days(ly, i);
        }
        if (is_leap && i == (leap + 1)) is_leap = false;
        offset -= temp;
    }
    if (offset == 0 && leap > 0 && i == leap + 1) {
        if (is_leap) {
            is_leap = false;
        } else {
            is_leap = true;
            --i;
        }
    }
    if (offset < 0) {
        offset += temp;
        --i;
    }

    int lunar_month = i;
    int lunar_day = (int)offset + 1;

    static const char *const MONTH_NAMES[12] = {
        "正月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "冬月", "腊月",
    };
    static const char *const DAY_NAMES[30] = {
        "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
    };
    static const char *const TIAN_GAN[10] = {
        "甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸",
    };
    static const char *const DI_ZHI[12] = {
        "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥",
    };
    static const char *const ZODIAC[12] = {
        "鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪",
    };

    out->year = ly;
    out->month = lunar_month;
    out->day = lunar_day;
    out->leap = is_leap;

    if (is_leap) {
        // "闰X月" 三字在 UTF-8 下最多 9 字节 + NUL，month_name 预留 12 字节可完整放下。
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "闰%s", MONTH_NAMES[lunar_month - 1]);
        copy_utf8_fit(out->month_name, sizeof(out->month_name), buffer);
    } else {
        copy_utf8_fit(out->month_name, sizeof(out->month_name), MONTH_NAMES[lunar_month - 1]);
    }
    copy_utf8_fit(out->day_name, sizeof(out->day_name), DAY_NAMES[lunar_day - 1]);

    // 干支纪年以 1984 年为甲子年，故偏移基准取 4。
    int gan = ((ly - 4) % 10 + 10) % 10;
    int zhi = ((ly - 4) % 12 + 12) % 12;
    snprintf(out->ganzhi, sizeof(out->ganzhi), "%s%s", TIAN_GAN[gan], DI_ZHI[zhi]);
    copy_utf8_fit(out->zodiac, sizeof(out->zodiac), ZODIAC[zhi]);
    return true;
}

// ---------------------------------------------------------------------------
// 二十四节气
// ---------------------------------------------------------------------------

// 各节气相对基准时刻的分钟偏移（0=小寒 .. 23=冬至）。
static const int S_TERM_INFO[24] = {
    0, 21208, 42467, 63836, 85337, 107014, 128867, 150921,
    173149, 195551, 218072, 240693, 263343, 285989, 308563, 331033,
    353350, 375494, 397447, 419210, 440795, 462224, 483532, 504758,
};

static const char *const TERM_NAMES[24] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨",
    "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋", "处暑",
    "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};

// 1900-01-06T02:05:00Z 的 Unix 毫秒值。
static const long long TERM_BASE_MS = -2208549300000LL;

static long long floor_div(long long a, long long b)
{
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// 返回该节气所在公历日（UTC）。
static int solar_term_day(int year, int n)
{
    long long ms = (long long)(31556925974.7 * (double)(year - 1900))
                 + (long long)S_TERM_INFO[n] * 60000LL
                 + TERM_BASE_MS;
    long long days = floor_div(ms, 86400000LL);
    int ty, tm, td;
    civil_from_days(days, &ty, &tm, &td);
    return td;
}

const char *app_solar_term_name(int year, int month, int day)
{
    if (year < 1970 || year > 2099) return NULL;
    if (month < 1 || month > 12) return NULL;
    for (int k = 0; k < 2; k++) {
        int n = (month - 1) * 2 + k;
        if (solar_term_day(year, n) == day) return TERM_NAMES[n];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// 建除十二神
// ---------------------------------------------------------------------------

// 干支日序：0=甲子 .. 59=癸亥。
// 锚点取 1970-01-01 的儒略日 2440588，公式 (JDN + 49) % 60 经历书校验：
// 2000-01-01 得戊午、2024-02-10 得甲辰，均与通用万年历一致；
// 2026-09-24 得日序 37（丑日），故建除取 37 % 12 = 1（除）。
static int ganzhi_day_index(int year, int month, int day)
{
    long jdn = days_from_civil(year, month, day) + 2440588L;
    int index = (int)((jdn + 49) % 60);
    if (index < 0) index += 60;
    return index;
}

const char *app_lunar_duty_name(int year, int month, int day)
{
    static const char *const DUTY[12] = {
        "建", "除", "满", "平", "定", "执", "破", "危", "成", "收", "开", "闭",
    };
    if (!date_valid(year, month, day)) return NULL;
    int duty = ganzhi_day_index(year, month, day) % 12;
    return DUTY[duty];
}

// 宜/忌词表，每项最多三个两字词，用单个空格分隔；由建除索引确定，不含随机。
static const char *const YI_TABLE[12] = {
    "出行 祈福 祭祀",  // 建
    "沐浴 扫舍 治病",  // 除
    "祭祀 祈福 开市",  // 满
    "修饰 平治 涂泥",  // 平
    "祭祀 订盟 纳采",  // 定
    "捕捉 结网 纳畜",  // 执
    "求医 治病 拆卸",  // 破
    "安床 祭祀 祈福",  // 危
    "开市 入学 出行",  // 成
    "纳财 收账 交易",  // 收
    "祈福 求嗣 开光",  // 开
    "安葬 祭祀 修坟",  // 闭
};

static const char *const JI_TABLE[12] = {
    "动土 安葬 开仓",  // 建
    "嫁娶 出行 开市",  // 除
    "嫁娶 安葬 动土",  // 满
    "开市 安葬 动土",  // 平
    "诉讼 出行 移徙",  // 定
    "开市 出行 移徙",  // 执
    "嫁娶 开市 安葬",  // 破
    "出行 移徙 开市",  // 危
    "诉讼 安葬 动土",  // 成
    "开市 安葬 破土",  // 收
    "安葬 动土 诉讼",  // 开
    "开市 出行 嫁娶",  // 闭
};

void app_lunar_yi_ji(int year, int month, int day,
                     char *yi, size_t yi_cap, char *ji, size_t ji_cap)
{
    if (yi && yi_cap > 0) yi[0] = '\0';
    if (ji && ji_cap > 0) ji[0] = '\0';
    if (!yi || !ji || yi_cap == 0 || ji_cap == 0) return;
    if (!date_valid(year, month, day)) return;

    int duty = ganzhi_day_index(year, month, day) % 12;
    copy_utf8_fit(yi, yi_cap, YI_TABLE[duty]);
    copy_utf8_fit(ji, ji_cap, JI_TABLE[duty]);
}

// ---------------------------------------------------------------------------
// 时间进度
// ---------------------------------------------------------------------------

void app_time_progress(app_scale_t scale, app_precision_t prec,
                       const app_datetime_t *now,
                       int birth_year, int birth_month, int birth_day, int life_expectancy,
                       int *total, int *filled)
{
    if (total) *total = 0;
    if (filled) *filled = 0;
    if (!total || !filled || !now) return;

    int scale_value = (int)scale;
    int prec_value = (int)prec;
    if (scale_value < 0 || scale_value >= (int)APP_SCALE_COUNT) return;
    if (prec_value < 0 || prec_value >= (int)APP_PREC_COUNT) return;
    if (!app_time_valid(now)) return;

    int t = 0;
    int f = 0;

    switch (scale) {
    case APP_SCALE_DAY:
        if (prec == APP_PREC_COARSE) {
            t = 24;
            f = now->hour + 1;
        } else if (prec == APP_PREC_BALANCED) {
            t = 96;
            f = now->hour * 4 + now->minute / 15 + 1;
        } else {
            t = 288;
            f = now->hour * 12 + now->minute / 5 + 1;
        }
        break;

    case APP_SCALE_WEEK: {
        // 周一为一周之始：把 0=周日 转成 0=周一。
        int monday_index = (app_time_weekday(now->year, now->month, now->day) + 6) % 7;
        if (prec == APP_PREC_COARSE) {
            t = 7;
            f = monday_index + 1;
        } else if (prec == APP_PREC_BALANCED) {
            t = 56;
            f = monday_index * 8 + now->hour / 3 + 1;
        } else {
            t = 168;
            f = monday_index * 24 + now->hour + 1;
        }
        break;
    }

    case APP_SCALE_MONTH: {
        int days = app_time_days_in_month(now->year, now->month);
        if (prec == APP_PREC_COARSE) {
            t = days;
            f = now->day;
        } else if (prec == APP_PREC_BALANCED) {
            t = days * 4;
            f = (now->day - 1) * 4 + now->hour / 6 + 1;
        } else {
            t = days * 8;
            f = (now->day - 1) * 8 + now->hour / 3 + 1;
        }
        break;
    }

    case APP_SCALE_YEAR: {
        int doy = app_time_day_of_year(now->year, now->month, now->day);
        int days_in_year = app_time_is_leap_year(now->year) ? 366 : 365;
        if (prec == APP_PREC_COARSE) {
            t = 12;
            f = now->month;
        } else if (prec == APP_PREC_BALANCED) {
            t = 53;
            f = (doy - 1) / 7 + 1;
        } else {
            t = days_in_year;
            f = doy;
        }
        break;
    }

    case APP_SCALE_LIFE: {
        if (birth_year < 1970 || birth_month < 1 || birth_month > 12 ||
            birth_day < 1 || birth_day > app_time_days_in_month(birth_year, birth_month) ||
            life_expectancy <= 0) {
            return;
        }

        long start = days_from_civil(birth_year, birth_month, birth_day);
        long now_days = days_from_civil(now->year, now->month, now->day);
        if (now_days < start) return;

        int end_year = birth_year + life_expectancy;
        int end_day = birth_day;
        int end_dim = app_time_days_in_month(end_year, birth_month);
        if (end_day > end_dim) end_day = end_dim;
        long end = days_from_civil(end_year, birth_month, end_day);
        long span_days = end - start;
        if (span_days <= 0) return;

        long long elapsed = (long long)(now_days - start) * 86400LL
                          + now->hour * 3600LL + now->minute * 60LL + now->second;
        long long span = (long long)span_days * 86400LL;
        if (elapsed < 0) elapsed = 0;
        if (elapsed > span) elapsed = span;

        if (prec == APP_PREC_COARSE) {
            t = life_expectancy;
        } else if (prec == APP_PREC_BALANCED) {
            t = life_expectancy * 4;
        } else {
            t = life_expectancy * 12;
        }
        f = (int)(elapsed * (long long)t / span) + 1;
        break;
    }

    default:
        return;
    }

    if (f < 0) f = 0;
    if (f > t) f = t;
    *total = t;
    *filled = f;
}

// ---------------------------------------------------------------------------
// 名称
// ---------------------------------------------------------------------------

const char *app_scale_name(app_scale_t scale)
{
    static const char *const NAMES[] = {"今日", "本周", "本月", "今年", "人生"};
    int value = (int)scale;
    if (value < 0 || value >= (int)APP_SCALE_COUNT) return "??";
    return NAMES[value];
}

const char *app_precision_name(app_precision_t prec)
{
    static const char *const NAMES[] = {"总览", "均衡", "精细"};
    int value = (int)prec;
    if (value < 0 || value >= (int)APP_PREC_COUNT) return "??";
    return NAMES[value];
}
