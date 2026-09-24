// main/logic/app_badge.c —— 电子工牌数据模型的实现（与硬件无关）。
#include "app_badge.h"
#include "app_text.h"

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
