// main/ui/ui_theme.c —— 应用统一视觉规范与通用控件实现。
//
// 说明几个刻意的取舍：
//  - 中文字形来自 assets/fonts 生成的 app_font_12/16/20（完整 GB2312 覆盖）。这三个
//    字号覆盖标题、正文与提示条；展示级数字只用 ASCII，直接用 LVGL 自带 Montserrat
//    的 32/40，省下为几个数字多带一套中文字形的 Flash。
//  - 生成字体的描述符是只读常量。为了能画 LVGL 的符号字形（如 LV_SYMBOL_OK，位于
//    私用区，生成字体不含），这里复制一份可写描述符并挂上同号 Montserrat 作为回退。
//    回退是单向的：先查生成字体，缺字才落到 Montserrat。
//  - 单 DMA 缓冲、无 PSRAM，卡片只用 1px 描边与左侧 2px 语义色条区分层级，不用阴影
//    与渐变，避免大面积重绘掉帧。
#include "ui_theme.h"

#include "app_state.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------

static lv_font_t s_font_title;
static lv_font_t s_font_body;
static lv_font_t s_font_hint;

const lv_font_t *const ui_font_display   = &lv_font_montserrat_40;
const lv_font_t *const ui_font_display_s = &lv_font_montserrat_32;
const lv_font_t *const ui_font_title     = &s_font_title;
const lv_font_t *const ui_font_body      = &s_font_body;
const lv_font_t *const ui_font_hint      = &s_font_hint;

void ui_fonts_init(void)
{
    s_font_title = app_font_20;
    s_font_title.fallback = &lv_font_montserrat_20;
    s_font_body = app_font_16;
    s_font_body.fallback = &lv_font_montserrat_14;
    s_font_hint = app_font_12;
    s_font_hint.fallback = &lv_font_montserrat_14;
}

// ---------------------------------------------------------------------------
// 主题与颜色
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t bg, card, text, dim, accent, live, soon, done, ok, warn, sel, border;
} palette_t;

static const palette_t PALETTE_DARK = {
    .bg = 0x0B0E13, .card = 0x151A22, .text = 0xE8EDF4, .dim = 0x93A0B4,
    .accent = 0x22D3EE, .live = 0xFF4757, .soon = 0xFFB020, .done = 0x6B7280,
    .ok = 0x34D399, .warn = 0xF59E0B, .sel = 0x1E2632, .border = 0x262E3A,
};

static const palette_t PALETTE_LIGHT = {
    .bg = 0xF5F7FA, .card = 0xFFFFFF, .text = 0x10141A, .dim = 0x5B6577,
    .accent = 0x0891B2, .live = 0xE11D48, .soon = 0xB45309, .done = 0x9AA3B2,
    .ok = 0x059669, .warn = 0xB45309, .sel = 0xE6EEF6, .border = 0xD8DFE8,
};

static ui_theme_mode_t s_mode = UI_THEME_DARK;

static const palette_t *palette(void)
{
    return s_mode == UI_THEME_DARK ? &PALETTE_DARK : &PALETTE_LIGHT;
}

void ui_theme_set(ui_theme_mode_t mode)
{
    s_mode = (mode == UI_THEME_LIGHT) ? UI_THEME_LIGHT : UI_THEME_DARK;
}

ui_theme_mode_t ui_theme_mode(void)
{
    return s_mode;
}

void ui_theme_apply_auto(bool auto_mode, int hour)
{
    if (!auto_mode) return;
    ui_theme_set((hour >= 19 || hour < 7) ? UI_THEME_DARK : UI_THEME_LIGHT);
}

uint32_t ui_c_bg(void)     { return palette()->bg; }
uint32_t ui_c_card(void)   { return palette()->card; }
uint32_t ui_c_text(void)   { return palette()->text; }
uint32_t ui_c_dim(void)    { return palette()->dim; }
uint32_t ui_c_accent(void) { return palette()->accent; }
uint32_t ui_c_live(void)   { return palette()->live; }
uint32_t ui_c_soon(void)   { return palette()->soon; }
uint32_t ui_c_done(void)   { return palette()->done; }
uint32_t ui_c_ok(void)     { return palette()->ok; }
uint32_t ui_c_warn(void)   { return palette()->warn; }
uint32_t ui_c_sel(void)    { return palette()->sel; }
uint32_t ui_c_border(void) { return palette()->border; }

