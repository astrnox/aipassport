// main/logic/app_reminder.c —— 本地提醒的存储与到点判定实现。
#include "app_reminder.h"

#include <stdio.h>
#include <string.h>

#define WEEKDAY_ALL_MASK 0x7Fu   // bit0..bit6 全置位 = 每天

void app_reminder_list_init(app_reminder_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
    list->count = 0;
}

int app_reminder_add(app_reminder_list_t *list, const app_reminder_t *seed)
{
    if (!list) return -1;
    if (list->count >= APP_REMINDER_MAX) return -1;

    app_reminder_t *slot = &list->items[list->count];
    if (seed) {
        *slot = *seed;
    } else {
        // 默认项：禁用、08:00、每天重复，等用户去配置页打开。
        memset(slot, 0, sizeof(*slot));
        slot->enabled = false;
        slot->hour = 8;
        slot->minute = 0;
        slot->repeat_weekly = true;
        slot->weekday_mask = WEEKDAY_ALL_MASK;
    }
    return list->count++;
}

bool app_reminder_remove(app_reminder_list_t *list, int index)
{
    if (!list || index < 0 || index >= list->count) return false;

    // 用后一项覆盖前一项，把尾部整体前移。
    for (int i = index; i < list->count - 1; i++) {
        list->items[i] = list->items[i + 1];
    }
    list->count--;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));
    return true;
}

// 把 now_weekday 规范到 0..6，容忍调用方传入越界值。
static int normalize_weekday(int weekday)
{
    return ((weekday % 7) + 7) % 7;
}

// 判断日期 (ay,am,ad) 是否严格晚于 (by,bm,bd)。
static bool date_after(int ay, int am, int ad, int by, int bm, int bd)
{
    if (ay != by) return ay > by;
    if (am != bm) return am > bm;
    return ad > bd;
}

// 按周重复：在接下来 7 天（含今天）里找第一个命中且时刻严格晚于 now 的日子。
// 若唯一命中的就是今天且时刻已过，则顺延到 8 天窗口末端的下周同一天。
static int weekly_next_seconds(const app_reminder_t *r, const app_datetime_t *now, int now_weekday)
{
    int now_minute = now->hour * 60 + now->minute;
    int target_minute = r->hour * 60 + r->minute;
    int base_weekday = normalize_weekday(now_weekday);

    for (int offset = 0; offset <= 7; offset++) {
        int weekday = normalize_weekday(base_weekday + offset);
        if (!(r->weekday_mask & (uint8_t)(1u << weekday))) continue;
        // 今天命中但时刻已过（含正好此刻）则顺延到下一次。
        if (offset == 0 && target_minute <= now_minute) continue;
        return offset * 86400 + (target_minute - now_minute) * 60 - now->second;
    }
    return -1;
}

// 一次性提醒：日期在今天就比时刻，在未来则逐日推进求天数差。
static int onetime_next_seconds(const app_reminder_t *r, const app_datetime_t *now)
{
    if (!date_after(r->year, r->month, r->day, now->year, now->month, now->day)) {
        // 目标日期早于今天，或就是今天但时刻已过。
        if (r->year != now->year || r->month != now->month || r->day != now->day) {
            return -1;
        }
        int now_minute = now->hour * 60 + now->minute;
        int target_minute = r->hour * 60 + r->minute;
        if (target_minute <= now_minute) return -1;
        return (target_minute - now_minute) * 60 - now->second;
    }

    // 目标在未来：用 app_time_add_days 逐日推进，天然处理跨月/跨年。
    for (int days = 1;; days++) {
        int year, month, day;
        if (!app_time_add_days(now->year, now->month, now->day, days, &year, &month, &day)) {
            return -1;   // 超出可表示范围
        }
        if (year == r->year && month == r->month && day == r->day) {
            int now_minute = now->hour * 60 + now->minute;
            int target_minute = r->hour * 60 + r->minute;
            return days * 86400 + (target_minute - now_minute) * 60 - now->second;
        }
        // 目标日期非法或已越过（add_days 会跳过不存在的日期）则视为永不触发。
        if (date_after(year, month, day, r->year, r->month, r->day)) return -1;
    }
}

