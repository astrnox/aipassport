// main/logic/app_novel.c —— 离线小说阅读纯逻辑实现。约定与边界见 app_novel.h。
#include "app_novel.h"

#include "app_text.h"

#include <string.h>

// ---------------------------------------------------------------------------
// UTF-8 解码
// --------------------------------------------------------------------------- 

// 解一个码点并把 pos 推到下一个字符边界。非法序列按单字节前进（返回 U+FFFD），
// 保证任何输入都能在一个有限步数内走完，不会死循环。
uint32_t app_novel_utf8_next(const char *text, size_t len, size_t *pos)
{
    const uint8_t *p = (const uint8_t *)text + *pos;
    size_t remain = len - *pos;
    uint8_t b = p[0];

    if (b < 0x80) {
        (*pos)++;
        return b;
    }

    int need;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0) {
        need = 1;
        cp = b & 0x1Fu;
    } else if ((b & 0xF0) == 0xE0) {
        need = 2;
        cp = b & 0x0Fu;
    } else if ((b & 0xF8) == 0xF0) {
        need = 3;
        cp = b & 0x07u;
    } else {
        (*pos)++;
        return 0xFFFDu;
    }

    if ((size_t)need + 1 > remain) {
        (*pos)++;
        return 0xFFFDu;
    }
    for (int i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            (*pos)++;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    *pos += (size_t)need + 1;
    return cp;
}

static uint32_t utf8_next(const char *text, size_t len, size_t *pos)
{
    return app_novel_utf8_next(text, len, pos);
}