// ---------------------------------------------------------------------------
// 页面骨架
// ---------------------------------------------------------------------------

typedef struct {
    lv_obj_t *time_lbl;
    lv_obj_t *badge_lbl;   // 赛事角标，空则不显示
    lv_obj_t *net_lbl;     // 网络状态
    lv_obj_t *batt_lbl;    // 电量
} status_widgets_t;

static status_widgets_t s_status;
static lv_obj_t *s_hint_label;
static char s_hint_base[64];
static lv_timer_t *s_hint_timer;
static char s_net_text[24];
static bool s_net_online;
static char s_event_badge[24];     // 赛事角标文字，空则不显示
static uint32_t s_event_color;
static bool s_status_ready;

static void hint_restore_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_hint_label) lv_label_set_text(s_hint_label, s_hint_base);
    s_hint_timer = NULL;
}

void ui_hint_flash(const char *text, uint32_t ms)
{
    if (!s_hint_label) return;
    lv_label_set_text(s_hint_label, text);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(ui_c_accent()), 0);
    if (s_hint_timer) lv_timer_delete(s_hint_timer);
    s_hint_timer = lv_timer_create(hint_restore_cb, ms, NULL);
    lv_timer_set_repeat_count(s_hint_timer, 1);
}

void ui_page_set_hint(const char *text)
{
    if (text) {
        strncpy(s_hint_base, text, sizeof(s_hint_base) - 1);
        s_hint_base[sizeof(s_hint_base) - 1] = '\0';
    } else {
        s_hint_base[0] = '\0';
    }
    if (s_hint_label) {
        lv_label_set_text(s_hint_label, s_hint_base);
        lv_obj_set_style_text_color(s_hint_label, lv_color_hex(ui_c_dim()), 0);
    }
}

ui_page_t ui_page_create(const char *hint_text)
{
    ui_page_t page = {0};
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(ui_c_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    page.scr = scr;

    // 状态栏：左时间、右电量/网络/赛事角标。
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_W, UI_STATUS_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(ui_c_border()), 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    s_status.time_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_status.time_lbl, ui_font_body, 0);
    lv_obj_set_style_text_color(s_status.time_lbl, lv_color_hex(ui_c_text()), 0);
    lv_obj_align(s_status.time_lbl, LV_ALIGN_LEFT_MID, UI_MARGIN_X, 0);

    // 右侧按"赛事角标 · 网络 · 电量"顺序排列，缺失项自动不占位。
    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(right, LV_SIZE_CONTENT, UI_STATUS_H);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -UI_MARGIN_X, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_status.badge_lbl = lv_label_create(right);
    lv_obj_set_style_text_font(s_status.badge_lbl, ui_font_hint, 0);
    s_status.net_lbl = lv_label_create(right);
    lv_obj_set_style_text_font(s_status.net_lbl, ui_font_hint, 0);
    s_status.batt_lbl = lv_label_create(right);
    lv_obj_set_style_text_font(s_status.batt_lbl, ui_font_hint, 0);
    s_status_ready = true;

    // 内容区：可滚动。
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, UI_STATUS_H);
    lv_obj_set_size(content, UI_W, UI_CONTENT_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_left(content, UI_MARGIN_X, 0);
    lv_obj_set_style_pad_right(content, UI_MARGIN_X, 0);
    lv_obj_set_style_pad_top(content, UI_CARD_GAP, 0);
    lv_obj_set_style_pad_bottom(content, UI_CARD_GAP, 0);
    lv_obj_set_style_pad_row(content, UI_CARD_GAP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    // 只允许纵向滚动，并关掉弹性回弹与惯性：设备靠焦点移动来滚动，弹性回弹会把内容
    // 拖出边界、露出大片空白，用户看到的就是"能滑到没有文字的地方"。关掉后滚动只
    // 在内容真实范围内发生。
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    page.content = content;

    // 提示条。
    lv_obj_t *hint = lv_obj_create(scr);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(hint, 0, UI_H - UI_HINT_H);
    lv_obj_set_size(hint, UI_W, UI_HINT_H);
    lv_obj_set_style_bg_color(hint, lv_color_hex(ui_c_card()), 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hint, 0, 0);
    lv_obj_set_style_radius(hint, 0, 0);
    lv_obj_set_style_pad_all(hint, 0, 0);
    page.hint = hint;

    // 提示条文字：允许折行。按键约定常常超过 240px 一行能容纳的宽度，不给宽度就会
    // 溢出到条外被裁掉；这里钉死宽度并居中，让它最多占两行、始终落在条内。
    s_hint_label = lv_label_create(hint);
    lv_obj_set_style_text_font(s_hint_label, ui_font_hint, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(ui_c_dim()), 0);
    lv_obj_set_width(s_hint_label, UI_W - 8);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_hint_label);

    if (s_hint_timer) {
        lv_timer_delete(s_hint_timer);
        s_hint_timer = NULL;
    }
    ui_page_set_hint(hint_text);
    ui_status_bar_refresh();
    // 新页面自己成为活动屏幕。控制器总是先 exit 旧页（删除旧屏，LVGL 会把 act_scr 置空）
    // 再 enter 新页，所以这里加载不会与旧屏的删除顺序冲突。
    lv_screen_load(scr);
    return page;
}