int app_reminder_next_seconds(const app_reminder_t *r, const app_datetime_t *now, int now_weekday)
{
    if (!r || !now) return -1;
    if (!r->enabled) return -1;
    if (r->repeat_weekly) {
        return weekly_next_seconds(r, now, now_weekday);
    }
    return onetime_next_seconds(r, now);
}

bool app_reminder_due(const app_reminder_t *r, const app_datetime_t *now, int now_weekday)
{
    if (!r || !now) return false;
    if (!r->enabled) return false;
    if (r->hour != now->hour || r->minute != now->minute) return false;

    if (r->repeat_weekly) {
        int weekday = normalize_weekday(now_weekday);
        return (r->weekday_mask & (uint8_t)(1u << weekday)) != 0;
    }
    return r->year == now->year && r->month == now->month && r->day == now->day;
}

int app_reminder_schedule_text(const app_reminder_t *r, char *out, size_t cap)
{
    if (!r || !out || cap == 0) return -1;
    out[0] = '\0';

    if (!r->repeat_weekly) {
        return snprintf(out, cap, "%04d-%02d-%02d", r->year, r->month, r->day);
    }
    if ((r->weekday_mask & WEEKDAY_ALL_MASK) == WEEKDAY_ALL_MASK) {
        return snprintf(out, cap, "每天");
    }

    static const char *const NAMES[7] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六",
    };
    size_t used = 0;
    bool first = true;
    for (int i = 0; i < 7; i++) {
        if (!(r->weekday_mask & (uint8_t)(1u << i))) continue;
        const char *name = NAMES[i];
        size_t name_len = strlen(name);
        size_t separator = first ? 0 : 1;
        // 预留给结尾 NUL，避免越界写。
        if (used + separator + name_len + 1 > cap) break;
        if (separator) out[used++] = ' ';
        memcpy(out + used, name, name_len);
        used += name_len;
        out[used] = '\0';
        first = false;
    }
    return (int)used;
}

int app_reminder_missed(const app_reminder_list_t *list,
                        int64_t from_unix, int64_t to_unix,
                        int utc_offset_minutes,
                        char (*times)[6], int max)
{
    if (!list || !times || max <= 0) return 0;
    if (from_unix <= 0 || to_unix <= from_unix) return 0;

    // 只回溯固定窗口，且从整分钟之后开始，避免把基准那一分钟本身算进来。
    int64_t start = from_unix;
    if (to_unix - start > APP_REMINDER_MISSED_WINDOW_S) {
        start = to_unix - APP_REMINDER_MISSED_WINDOW_S;
    }
    start -= start % 60;

    int64_t offset = (int64_t)utc_offset_minutes * 60;
    int  minutes_of_day[APP_REMINDER_MAX];   // 每条提醒最近一次命中的时刻
    int  slot[APP_REMINDER_MAX];             // 提醒下标 -> 输出位置
    bool hit[APP_REMINDER_MAX];
    memset(hit, 0, sizeof(hit));

    int found = 0;
    for (int64_t t = start + 60; t <= to_unix; t += 60) {
        app_datetime_t dt;
        if (!app_time_from_unix(t + offset, &dt)) continue;
        int weekday = app_time_weekday(dt.year, dt.month, dt.day);
        if (weekday < 0) continue;

        for (int i = 0; i < list->count; i++) {
            if (!app_reminder_due(&list->items[i], &dt, weekday)) continue;
            if (hit[i]) {
                // 同一条提醒在同一窗口内命中多次时保留最近一次。
                minutes_of_day[slot[i]] = dt.hour * 60 + dt.minute;
                continue;
            }
            if (found >= max) continue;
            hit[i] = true;
            slot[i] = found;
            minutes_of_day[found] = dt.hour * 60 + dt.minute;
            found++;
        }
    }

    for (int i = 0; i < found; i++) {
        // 先夹到 0..23:59，让编译器能确定格式化长度不超过缓冲（6 字节）。
        int mo = minutes_of_day[i];
        if (mo < 0) mo = 0;
        if (mo > 23 * 60 + 59) mo = 23 * 60 + 59;
        snprintf(times[i], 6, "%02d:%02d", mo / 60, mo % 60);
    }
    return found;
}
