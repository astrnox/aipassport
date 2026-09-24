// main/logic/app_reminder.h —— 本地提醒的存储模型与到点判定（与硬件无关）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "logic/app_time.h"

#define APP_REMINDER_MAX 16

// 单条提醒。重复方式二选一：按周重复（weekday_mask）或一次性日期（year/month/day）。
typedef struct {
    bool    enabled;
    int     hour;          // 0..23
    int     minute;        // 0..59
    bool    repeat_weekly; // true 时按 weekday_mask 每周重复
    uint8_t weekday_mask;  // bit0=周日 .. bit6=周六
    int     year, month, day;  // repeat_weekly == false 时的一次性日期
    char    label[24];
} app_reminder_t;

// 定长存储的提醒列表，count 为有效条数。
typedef struct {
    app_reminder_t items[APP_REMINDER_MAX];
    int count;
} app_reminder_list_t;

// 清空列表。
void app_reminder_list_init(app_reminder_list_t *list);

// 追加一条提醒，返回新索引；seed 为 NULL 时插入默认项（禁用、08:00、每天）。
// 列表已满返回 -1。
int  app_reminder_add(app_reminder_list_t *list, const app_reminder_t *seed);

// 删除指定索引并前移后续项；索引非法返回 false。
bool app_reminder_remove(app_reminder_list_t *list, int index);

// 距下一次触发还有多少秒；永不触发返回 -1。now 为本地时间，now_weekday 0=周日。
int  app_reminder_next_seconds(const app_reminder_t *r, const app_datetime_t *now, int now_weekday);

// now 所在分钟内是否应触发。
bool app_reminder_due(const app_reminder_t *r, const app_datetime_t *now, int now_weekday);

// 形如 "每天" / "周一 周三" / "2026-09-24"。返回写入字节数（不含结尾 NUL），失败 -1。
int  app_reminder_schedule_text(const app_reminder_t *r, char *out, size_t cap);