void ui_status_bar_set_net(const char *text, bool online)
{
    if (text) {
        strncpy(s_net_text, text, sizeof(s_net_text) - 1);
        s_net_text[sizeof(s_net_text) - 1] = '\0';
    } else {
        s_net_text[0] = '\0';
    }
    s_net_online = online;
    ui_status_bar_refresh();
}

void ui_status_bar_set_badge(const char *text, bool live)
{
    if (text) {
        strncpy(s_event_badge, text, sizeof(s_event_badge) - 1);
        s_event_badge[sizeof(s_event_badge) - 1] = '\0';
    } else {
        s_event_badge[0] = '\0';
    }
    s_event_color = live ? ui_c_live() : ui_c_soon();
    ui_status_bar_refresh();
}

// 右侧三个标签按可用性显示：空文本的标签隐藏且不占位。
static void status_label_set(lv_obj_t *label, const char *text, uint32_t color)
{
    if (!label) return;
    if (text && text[0]) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(label, "");
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_status_bar_refresh(void)
{
    if (!s_status.time_lbl) return;

    app_datetime_t now = app_state_now();
    lv_label_set_text_fmt(s_status.time_lbl, "%02d:%02d", now.hour, now.minute);

    // 赛事角标：进行中/即将开赛时出现，用圆点加文字区分。
    if (s_event_badge[0]) {
        char buf[28];
        snprintf(buf, sizeof(buf), "● %s", s_event_badge);
        status_label_set(s_status.badge_lbl, buf, s_event_color);
    } else {
        status_label_set(s_status.badge_lbl, NULL, 0);
    }

    // 网络状态：仅在线或配网中显示，离线时不占位。
    status_label_set(s_status.net_lbl, s_net_text,
                     s_net_online ? ui_c_ok() : ui_c_soon());

    int soc = app_state_battery_soc();
    char buf[16];
    if (soc < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", soc);
    }
    status_label_set(s_status.batt_lbl, buf,
                     (soc >= 0 && soc < 20) ? ui_c_live() : ui_c_dim());
}

// ---------------------------------------------------------------------------
// 通用控件
// ---------------------------------------------------------------------------

lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text ? text : "");
    return label;
}

lv_obj_t *ui_card_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(ui_c_card()), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(ui_c_border()), 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    if (accent != 0) {
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        // LVGL 9 的 flex/grid 布局会覆盖 lv_obj_set_pos 设置的坐标；accent bar
        // 是纯装饰条，必须钉在 (0,0)，所以用 FLOATING 把它排除在布局之外。
        lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_size(bar, 3, h);
        lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
    }
    return card;
}

