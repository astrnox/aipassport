// main/logic/app_vcard.c —— vCard 3.0 文本生成实现。
#include "app_vcard.h"

#include "app_text.h"

// 逐字节写入，始终为终止符留 1 字节。容量不够就返回 false，由调用方退到更短的版本。
static bool put(char *out, size_t cap, size_t *used, char c)
{
    if (*used + 1 >= cap) return false;
    out[(*used)++] = c;
    return true;
}

// 追加一行 "KEY:VALUE" + CRLF（vCard 规定换行是 CRLF）。值里的 '\' ',' ';' 转义，
// 换行折成空格：不这样做的话，带逗号的单位名会被读卡方当成两个字段。
static bool append_field(char *out, size_t cap, size_t *used, const char *key, const char *value)
{
    size_t i = *used;
    for (const char *p = key; *p; p++) {
        if (!put(out, cap, &i, *p)) return false;
    }
    if (!put(out, cap, &i, ':')) return false;

    for (const char *p = value; *p; p++) {
        char c = *p;
        if (c == '\\' || c == ',' || c == ';') {
            if (!put(out, cap, &i, '\\')) return false;
            if (!put(out, cap, &i, c)) return false;
        } else if (c == '\r' || c == '\n') {
            if (!put(out, cap, &i, ' ')) return false;
        } else if (!put(out, cap, &i, c)) {
            return false;
        }
    }

    if (!put(out, cap, &i, '\r')) return false;
    if (!put(out, cap, &i, '\n')) return false;
    out[i] = '\0';
    *used = i;
    return true;
}

// 空字段允许缺省；非空则必须是合法 UTF-8，否则宁可不生成，也不把半个汉字写进二维码。
static bool field_ok(const char *text)
{
    return text[0] == '\0' || app_utf8_valid(text);
}

static int build_into(const app_vcard_t *card, char *out, size_t cap,
                      bool with_org, bool with_title, bool with_url)
{
    size_t used = 0;
    if (!append_field(out, cap, &used, "BEGIN", "VCARD")) return -1;
    if (!append_field(out, cap, &used, "VERSION", "3.0")) return -1;
    if (!append_field(out, cap, &used, "FN", card->name)) return -1;
    if (with_org && card->org[0] &&
        !append_field(out, cap, &used, "ORG", card->org)) return -1;
    if (with_title && card->title[0] &&
        !append_field(out, cap, &used, "TITLE", card->title)) return -1;
    if (card->tel[0] && !append_field(out, cap, &used, "TEL;TYPE=CELL", card->tel)) return -1;
    if (card->email[0] && !append_field(out, cap, &used, "EMAIL", card->email)) return -1;
    if (with_url && card->url[0] &&
        !append_field(out, cap, &used, "URL", card->url)) return -1;
    if (!append_field(out, cap, &used, "END", "VCARD")) return -1;
    return (int)used;
}

int app_vcard_build(const app_vcard_t *card, char *out, size_t cap, int *dropped_fields)
{
    if (dropped_fields) *dropped_fields = 0;
    if (!card || !out || cap == 0) return -1;
    if (!card->name[0] || !field_ok(card->name) || !field_ok(card->org) ||
        !field_ok(card->title) || !field_ok(card->tel) ||
        !field_ok(card->email) || !field_ok(card->url)) {
        return -1;
    }

    // 第 n 次尝试丢掉前 n 个可选字段，顺序固定为 网址 → 职务 → 单位。
    for (int attempt = 0; attempt < 4; attempt++) {
        int len = build_into(card, out, cap, attempt < 3, attempt < 2, attempt < 1);
        if (len > 0) {
            if (dropped_fields) *dropped_fields = attempt;
            return len;
        }
    }
    return -1;
}
