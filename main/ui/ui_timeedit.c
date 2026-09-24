// main/ui/ui_timeedit.c —— 数值字段编辑浮层的实现。
//
// 浮层挂在调用页面的屏幕上，随页面一起销毁；编辑期间独占按键，由调用方的按键处理
// 转发进来（见 ui_timeedit_handle）。浮层自身不持有页面状态，只负责按约定改数字。
#include "ui_timeedit.h"

#include "ui_theme.h"

#include <string.h>

#define TE_MAX_FIELDS 5
#define TE_CARD_W     (UI_W - 16)
#define TE_CARD_H     150
#define TE_FIELD_Y    84
#define TE_VALUE_H    26

static struct {
    lv_obj_t *overlay;
    lv_obj_t *value_lbl[TE_MAX_FIELDS];
    lv_obj_t *name_lbl[TE_MAX_FIELDS];
    lv_obj_t *box[TE_MAX_FIELDS];
    const ui_timeedit_field_t *fields;
    int *values;
    int count;
    int cursor;
    void (*done)(bool saved, void *user);
    void *user;
} s;

static void te_clamp(int index)
{
    const ui_timeedit_field_t *f = &s.fields[index];
    int v = s.values[index];
    if (v < f->min) v = f->min;
    if (v > f->max) v = f->max;
    s.values[index] = v;
}

static void te_render(void)
{
    for (int i = 0; i < s.count; i++) {
        if (s.value_lbl[i]) {
            const char *const *names = s.fields[i].names;
            if (names) {
                const char *txt = names[s.values[i]];
                lv_label_set_text(s.value_lbl[i], txt ? txt : "?");
            } else {
                lv_label_set_text_fmt(s.value_lbl[i], "%d", s.values[i]);
            }
            lv_obj_set_style_text_color(s.value_lbl[i],
                lv_color_hex(i == s.cursor ? ui_c_accent() : ui_c_text()), 0);
        }
        if (s.name_lbl[i]) {
            lv_obj_set_style_text_color(s.name_lbl[i],
                lv_color_hex(i == s.cursor ? ui_c_accent() : ui_c_dim()), 0);
        }
        if (s.box[i]) {
            lv_obj_set_style_border_color(s.box[i],
                lv_color_hex(i == s.cursor ? ui_c_accent() : ui_c_border()), 0);
            lv_obj_set_style_border_width(s.box[i], i == s.cursor ? 2 : 1, 0);
        }
    }
}

void ui_timeedit_open(lv_obj_t *parent, const char *title,
                      const ui_timeedit_field_t *fields, int *values, int count,
                      void (*done)(bool saved, void *user), void *user)
{
    if (!parent || !fields || !values || count <= 0) return;
    ui_timeedit_close();
    if (count > TE_MAX_FIELDS) count = TE_MAX_FIELDS;

    memset(&s, 0, sizeof(s));
    s.fields = fields;
    s.values = values;
    s.count = count;
    s.done = done;
    s.user = user;
    s.cursor = 0;

    lv_obj_t *ov = lv_obj_create(parent);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_size(ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    s.overlay = ov;

    lv_obj_t *card = ui_card_create(ov, 8, 80, TE_CARD_W, TE_CARD_H, ui_c_accent());
    lv_obj_t *title_lbl = ui_label_create(card, title, ui_font_title, ui_c_text());
    lv_obj_set_pos(title_lbl, 12, 8);

    lv_obj_t *tip = ui_label_create(card, "长按↑↓ 大步调整", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(tip, 12, 34);

    int inner = TE_CARD_W - 24;
    int slot = inner / count;

    for (int i = 0; i < count; i++) {
        int x = 12 + i * slot;

        lv_obj_t *name = ui_label_create(card, fields[i].name, ui_font_hint, ui_c_dim());
        lv_obj_set_width(name, slot - 4);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name, x, TE_FIELD_Y - 18);
        s.name_lbl[i] = name;

        lv_obj_t *box = lv_obj_create(card);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(box, x, TE_FIELD_Y);
        lv_obj_set_size(box, slot - 4, TE_VALUE_H + 8);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(ui_c_border()), 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        s.box[i] = box;

        lv_obj_t *value = ui_label_create(box, "", ui_font_body, ui_c_text());
        lv_obj_set_width(value, slot - 4);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(value, LV_ALIGN_CENTER, 0, 0);
        s.value_lbl[i] = value;
    }

    te_render();
    ui_page_set_hint("短按 ±1  长按 ±10  OK 下一项  长按OK 保存");
}

void ui_timeedit_close(void)
{
    if (s.overlay) {
        lv_obj_delete(s.overlay);
        s.overlay = NULL;
    }
    s.done = NULL;
    s.user = NULL;
    s.count = 0;
}

bool ui_timeedit_active(void) { return s.overlay != NULL; }

static void te_finish(bool saved)
{
    void (*done)(bool, void *) = s.done;
    void *user = s.user;
    ui_timeedit_close();
    if (done) done(saved, user);
}

bool ui_timeedit_handle(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.overlay) return false;

    if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) {
            te_finish(true);
            return true;
        }
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            int step = s.fields[s.cursor].step;
            if (step < 1) step = 1;
            s.values[s.cursor] += (btn == BSP_BTN_UP) ? -step : step;
            te_clamp(s.cursor);
            te_render();
            return true;
        }
        return true;
    }

    if (ev != BSP_BTN_CLICK) return true;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        s.values[s.cursor] += (btn == BSP_BTN_UP) ? -1 : 1;
        te_clamp(s.cursor);
        te_render();
        return true;
    }

    if (btn == BSP_BTN_OK) {
        if (s.cursor + 1 >= s.count) {
            te_finish(true);
        } else {
            s.cursor++;
            te_render();
        }
        return true;
    }

    return true;
}