ui_digit_row_t ui_digit_row_create(lv_obj_t *parent, int digits,
                                   const lv_font_t *font, uint32_t color, int digit_w)
{
    ui_digit_row_t row = {0};
    if (digits < 1) return row;
    if (digits > (int)(sizeof(row.labels) / sizeof(row.labels[0]))) {
        digits = (int)(sizeof(row.labels) / sizeof(row.labels[0]));
    }
    row.count = digits;
    for (int i = 0; i < digits; i++) {
        lv_obj_t *label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_width(label, digit_w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(label, "-");
        row.labels[i] = label;
    }
    return row;
}

void ui_digit_row_set(ui_digit_row_t *row, const char *text, uint32_t color)
{
    if (!row) return;
    const char *cursor = text ? text : "";
    for (int i = 0; i < row->count; i++) {
        char ch[2] = { (cursor && *cursor) ? *cursor : ' ', 0 };
        if (cursor && *cursor) cursor++;
        lv_label_set_text(row->labels[i], ch);
        lv_obj_set_style_text_color(row->labels[i], lv_color_hex(color), 0);
    }
}

lv_obj_t *ui_list_create(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    return list;
}

ui_row_t ui_row_create(lv_obj_t *parent, const char *title, const char *value)
{
    ui_row_t row = {0};
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, UI_ROW_H);
    lv_obj_set_style_bg_color(obj, lv_color_hex(ui_c_card()), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 5, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    row.obj = obj;

    row.indicator = lv_obj_create(obj);
    lv_obj_remove_flag(row.indicator, LV_OBJ_FLAG_SCROLLABLE);
    // 选中指示条是装饰元素，钉在行左侧；若行将来被设为 flex 容器，
    // 需要 FLOATING 才能不被布局重新排列。
    lv_obj_add_flag(row.indicator, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(row.indicator, 0, 4);
    lv_obj_set_size(row.indicator, 3, UI_ROW_H - 8);
    lv_obj_set_style_bg_color(row.indicator, lv_color_hex(ui_c_accent()), 0);
    lv_obj_set_style_bg_opa(row.indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row.indicator, 0, 0);
    lv_obj_set_style_radius(row.indicator, 0, 0);
    lv_obj_add_flag(row.indicator, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title_lbl = ui_label_create(obj, title, ui_font_body, ui_c_text());
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    row.value = ui_label_create(obj, value ? value : "", ui_font_hint, ui_c_dim());
    lv_obj_align(row.value, LV_ALIGN_RIGHT_MID, -10, 0);
    return row;
}

void ui_row_set_value(ui_row_t row, const char *value)
{
    if (row.value) lv_label_set_text(row.value, value ? value : "");
}

void ui_row_set_selected(ui_row_t row, bool selected)
{
    if (!row.obj) return;
    if (row.indicator) {
        if (selected) lv_obj_remove_flag(row.indicator, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(row.indicator, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(row.obj,
        lv_color_hex(selected ? ui_c_sel() : ui_c_card()), 0);
}

void ui_row_set_title_color(ui_row_t row, uint32_t color)
{
    if (!row.obj) return;
    lv_obj_t *title = lv_obj_get_child(row.obj, 1);
    if (title) lv_obj_set_style_text_color(title, lv_color_hex(color), 0);
}

lv_obj_t *ui_tabs_create(lv_obj_t *parent, const char *const *names, int count)
{
    lv_obj_t *tabs = lv_obj_create(parent);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(tabs, LV_PCT(100));
    lv_obj_set_height(tabs, 30);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_hex(ui_c_border()), 0);
    lv_obj_set_style_radius(tabs, 0, 0);
    lv_obj_set_style_pad_all(tabs, 0, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < count; i++) {
        lv_obj_t *label = ui_label_create(tabs, names[i], ui_font_hint, ui_c_dim());
        lv_obj_set_style_pad_hor(label, 8, 0);
    }
    ui_tabs_select(tabs, 0);
    return tabs;
}

void ui_tabs_select(lv_obj_t *tabs, int index)
{
    if (!tabs) return;
    uint32_t child_count = lv_obj_get_child_count(tabs);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *label = lv_obj_get_child(tabs, i);
        bool active = ((int)i == index);
        lv_obj_set_style_text_color(label,
            lv_color_hex(active ? ui_c_accent() : ui_c_dim()), 0);
    }
}

lv_obj_t *ui_ring_create(lv_obj_t *parent, int size, int x, int y, uint32_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, x, y);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(ui_c_border()), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    return arc;
}

void ui_ring_set(lv_obj_t *ring, int permille, uint32_t color)
{
    if (!ring) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    lv_arc_set_value(ring, permille);
    lv_obj_set_style_arc_color(ring, lv_color_hex(color), LV_PART_INDICATOR);
}

lv_obj_t *ui_empty_create(lv_obj_t *parent, const char *title, const char *body)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_lbl = ui_label_create(box, title, ui_font_title, ui_c_text());
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *body_lbl = ui_label_create(box, body, ui_font_body, ui_c_dim());
    lv_obj_set_width(body_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    return box;
}

lv_obj_t *ui_banner_create(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *banner = lv_obj_create(parent);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(banner, LV_PCT(100));
    lv_obj_set_height(banner, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(banner, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner, 5, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 8, 0);
    lv_obj_t *label = ui_label_create(banner, text, ui_font_hint, ui_c_bg());
    lv_obj_set_width(label, LV_PCT(100));
    return banner;
}

lv_obj_t *ui_header_create(lv_obj_t *parent, const char *title, const char *right,
                           lv_obj_t **title_out, lv_obj_t **right_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *title_lbl = ui_label_create(row, title, ui_font_title, ui_c_text());
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *right_lbl = ui_label_create(row, right ? right : "", ui_font_hint, ui_c_dim());
    lv_obj_align(right_lbl, LV_ALIGN_RIGHT_MID, -2, 0);

    if (title_out) *title_out = title_lbl;
    if (right_out) *right_out = right_lbl;
    return row;
}

lv_obj_t *ui_progress_create(lv_obj_t *parent, int w, int h, uint32_t color)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ui_c_border()), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    return bar;
}

void ui_progress_set(lv_obj_t *bar, int permille)
{
    if (!bar) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    lv_bar_set_value(bar, permille, LV_ANIM_OFF);
}

void ui_scroll_into_view(lv_obj_t *obj)
{
    if (!obj) return;
    // 先强制结算一次布局：刚创建的控件其父容器还没排好子控件坐标，此时
    // lv_obj_scroll_to_view 会按陈旧坐标（常常是 0）计算滚动量，表现为"光标往下走了
    // 屏幕却不跟着滑"。结算后再滚，坐标才是真实的。
    lv_obj_update_layout(obj);
    lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// 二次确认对话框
// ---------------------------------------------------------------------------

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *options[2];
    ui_dialog_cb_t cb;
    void *user;
    int focus;
} dialog_t;

static dialog_t s_dialog;

bool ui_dialog_is_open(void)
{
    return s_dialog.overlay != NULL;
}

void ui_dialog_close(void)
{
    if (s_dialog.overlay) {
        lv_obj_delete(s_dialog.overlay);
        s_dialog.overlay = NULL;
    }
    s_dialog.cb = NULL;
    s_dialog.user = NULL;
}

static void dialog_confirm(bool confirmed)
{
    ui_dialog_cb_t cb = s_dialog.cb;
    void *user = s_dialog.user;
    ui_dialog_close();
    if (cb) cb(confirmed, user);
}

void ui_dialog_open(lv_obj_t *parent, const char *title, const char *body,
                    const char *confirm_text, ui_dialog_cb_t cb, void *user)
{
    ui_dialog_close();
    if (!parent) return;

    lv_obj_t *overlay = lv_obj_create(parent);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, UI_W, UI_H);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    // 卡片高度放宽到 148 并把两个选项上移一行：多出的底部一行专门留给按键提示，
    // 提示与选项分处上下两条带，不会互相压字。
    lv_obj_t *card = ui_card_create(overlay, 20, 86, UI_W - 40, 148, ui_c_warn());
    lv_obj_set_style_bg_color(card, lv_color_hex(ui_c_card()), 0);

    lv_obj_t *title_lbl = ui_label_create(card, title, ui_font_title, ui_c_text());
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *body_lbl = ui_label_create(card, body, ui_font_hint, ui_c_dim());
    lv_obj_set_width(body_lbl, UI_W - 64);
    lv_obj_set_style_text_align(body_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(body_lbl, LV_ALIGN_TOP_LEFT, 12, 40);

    // 对话框自带按键提示：默认焦点落在"取消"（安全项），用户必须知道按 ↑↓ 才能切到
    // 确认项。没有这行提示时，很多人直接按 OK 触发的是"取消"，会以为删除失效。
    lv_obj_t *key_hint = ui_label_create(card, "↑↓ 选择   OK 确认   长按OK 取消",
                                         ui_font_hint, ui_c_dim());
    lv_obj_align(key_hint, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    // 默认焦点放在安全选项（取消）上。
    s_dialog.options[0] = ui_label_create(card, "取消", ui_font_body, ui_c_text());
    lv_obj_align(s_dialog.options[0], LV_ALIGN_BOTTOM_RIGHT, -16, -40);
    s_dialog.options[1] = ui_label_create(card,
        confirm_text ? confirm_text : "确认", ui_font_body, ui_c_warn());
    lv_obj_align(s_dialog.options[1], LV_ALIGN_BOTTOM_RIGHT, -86, -40);

    s_dialog.overlay = overlay;
    s_dialog.cb = cb;
    s_dialog.user = user;
    s_dialog.focus = 0;
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_color(s_dialog.options[i],
            lv_color_hex(i == 0 ? ui_c_accent() : ui_c_warn()), 0);
    }
}

bool ui_dialog_handle(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_dialog.overlay) return false;

    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_dialog.focus = 1 - s_dialog.focus;
            for (int i = 0; i < 2; i++) {
                lv_obj_set_style_text_color(s_dialog.options[i],
                    lv_color_hex(i == s_dialog.focus
                                     ? (i == 0 ? ui_c_accent() : ui_c_warn())
                                     : ui_c_dim()),
                    0);
            }
        } else if (btn == BSP_BTN_OK) {
            dialog_confirm(s_dialog.focus == 1);
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        dialog_confirm(false);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 通知弹层
// ---------------------------------------------------------------------------

static lv_obj_t *s_alert;

bool ui_alert_is_open(void)
{
    return s_alert != NULL;
}

void ui_alert_close(void)
{
    if (s_alert) {
        lv_obj_delete(s_alert);
        s_alert = NULL;
    }
}

void ui_alert_open(lv_obj_t *parent, const char *title, const char *body)
{
    ui_alert_close();
    if (!parent) return;

    lv_obj_t *overlay = lv_obj_create(parent);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, UI_W, UI_H);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    lv_obj_t *card = ui_card_create(overlay, 16, 92, UI_W - 32, 136, ui_c_accent());
    lv_obj_set_style_bg_color(card, lv_color_hex(ui_c_card()), 0);

    lv_obj_t *title_lbl = ui_label_create(card, title ? title : "", ui_font_title,
                                          ui_c_accent());
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *body_lbl = ui_label_create(card, body ? body : "", ui_font_hint, ui_c_text());
    lv_obj_set_width(body_lbl, UI_W - 60);
    lv_obj_set_style_text_align(body_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(body_lbl, LV_ALIGN_TOP_LEFT, 12, 40);

    lv_obj_t *option = ui_label_create(card, "OK 知道了", ui_font_body, ui_c_accent());
    lv_obj_align(option, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_alert = overlay;
}

bool ui_alert_handle(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    if (!s_alert) return false;
    // 任意按键都只关掉弹层：把它当作"我知道了"，不做其它动作。
    if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
        ui_alert_close();
    }
    return true;
}
