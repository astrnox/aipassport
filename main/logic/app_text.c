#include "app_text.h"

#include <stdio.h>
#include <string.h>

// 返回该字节在 UTF-8 序列中的长度；非法起始字节按 1 处理。
static size_t utf8_seq_len(unsigned char byte)
{
    if (byte < 0x80) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;
}

// 判断以 text 开头的序列是否合法（后续字节必须是 10xxxxxx，且不越界）。
static bool utf8_seq_valid(const char *text, size_t remaining)
{
    size_t len = utf8_seq_len((unsigned char)text[0]);
    if (len == 1) return (unsigned char)text[0] < 0x80;
    if (len > remaining) return false;
    for (size_t i = 1; i < len; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) return false;
    }
    return true;
}

bool app_utf8_valid(const char *text)
{
    if (!text) return false;
    size_t remaining = strlen(text);
    const char *cursor = text;
    while (remaining > 0) {
        size_t len = utf8_seq_len((unsigned char)cursor[0]);
        if (!utf8_seq_valid(cursor, remaining)) return false;
        cursor += len;
        remaining -= len;
    }
    return true;
}

size_t app_utf8_chars(const char *text)
{
    if (!text) return 0;
    size_t remaining = strlen(text);
    const char *cursor = text;
    size_t count = 0;
    while (remaining > 0) {
        size_t len = utf8_seq_len((unsigned char)cursor[0]);
        if (len > remaining) len = 1;
        cursor += len;
        remaining -= len;
        count++;
    }
    return count;
}

size_t app_utf8_copy_prefix(const char *text, size_t max_chars, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    if (!text) return 0;

    size_t remaining = strlen(text);
    const char *cursor = text;
    size_t written = 0;
    size_t chars = 0;
    while (remaining > 0 && chars < max_chars) {
        size_t len = utf8_seq_len((unsigned char)cursor[0]);
        if (len > remaining) len = 1;
        if (written + len + 1 > out_cap) break;
        memcpy(out + written, cursor, len);
        written += len;
        cursor += len;
        remaining -= len;
        chars++;
    }
    out[written] = '\0';
    return written;
}

int app_fmt_hhmm(char *out, size_t cap, int minutes_of_day)
{
    if (!out || cap < 6) return -1;
    if (minutes_of_day < 0) minutes_of_day = 0;
    if (minutes_of_day > 24 * 60) minutes_of_day = 24 * 60;
    return snprintf(out, cap, "%02d:%02d", minutes_of_day / 60, minutes_of_day % 60);
}

int app_fmt_countdown(char *out, size_t cap, int seconds)
{
    if (!out || cap < 6) return -1;
    if (seconds < 0) seconds = -seconds;
    if (seconds >= 3600) {
        return snprintf(out, cap, "%d:%02d:%02d", seconds / 3600,
                        (seconds / 60) % 60, seconds % 60);
    }
    return snprintf(out, cap, "%02d:%02d", seconds / 60, seconds % 60);
}

int app_fmt_date_short(char *out, size_t cap, int month, int day, int weekday)
{
    if (!out || cap < 16) return -1;
    return snprintf(out, cap, "%s %d/%d", app_weekday_name(weekday), month, day);
}

int app_fmt_year_month(char *out, size_t cap, int year, int month)
{
    if (!out || cap < 16) return -1;
    return snprintf(out, cap, "%d 年 %d 月", year, month);
}

const char *app_weekday_name(int weekday)
{
    // struct tm 约定：0 = 周日。越界时给问号，界面不会因为脏数据崩掉。
    static const char *const NAMES[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六",
    };
    if (weekday < 0 || weekday > 6) return "??";
    return NAMES[weekday];
}

static bool is_space_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char *app_text_trim(char *text)
{
    if (!text) return text;
    char *start = text;
    while (*start && is_space_byte(*start)) start++;
    size_t len = strlen(start);
    while (len > 0 && is_space_byte(start[len - 1])) start[--len] = '\0';
    // 全角空格（U+3000，EF BC 80）与不换行空格（U+00A0，C2 A0）也按空白处理。
    if (len >= 3 && (unsigned char)start[0] == 0xEF &&
        (unsigned char)start[1] == 0xBC && (unsigned char)start[2] == 0x80) {
        memmove(start, start + 3, len - 3 + 1);
        return app_text_trim(start);
    }
    if (len >= 2 && (unsigned char)start[0] == 0xC2 && (unsigned char)start[1] == 0xA0) {
        memmove(start, start + 2, len - 2 + 1);
        return app_text_trim(start);
    }
    return start;
}

bool app_text_split_kv(const char *line, char *key, size_t key_cap,
                       char *value, size_t value_cap)
{
    if (!line || !key || !value || key_cap == 0 || value_cap == 0) return false;
    key[0] = '\0';
    value[0] = '\0';

    char buffer[192];
    size_t len = strlen(line);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, line, len);
    buffer[len] = '\0';

    // '=' ':' 为半角，'：' 为全角（EF BC 9A）。
    const char *separator = NULL;
    for (const char *cursor = buffer; *cursor; cursor++) {
        if (*cursor == '=' || *cursor == ':') {
            separator = cursor;
            break;
        }
        if ((unsigned char)cursor[0] == 0xEF && (unsigned char)cursor[1] == 0xBC &&
            (unsigned char)cursor[2] == 0x9A) {
            separator = cursor;
            break;
        }
    }
    if (!separator) return false;

    size_t separator_len = ((unsigned char)separator[0] == 0xEF) ? 3 : 1;
    size_t key_len = (size_t)(separator - buffer);
    char *key_start = buffer;
    while (key_len > 0 && is_space_byte(*key_start)) {
        key_start++;
        key_len--;
    }
    while (key_len > 0 && is_space_byte(key_start[key_len - 1])) key_len--;

    char *value_start = (char *)separator + separator_len;
    char *trimmed_value = app_text_trim(value_start);

    size_t copy = key_len < key_cap - 1 ? key_len : key_cap - 1;
    memcpy(key, key_start, copy);
    key[copy] = '\0';

    strncpy(value, trimmed_value, value_cap - 1);
    value[value_cap - 1] = '\0';
    return true;
}
