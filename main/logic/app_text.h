// main/logic/app_text.h —— 与硬件无关的文本与时间格式化工具。
//
// 本层不依赖 ESP-IDF 与 LVGL，可在主机上直接编译测试。界面层与配置页共用同一
// 套格式化，避免同一个时间在两处显示成不同样子。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// UTF-8 字符数（不按字节算）。非法字节序列按单字节字符计数，不会死循环。
size_t app_utf8_chars(const char *text);

// 把 text 的前 max_chars 个字符复制到 out（始终以 NUL 结尾，且不在字符中间截断）。
// 返回写入的字节数（不含结尾 NUL）。用于工牌文本、作息节点名等用户输入。
size_t app_utf8_copy_prefix(const char *text, size_t max_chars, char *out, size_t out_cap);

// 校验是否为合法 UTF-8；用于配置页导入数据，避免把半个汉字存进 NVS。
bool app_utf8_valid(const char *text);

// "09:40"（一天内分钟数）。cap 不足返回 -1。
int app_fmt_hhmm(char *out, size_t cap, int minutes_of_day);

// 倒计时：小于一小时用 "12:36"，否则 "1:02:03"。秒数取绝对值。
int app_fmt_countdown(char *out, size_t cap, int seconds);

// "周三 9/24"
int app_fmt_date_short(char *out, size_t cap, int month, int day, int weekday);

// "2026 年 9 月"
int app_fmt_year_month(char *out, size_t cap, int year, int month);

// 周一..周日；越界返回 "??"。
const char *app_weekday_name(int weekday);

// 把一行结构化文本拆成两段：左边是键，右边是值。分隔符为 '=' 或 ':'（全角也支持）。
// 返回 false 表示没有分隔符。两端空白会被去掉，键与值分别复制到 key/value。
bool app_text_split_kv(const char *line, char *key, size_t key_cap,
                       char *value, size_t value_cap);

// 去掉字符串两端的空白（含全角空格），原地修改并返回首指针。
char *app_text_trim(char *text);