int app_novel_char_units(uint32_t cp)
{
    if (cp == '\r') return 0;      // CRLF 文本的 '\r' 不计宽度，排版与 LF 文本一致
    if (cp == '\t') return 4;

    // 全角/宽字符判据（wcwidth 的常用区间）。判窄只会让那一行多放一两个字，
    // 不会破坏"不劈开汉字"的约束，因此不必穷举到每个区间都精确。
    if (cp >= 0x1100 &&
        (cp <= 0x115F ||
         cp == 0x2329 || cp == 0x232A ||
         (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) ||
         (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6))) {
        return 2;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 分区头部（纯计算部分）
// ---------------------------------------------------------------------------

void app_novel_header_init(app_novel_header_t *header, const char *title,
                           uint32_t data_bytes, uint32_t data_crc, uint16_t chapter_count)
{
    if (!header) return;
    memset(header, 0, sizeof(*header));
    header->magic = APP_NOVEL_MAGIC;
    header->version = APP_NOVEL_VERSION;
    header->data_bytes = data_bytes;
    header->data_crc = data_crc;
    header->chapter_count = chapter_count > APP_NOVEL_MAX_CHAPTERS
                                ? APP_NOVEL_MAX_CHAPTERS : chapter_count;
    app_utf8_copy_prefix(title ? title : "", 8, header->title, sizeof(header->title));
}

app_novel_status_t app_novel_header_check(const app_novel_header_t *header, uint32_t capacity,
                                          uint32_t *data_bytes)
{
    if (!header) return APP_NOVEL_ERR_HEADER;
    if (header->magic != APP_NOVEL_MAGIC) return APP_NOVEL_ERR_HEADER;
    if (header->version != APP_NOVEL_VERSION) return APP_NOVEL_ERR_HEADER;
    if (header->chapter_count > APP_NOVEL_MAX_CHAPTERS) return APP_NOVEL_ERR_HEADER;
    if (header->data_bytes == 0) return APP_NOVEL_ERR_EMPTY;
    if (header->data_bytes > capacity) return APP_NOVEL_ERR_CAPACITY;
    // 标题不强制要求以 NUL 结尾（头部可能是旧的写入），但必须能读出内容。
    if (data_bytes) *data_bytes = header->data_bytes;
    return APP_NOVEL_OK;
}

// ---------------------------------------------------------------------------
// 分页
// ---------------------------------------------------------------------------

size_t app_novel_page_next(const char *text, size_t len, size_t off,
                           const app_novel_layout_t *layout)
{
    if (!text || !layout) return off;
    if (layout->units_per_line <= 0 || layout->lines_per_page <= 0) return off;
    if (off >= len) return len;

    size_t pos = off;
    int lines = 0;      // 已经排满的行数
    int units = 0;      // 当前行已占的单位

    while (pos < len) {
        size_t ch_start = pos;
        uint32_t cp = utf8_next(text, len, &pos);

        if (cp == '\n') {
            lines++;
            units = 0;
            if (lines >= layout->lines_per_page) return pos;   // 换行符归本页
            continue;
        }

        int u = app_novel_char_units(cp);
        if (units > 0 && units + u > layout->units_per_line) {
            // 本行放不下这个字符：当前行结束，字符本身留给下一行。
            lines++;
            units = 0;
            if (lines >= layout->lines_per_page) return ch_start;
            units = u;
            continue;
        }
        units += u;
    }
    return len;
}

size_t app_novel_page_prev(const char *text, size_t len, size_t off,
                           const app_novel_layout_t *layout)
{
    if (!text || !layout || off == 0) return 0;

    // 逐页向前走直到越过 off。一本 1MB 的书约七八百页，单次按键的扫描量在毫秒级，
    // 换来的是"往回翻页与往前翻页永远落在同一批页边界上"，不会因为反向算法不同
    // 而出现漏页或跳页。
    size_t prev = 0;
    size_t cur = 0;
    while (cur < off) {
        size_t next = app_novel_page_next(text, len, cur, layout);
        if (next <= cur) break;     // 异常布局：防死循环
        prev = cur;
        cur = next;
    }
    return prev;
}

size_t app_novel_page_floor(const char *text, size_t len, size_t off,
                            const app_novel_layout_t *layout)
{
    if (!text || !layout) return 0;
    if (len == 0) return 0;
    // off 是"从哪里开始读"，等于 len 表示读完了；此时应吸附到最后一页，而不是
    // 一个空页。因此按最后一个字节所在的位置来算。
    if (off >= len) off = len - 1;
    if (off == 0) return 0;

    size_t page = 0;
    while (page < off && page < len) {
        size_t next = app_novel_page_next(text, len, page, layout);
        if (next <= page) break;        // 异常布局：防死循环
        if (next > off) break;          // off 落在这一页里
        page = next;
    }
    return page;
}

// ---------------------------------------------------------------------------
// 章节识别
// ---------------------------------------------------------------------------

static bool is_digit_or_numeral(uint32_t cp)
{
    if (cp >= '0' && cp <= '9') return true;
    switch (cp) {
    case 0x4E00:    // 一
    case 0x4E8C:    // 二
    case 0x4E09:    // 三
    case 0x56DB:    // 四
    case 0x4E94:    // 五
    case 0x516D:    // 六
    case 0x4E03:    // 七
    case 0x516B:    // 八
    case 0x4E5D:    // 九
    case 0x5341:    // 十
    case 0x767E:    // 百
    case 0x5343:    // 千
    case 0x96F6:    // 零
    case 0x4E24:    // 两
        return true;
    default:
        return false;
    }
}

static bool is_chapter_keyword(uint32_t cp)
{
    return cp == 0x7AE0 ||    // 章
           cp == 0x56DE ||    // 回
           cp == 0x8282 ||    // 节
           cp == 0x5377 ||    // 卷
           cp == 0x7BC7;      // 篇
}

// 统计 [s, s+n) 里的字符数，最多数到 limit+1 个就停（只用来做"太长"的判断，
// 不能像 app_utf8_chars 那样读到 NUL——这里的一行是正文中间的一段，没有结尾符）。
static int utf8_count_capped(const char *s, size_t n, int limit)
{
    size_t i = 0;
    int count = 0;
    while (i < n && count <= limit) {
        utf8_next(s, n, &i);
        count++;
    }
    return count;
}

// 判断一行是不是章节标题。规则：行首（允许前导空格/制表符）是"第"，其后 12 个字符
// 内出现数字或汉字数字，前 16 个字符内出现"章/回/节/卷/篇"，整行不超过 40 个字符。
// 三个条件同时收紧，才不会把正文里"他想起第三章里的那句话"这种句子认成标题。
static bool line_is_chapter(const char *line, size_t n)
{
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n) return false;

    size_t scan = i;
    uint32_t cp = utf8_next(line, n, &scan);
    if (cp != 0x7B2C) return false;                  // 第

    bool has_number = false;
    bool has_keyword = false;
    int count = 1;
    while (scan < n && count <= 16) {
        cp = utf8_next(line, n, &scan);
        if (cp == '\n' || cp == '\r') break;
        count++;
        if (is_chapter_keyword(cp)) {
            has_keyword = true;
            break;
        }
        if (count <= 13 && is_digit_or_numeral(cp)) has_number = true;
    }
    if (!has_number || !has_keyword) return false;

    // 标题行不会很长；正文里偶然出现的"第…章"句子通常远超 40 个字符。
    return utf8_count_capped(line + i, n - i, 40) <= 40;
}

size_t app_novel_next_chapter(const char *text, size_t len, size_t from,
                              char *title, size_t title_cap)
{
    if (!text) return len;
    if (from >= len) return len;

    size_t line_start = from;
    while (line_start < len) {
        size_t p = line_start;
        while (p < len && text[p] != '\n') p++;
        size_t line_len = p - line_start;        // 不含 '\n'

        if (line_len > 0 && line_is_chapter(text + line_start, line_len)) {
            if (title && title_cap) {
                // 先把这一行拷成独立字符串再裁剪：正文里的"一行"没有结尾符，直接交给
                // 字符串工具会越行读到下一行去。
                char buf[APP_NOVEL_MAX_LINE + 1];
                size_t n = line_len < APP_NOVEL_MAX_LINE ? line_len : APP_NOVEL_MAX_LINE;
                memcpy(buf, text + line_start, n);
                buf[n] = '\0';
                app_utf8_copy_prefix(app_text_trim(buf), 10, title, title_cap);
            }
            return line_start;
        }

        if (p >= len) break;
        line_start = p + 1;                      // 跳过 '\n'
    }
    return len;
}

// ---------------------------------------------------------------------------
// 上传期流式 UTF-8 校验
// ---------------------------------------------------------------------------

bool app_novel_utf8_feed(uint32_t *state, const uint8_t *data, size_t len)
{
    if (!state) return false;
    if (!data && len) return false;

    uint32_t need = *state & 0xFFu;     // 还差几个续字节
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (need > 0) {
            if ((b & 0xC0) != 0x80) {
                *state = 0;
                return false;
            }
            need--;
            continue;
        }
        if (b < 0x80) continue;
        if ((b & 0xE0) == 0xC0) need = 1;
        else if ((b & 0xF0) == 0xE0) need = 2;
        else if ((b & 0xF8) == 0xF0) need = 3;
        else {
            *state = 0;
            return false;
        }
    }
    *state = need;
    return true;
}

bool app_novel_utf8_finish(uint32_t state)
{
    return state == 0;
}