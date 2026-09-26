// main/logic/app_badge.c —— 电子工牌数据模型的实现（与硬件无关）。
#include "app_badge.h"
#include "app_qr.h"
#include "app_text.h"

#include <stdio.h>
#include <string.h>

void app_badge_list_init(app_badge_list_t *l)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->count = 0;
    l->selected = 0;
}

int app_badge_add(app_badge_list_t *l)
{
    if (!l) return -1;
    if (l->count >= APP_BADGE_MAX) return -1;

    int index = l->count;
    app_badge_t *b = &l->items[index];
    memset(b, 0, sizeof(*b));
    b->used = true;
    b->anim_slot = -1;
    strncpy(b->nickname, "未命名", sizeof(b->nickname) - 1);
    b->nickname[sizeof(b->nickname) - 1] = '\0';
    l->count++;
    return index;
}

bool app_badge_remove(app_badge_list_t *l, int index)
{
    if (!l || index < 0 || index >= l->count) return false;

    for (int i = index; i < l->count - 1; i++) l->items[i] = l->items[i + 1];
    memset(&l->items[l->count - 1], 0, sizeof(l->items[0]));
    l->count--;

    if (l->count == 0) {
        l->selected = 0;
    } else {
        if (l->selected >= l->count) l->selected = l->count - 1;
        if (l->selected < 0) l->selected = 0;
    }
    return true;
}

void app_badge_cycle(app_badge_list_t *l, int delta)
{
    if (!l || l->count <= 0) return;

    int n = l->count;
    int sel = l->selected % n;
    if (sel < 0) sel += n;

    int step = delta % n;
    int next = (sel + step) % n;
    if (next < 0) next += n;

    l->selected = next;
}

bool app_badge_set_text(app_badge_t *b, const char *nickname,
                        const char *const *lines, int line_count)
{
    if (!b) return false;

    // 先校验，避免把半个汉字写进缓冲区。
    if (nickname && !app_utf8_valid(nickname)) return false;
    if (lines) {
        for (int i = 0; i < line_count && i < APP_BADGE_MAX_LINES; i++) {
            if (lines[i] && !app_utf8_valid(lines[i])) return false;
        }
    }

    app_utf8_copy_prefix(nickname ? nickname : "", 8, b->nickname, sizeof(b->nickname));

    for (int i = 0; i < APP_BADGE_MAX_LINES; i++) {
        const char *src = "";
        if (lines && i < line_count && lines[i]) src = lines[i];
        app_utf8_copy_prefix(src, 12, b->lines[i], sizeof(b->lines[i]));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 二维码
// ---------------------------------------------------------------------------

// 复制一段用户输入到定长缓冲：非法 UTF-8 或空内容直接拒绝，不写入半截字符。
static bool copy_text(const char *src, char *out, size_t out_cap, size_t max_chars)
{
    if (!src || !src[0]) return false;
    if (!app_utf8_valid(src)) return false;

    app_utf8_copy_prefix(src, max_chars, out, out_cap);
    return out[0] != '\0';
}

int app_badge_qr_add(app_badge_t *b, const char *label, const char *text)
{
    if (!b) return -1;
    if (b->qr_count >= APP_BADGE_QR_MAX) return -1;

    int index = b->qr_count;
    char text_buf[APP_BADGE_QR_TEXT_LEN];
    char label_buf[APP_BADGE_QR_LABEL_LEN];

    // 先全部校验再落盘，避免只写进一半。
    if (!copy_text(text, text_buf, sizeof(text_buf), APP_QR_MAX_BYTES)) return -1;
    label_buf[0] = '\0';
    if (label && label[0]) {
        if (!app_utf8_valid(label)) return -1;
        app_utf8_copy_prefix(label, 4, label_buf, sizeof(label_buf));
    }

    memcpy(b->qr_text[index], text_buf, sizeof(b->qr_text[index]));
    memcpy(b->qr_label[index], label_buf, sizeof(b->qr_label[index]));
    b->qr_count++;
    return index;
}

bool app_badge_qr_set(app_badge_t *b, int index, const char *label, const char *text)
{
    if (!b || index < 0 || index >= b->qr_count) return false;

    char text_buf[APP_BADGE_QR_TEXT_LEN];
    char label_buf[APP_BADGE_QR_LABEL_LEN];

    if (!copy_text(text, text_buf, sizeof(text_buf), APP_QR_MAX_BYTES)) return false;
    label_buf[0] = '\0';
    if (label && label[0]) {
        if (!app_utf8_valid(label)) return false;
        app_utf8_copy_prefix(label, 4, label_buf, sizeof(label_buf));
    }

    memcpy(b->qr_text[index], text_buf, sizeof(b->qr_text[index]));
    memcpy(b->qr_label[index], label_buf, sizeof(b->qr_label[index]));
    return true;
}

bool app_badge_qr_remove(app_badge_t *b, int index)
{
    if (!b || index < 0 || index >= b->qr_count) return false;

    for (int i = index; i < b->qr_count - 1; i++) {
        memcpy(b->qr_text[i], b->qr_text[i + 1], sizeof(b->qr_text[i]));
        memcpy(b->qr_label[i], b->qr_label[i + 1], sizeof(b->qr_label[i]));
    }
    b->qr_count--;
    memset(b->qr_text[b->qr_count], 0, sizeof(b->qr_text[0]));
    memset(b->qr_label[b->qr_count], 0, sizeof(b->qr_label[0]));
    return true;
}

int app_badge_qr_count(const app_badge_t *b)
{
    if (!b) return 0;
    if (b->qr_count < 0) return 0;
    return b->qr_count > APP_BADGE_QR_MAX ? APP_BADGE_QR_MAX : b->qr_count;
}

const char *app_badge_qr_text(const app_badge_t *b, int index)
{
    if (!b || index < 0 || index >= app_badge_qr_count(b)) return NULL;
    return b->qr_text[index][0] ? b->qr_text[index] : NULL;
}

const char *app_badge_qr_label(const app_badge_t *b, int index, char *scratch,
                               size_t scratch_cap)
{
    if (!b || index < 0 || index >= app_badge_qr_count(b)) return NULL;
    if (b->qr_label[index][0]) return b->qr_label[index];

    if (scratch && scratch_cap > 0) {
        snprintf(scratch, scratch_cap, "二维码 %d", index + 1);
        return scratch;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// 动图
// ---------------------------------------------------------------------------

bool app_badge_set_anim(app_badge_t *b, int slot)
{
    if (!b) return false;
    if (slot < -1 || slot >= APP_ANIM_SLOT_MAX) return false;

    b->anim_slot = slot;
    return true;
}
