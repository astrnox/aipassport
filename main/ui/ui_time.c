// main/ui/ui_time.c —— 1 时间与日历：万年历 / 时间进度 / 秒表 / 计时器。
//
// 四个视图共用一个页面：长按 DOWN 循环切换视图，每个视图内 UP/DOWN 与 OK 的含义随
// 视图变化，底部提示条始终写出当前可用按键，用户不需要记住跨页规则。
//
// 计时用 esp_timer_get_time() 的单调时钟，与墙钟无关：熄屏、校时或用户改时间都不会
// 让秒表与计时器跳变。日历与时间进度全部在本地计算，不依赖网络。
#include "ui_pages.h"

#include "ui_app.h"
#include "ui_sound.h"
#include "ui_theme.h"
#include "ui_timeedit.h"

#include "app_state.h"
#include "logic/app_text.h"
#include "logic/app_time.h"

#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define CAL_CELL_W      ((UI_W - 16) / 7)
#define CAL_CELL_H      22
#define CAL_ROWS        6
#define CAL_CELLS       (CAL_ROWS * 7)
#define PROG_H          176
#define STOP_LAP_MAX    4

enum { VIEW_CAL = 0, VIEW_PROGRESS, VIEW_STOPWATCH, VIEW_TIMER, VIEW_COUNT };

static const char *const VIEW_NAMES[VIEW_COUNT] = {
    "万年历", "时间进度", "秒表", "计时器"
};

static const char *const VIEW_HINTS[VIEW_COUNT] = {
    "↑↓ 翻月  长按↑ 农历  OK 进度  长按↓ 切视图  长按OK 返回",
    "↑↓ 精度  OK 尺度  长按↑ 设生日  长按↓ 切视图  长按OK 返回",
    "OK 开始  ↑ 记圈  长按↑ 清零  长按↓ 切视图  长按OK 返回",
    "OK 开始  ↑ 重置  长按↑ 设时长  长按↓ 切视图  长按OK 返回",
};

static struct {
    ui_page_t page;

    int view;
    lv_obj_t *views[VIEW_COUNT];
    lv_obj_t *hdr_title;
    lv_obj_t *hdr_right;

    // 万年历
    int cy, cm;
    bool cn_view;
    int rendered_day;      // year*10000 + month*100 + day，避免每秒重绘整月
    lv_obj_t *cal_cells[CAL_CELLS];
    lv_obj_t *cal_info1;
    lv_obj_t *cal_info2;

    // 时间进度
    lv_obj_t *prog_grid;
    lv_obj_t *prog_note;
    app_scale_t scale;
    app_precision_t prec;
    char prog_pct[8];

    // 秒表 [0] / 计时器 [1]
    lv_obj_t *st_time[2];
    lv_obj_t *st_note[2];
    lv_obj_t *st_laps;
    bool running;
    int64_t accum_us;
    int64_t start_us;
    int64_t stop_us;
    int timer_seconds;
    int64_t laps[STOP_LAP_MAX];
    int lap_count;
    bool beeped;
    lv_timer_t *fast;
} s;

// ---------------------------------------------------------------------------
// 视图骨架
// ---------------------------------------------------------------------------

static void make_view(int index)
{
    lv_obj_t *v = lv_obj_create(s.page.content);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(v, LV_PCT(100));
    lv_obj_set_height(v, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_radius(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_set_style_pad_row(v, 4, 0);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    s.views[index] = v;
}

static void show_view(int index)
{
    if (index < 0) index = VIEW_COUNT - 1;
    if (index >= VIEW_COUNT) index = 0;
    s.view = index;
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (!s.views[i]) continue;
        if (i == index) lv_obj_remove_flag(s.views[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s.views[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s.hdr_title, VIEW_NAMES[index]);
    ui_page_set_hint(VIEW_HINTS[index]);
    lv_obj_scroll_to_y(s.page.content, 0, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// 万年历
// ---------------------------------------------------------------------------

static void calendar_build(void)
{
    static const char *const NAMES[7] = { "日", "一", "二", "三", "四", "五", "六" };

    lv_obj_t *week = lv_obj_create(s.views[VIEW_CAL]);
    lv_obj_remove_flag(week, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(week, LV_PCT(100));
    lv_obj_set_height(week, 16);
    lv_obj_set_style_bg_opa(week, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(week, 0, 0);
    lv_obj_set_style_pad_all(week, 0, 0);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *lbl = ui_label_create(week, NAMES[i], ui_font_hint, ui_c_dim());
        lv_obj_set_pos(lbl, i * CAL_CELL_W + 4, 0);
        lv_obj_set_width(lbl, CAL_CELL_W);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *grid = lv_obj_create(s.views[VIEW_CAL]);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, CAL_ROWS * CAL_CELL_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    for (int i = 0; i < CAL_CELLS; i++) {
        lv_obj_t *cell = ui_label_create(grid, "", ui_font_hint, ui_c_text());
        lv_obj_set_pos(cell, (i % 7) * CAL_CELL_W + 4, (i / 7) * CAL_CELL_H + 3);
        lv_obj_set_width(cell, CAL_CELL_W);
        lv_obj_set_height(cell, CAL_CELL_H - 6);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        s.cal_cells[i] = cell;
    }

    s.cal_info1 = ui_label_create(s.views[VIEW_CAL], "", ui_font_hint, ui_c_text());
    lv_obj_set_width(s.cal_info1, LV_PCT(100));
    s.cal_info2 = ui_label_create(s.views[VIEW_CAL], "", ui_font_hint, ui_c_dim());
    lv_obj_set_width(s.cal_info2, LV_PCT(100));
}

static void calendar_render(void)
{
    app_datetime_t now = app_state_now();
    lv_label_set_text_fmt(s.hdr_right, "%d 年 %d 月", s.cy, s.cm);

    int first_wd = app_time_weekday(s.cy, s.cm, 1);
    int dim = app_time_days_in_month(s.cy, s.cm);
    bool current_month = (now.year == s.cy && now.month == s.cm);

    for (int i = 0; i < CAL_CELLS; i++) {
        lv_obj_t *cell = s.cal_cells[i];
        if (!cell) continue;
        int day = i - first_wd + 1;
        if (day < 1 || day > dim) {
            lv_label_set_text(cell, "");
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            continue;
        }
        lv_label_set_text_fmt(cell, "%d", day);

        bool today = current_month && day == now.day;
        int wd = app_time_weekday(s.cy, s.cm, day);
        lv_obj_set_style_text_color(cell,
            lv_color_hex(today ? ui_c_bg()
                               : ((wd == 0 || wd == 6) ? ui_c_soon() : ui_c_text())), 0);
        if (today) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(ui_c_accent()), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, 3, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        }
    }

    app_lunar_t lunar;
    if (app_lunar_from_solar(now.year, now.month, now.day, &lunar)) {
        char line1[64];
        snprintf(line1, sizeof(line1), "%s%s年 %s%s%s · %s %s",
                 lunar.ganzhi, lunar.zodiac, lunar.leap ? "闰" : "",
                 lunar.month_name, lunar.day_name, lunar.zodiac, "");
        // 农历年只在信息行出现一次，这里去掉重复的生肖，保持一行可读。
        snprintf(line1, sizeof(line1), "%s年 %s%s%s",
                 lunar.ganzhi, lunar.leap ? "闰" : "",
                 lunar.month_name, lunar.day_name);
        lv_label_set_text(s.cal_info1, line1);
    } else {
        lv_label_set_text(s.cal_info1, "-");
    }

    if (s.cn_view) {
        const char *term = app_solar_term_name(now.year, now.month, now.day);
        const char *duty = app_lunar_duty_name(now.year, now.month, now.day);
        char yi[48] = { 0 };
        char ji[48] = { 0 };
        app_lunar_yi_ji(now.year, now.month, now.day, yi, sizeof(yi), ji, sizeof(ji));
        char line2[128];
        snprintf(line2, sizeof(line2), "%s%s%s  宜 %s  忌 %s",
                 term ? term : "", term ? " · " : "", duty ? duty : "", yi, ji);
        lv_label_set_text(s.cal_info2, line2);
    } else {
        lv_label_set_text(s.cal_info2, "长按↑ 叠加节气与宜忌");
    }

    s.rendered_day = now.year * 10000 + now.month * 100 + now.day;
}

static void calendar_shift_month(int delta)
{
    int y = s.cy;
    int m = s.cm;
    if (!app_time_add_months(y, m, delta, &y, &m)) return;
    if (y < 1970 || y > 2099) return;
    s.cy = y;
    s.cm = m;
    calendar_render();
}

// ---------------------------------------------------------------------------
// 时间进度：一次绘制回调画完整网格，避免几百个 LVGL 对象拖垮无 PSRAM 的设备。
// ---------------------------------------------------------------------------

typedef struct {
    int total;
    int filled;
    lv_color_t on;
    lv_color_t off;
    lv_color_t now;
} grid_ctx_t;

static grid_ctx_t s_grid;

static void grid_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer || s_grid.total <= 0) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int w = lv_area_get_width(&coords);
    int h = lv_area_get_height(&coords);
    if (w <= 0 || h <= 0) return;

    int cols = 1;
    while ((int64_t)(cols + 1) * (cols + 1) * h <= (int64_t)s_grid.total * w) cols++;
    int rows = (s_grid.total + cols - 1) / cols;

    const int gap = 1;
    int cw = (w - (cols - 1) * gap) / cols;
    int ch = (h - (rows - 1) * gap) / rows;
    if (cw < 2) cw = 2;
    if (ch < 2) ch = 2;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 1;
    dsc.bg_opa = LV_OPA_COVER;

    for (int i = 0; i < s_grid.total; i++) {
        int r = i / cols;
        int c = i % cols;
        lv_area_t cell;
        cell.x1 = coords.x1 + c * (cw + gap);
        cell.y1 = coords.y1 + r * (ch + gap);
        cell.x2 = cell.x1 + cw - 1;
        cell.y2 = cell.y1 + ch - 1;
        if (cell.x2 > coords.x2) continue;
        if (cell.y2 > coords.y2) break;

        dsc.bg_color = (i < s_grid.filled)
                           ? ((i == s_grid.filled - 1) ? s_grid.now : s_grid.on)
                           : s_grid.off;
        lv_draw_rect(layer, &dsc, &cell);
    }
}

static void progress_build(void)
{
    lv_obj_t *grid = lv_obj_create(s.views[VIEW_PROGRESS]);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, PROG_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_add_event_cb(grid, grid_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s.prog_grid = grid;

    s.prog_note = ui_label_create(s.views[VIEW_PROGRESS], "", ui_font_hint, ui_c_dim());
    lv_obj_set_width(s.prog_note, LV_PCT(100));
}

static void progress_render(void)
{
    if (!s.prog_grid) return;

    app_settings_t *st = app_state_settings();
    app_datetime_t now = app_state_now();

    if (s.scale == APP_SCALE_LIFE && st->birth_year <= 0) {
        s_grid.total = 0;
        s_grid.filled = 0;
        lv_obj_invalidate(s.prog_grid);
        lv_label_set_text(s.prog_note,
                          "未设置生日，人生尺度暂不可用。长按↑ 填写出生日期与预期寿命。");
        lv_label_set_text(s.hdr_right, app_scale_name(s.scale));
        return;
    }

    int total = 0, filled = 0;
    app_time_progress(s.scale, s.prec, &now,
                      st->birth_year, st->birth_month, st->birth_day,
                      st->life_expectancy, &total, &filled);
    if (total <= 0) total = 1;
    if (filled < 0) filled = 0;
    if (filled > total) filled = total;

    s_grid.total = total;
    s_grid.filled = filled;
    s_grid.on = lv_color_hex(ui_c_accent());
    s_grid.off = lv_color_hex(ui_c_border());
    s_grid.now = lv_color_hex(ui_c_live());
    lv_obj_invalidate(s.prog_grid);

    snprintf(s.prog_pct, sizeof(s.prog_pct), "%d%%",
             (int)((int64_t)filled * 100 / total));

    char note[112];
    if (s.scale == APP_SCALE_LIFE) {
        int age = now.year - st->birth_year;
        if (now.month < st->birth_month ||
            (now.month == st->birth_month && now.day < st->birth_day)) age--;
        if (age < 0) age = 0;
        snprintf(note, sizeof(note), "%d / %d 格 · 已过 %s · 约 %d 岁，预期 %d 岁",
                 filled, total, s.prog_pct, age, st->life_expectancy);
    } else {
        snprintf(note, sizeof(note), "%d / %d 格 · 已过 %s", filled, total, s.prog_pct);
    }
    lv_label_set_text(s.prog_note, note);
    lv_label_set_text_fmt(s.hdr_right, "%s · %s",
                          app_scale_name(s.scale), app_precision_name(s.prec));
}

static void birth_saved(bool saved, void *user)
{
    (void)user;
    show_view(VIEW_PROGRESS);
    if (saved) {
        app_state_save_settings();
        ui_hint_flash("已保存", 1200);
    }
    progress_render();
}

static void progress_edit_birth(void)
{
    static const ui_timeedit_field_t FIELDS[4] = {
        { "年", 1940, 2025, 5 },
        { "月", 1, 12, 1 },
        { "日", 1, 31, 1 },
        { "寿命", 60, 120, 5 },
    };
    static int values[4];
    app_settings_t *st = app_state_settings();
    app_datetime_t now = app_state_now();

    values[0] = st->birth_year > 0 ? st->birth_year : (now.year - 18);
    values[1] = st->birth_month > 0 ? st->birth_month : 1;
    values[2] = st->birth_day > 0 ? st->birth_day : 1;
    values[3] = st->life_expectancy >= 60 ? st->life_expectancy : 80;

    ui_timeedit_open(s.page.scr, "出生日期与预期寿命", FIELDS, values, 4,
                     birth_saved, NULL);
}

// ---------------------------------------------------------------------------
// 秒表 / 计时器
// ---------------------------------------------------------------------------

static int64_t mono_us(void) { return esp_timer_get_time(); }

static int64_t stop_elapsed_us(void)
{
    if (!s.running) return s.stop_us;
    return s.accum_us + (mono_us() - s.start_us);
}

static void stop_fmt(char *out, size_t cap, int64_t us)
{
    if (us < 0) us = 0;
    int cs = (int)((us / 10000) % 100);
    int sec = (int)((us / 1000000) % 60);
    int min = (int)((us / 60000000) % 60);
    int hour = (int)(us / 3600000000LL);
    if (hour > 0) snprintf(out, cap, "%d:%02d:%02d", hour, min, sec);
    else snprintf(out, cap, "%02d:%02d.%02d", min, sec, cs);
}

static void stop_render_laps(void)
{
    if (!s.st_laps) return;
    if (s.lap_count <= 0) {
        lv_label_set_text(s.st_laps, "按 ↑ 记录分段");
        return;
    }
    char buf[160];
    int off = 0;
    for (int i = 0; i < s.lap_count; i++) {
        char t[24];
        stop_fmt(t, sizeof(t), s.laps[i]);
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off, "%d  %s\n", i + 1, t);
        if (n <= 0 || off + n >= (int)sizeof(buf)) break;
        off += n;
    }
    lv_label_set_text(s.st_laps, buf);
}

static void stop_render(void)
{
    if (s.view != VIEW_STOPWATCH && s.view != VIEW_TIMER) return;
    int idx = s.view - VIEW_STOPWATCH;
    if (!s.st_time[idx]) return;

    char t[24];
    if (idx == 0) {
        stop_fmt(t, sizeof(t), stop_elapsed_us());
        lv_label_set_text(s.st_time[idx], t);
        lv_obj_set_style_text_color(s.st_time[idx], lv_color_hex(ui_c_text()), 0);
        lv_label_set_text(s.st_note[idx],
                          s.running ? "计时中" : (s.stop_us > 0 ? "已暂停" : "就绪"));
        return;
    }

    int64_t remain = (int64_t)s.timer_seconds * 1000000 - stop_elapsed_us();
    if (remain <= 0 && s.running) {
        s.running = false;
        s.accum_us = (int64_t)s.timer_seconds * 1000000;
        s.stop_us = s.accum_us;
        if (!s.beeped) {
            s.beeped = true;
            ui_sound_beep();
        }
        remain = 0;
    }
    if (remain < 0) remain = 0;

    stop_fmt(t, sizeof(t), remain);
    lv_label_set_text(s.st_time[idx], t);
    bool done = (remain == 0 && s.accum_us >= (int64_t)s.timer_seconds * 1000000);
    lv_obj_set_style_text_color(s.st_time[idx],
        lv_color_hex(done ? ui_c_live() : ui_c_text()), 0);
    if (done) {
        lv_label_set_text(s.st_note[idx], "时间到，按 OK 重新开始");
    } else {
        lv_label_set_text_fmt(s.st_note[idx], "设定 %d 分 %d 秒 · %s",
                              s.timer_seconds / 60, s.timer_seconds % 60,
                              s.running ? "计时中" : (s.stop_us > 0 ? "已暂停" : "就绪"));
    }
}

static void stop_reset(void)
{
    s.running = false;
    s.accum_us = 0;
    s.stop_us = 0;
    s.beeped = false;
    s.lap_count = 0;
    for (int i = 0; i < STOP_LAP_MAX; i++) s.laps[i] = 0;
    stop_render_laps();
    stop_render();
}

static void stop_toggle(void)
{
    if (s.running) {
        s.stop_us = stop_elapsed_us();
        s.accum_us = s.stop_us;
        s.running = false;
        stop_render();
        return;
    }

    if (s.view == VIEW_TIMER) {
        int64_t total = (int64_t)s.timer_seconds * 1000000;
        if (s.accum_us >= total) {      // 已到时，再按 OK 从头开始
            s.accum_us = 0;
            s.beeped = false;
        }
    }
    s.start_us = mono_us();
    s.running = true;
    stop_render();
}

static void stop_lap(void)
{
    if (s.view != VIEW_STOPWATCH || !s.running) return;
    int64_t now = stop_elapsed_us();
    if (s.lap_count < STOP_LAP_MAX) {
        s.laps[s.lap_count++] = now;
    } else {
        memmove(&s.laps[0], &s.laps[1], sizeof(s.laps[0]) * (STOP_LAP_MAX - 1));
        s.laps[STOP_LAP_MAX - 1] = now;
    }
    stop_render_laps();
}

static void timer_edit_done(bool saved, void *user)
{
    (void)user;
    show_view(VIEW_TIMER);
    if (saved) {
        if (s.timer_seconds < 5) s.timer_seconds = 5;
        stop_reset();
        ui_hint_flash("时长已更新", 1200);
    }
    stop_render();
}

static void timer_edit_open(void)
{
    static const ui_timeedit_field_t FIELDS[2] = {
        { "分", 0, 99, 5 },
        { "秒", 0, 59, 10 },
    };
    static int values[2];
    values[0] = s.timer_seconds / 60;
    values[1] = s.timer_seconds % 60;
    ui_timeedit_open(s.page.scr, "计时时长", FIELDS, values, 2, timer_edit_done, NULL);
}

static void fast_tick(lv_timer_t *timer)
{
    (void)timer;
    if (ui_app_is_asleep()) return;
    stop_render();
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------

void page_time_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ui_timeedit_active()) {
        ui_timeedit_handle(btn, ev);
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        show_view(s.view + 1);
        return;
    }

    switch (s.view) {
    case VIEW_CAL:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            s.cn_view = !s.cn_view;
            calendar_render();
            ui_hint_flash(s.cn_view ? "已叠加农历与宜忌" : "已切回公历视图", 1200);
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) calendar_shift_month(-1);
        else if (btn == BSP_BTN_DOWN) calendar_shift_month(1);
        else if (btn == BSP_BTN_OK) show_view(VIEW_PROGRESS);
        break;

    case VIEW_PROGRESS:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            progress_edit_birth();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s.prec = (app_precision_t)((s.prec + (btn == BSP_BTN_UP ? -1 : 1) +
                                        APP_PREC_COUNT) % APP_PREC_COUNT);
            progress_render();
        } else if (btn == BSP_BTN_OK) {
            s.scale = (app_scale_t)((s.scale + 1) % APP_SCALE_COUNT);
            progress_render();
        }
        break;

    case VIEW_STOPWATCH:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            stop_reset();
            ui_hint_flash("秒表已清零", 1200);
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_OK) stop_toggle();
        else if (btn == BSP_BTN_UP) stop_lap();
        break;

    default:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            timer_edit_open();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_OK) stop_toggle();
        else if (btn == BSP_BTN_UP) {
            stop_reset();
            ui_hint_flash("计时器已重置", 1200);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_time_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.page = ui_page_create(VIEW_HINTS[VIEW_CAL]);
    ui_header_create(s.page.content, "时间与日历", "", &s.hdr_title, &s.hdr_right);

    for (int i = 0; i < VIEW_COUNT; i++) make_view(i);

    calendar_build();
    progress_build();

    for (int i = VIEW_STOPWATCH; i <= VIEW_TIMER; i++) {
        int idx = i - VIEW_STOPWATCH;
        s.st_time[idx] = ui_label_create(s.views[i], "00:00.00", ui_font_display, ui_c_text());
        lv_obj_set_width(s.st_time[idx], LV_PCT(100));
        lv_obj_set_style_text_align(s.st_time[idx], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(s.st_time[idx], 28, 0);
        s.st_note[idx] = ui_label_create(s.views[i], "", ui_font_hint, ui_c_dim());
        lv_obj_set_width(s.st_note[idx], LV_PCT(100));
        lv_obj_set_style_text_align(s.st_note[idx], LV_TEXT_ALIGN_CENTER, 0);
    }
    s.st_laps = ui_label_create(s.views[VIEW_STOPWATCH], "", ui_font_hint, ui_c_dim());
    lv_obj_set_width(s.st_laps, LV_PCT(100));
    lv_obj_set_style_pad_left(s.st_laps, 16, 0);
    lv_obj_set_style_pad_top(s.st_laps, 8, 0);

    app_datetime_t now = app_state_now();
    s.cy = now.year;
    s.cm = now.month;
    s.scale = APP_SCALE_DAY;
    s.prec = APP_PREC_BALANCED;
    s.timer_seconds = 5 * 60;

    calendar_render();
    progress_render();
    stop_reset();
    show_view(VIEW_CAL);

    s.fast = lv_timer_create(fast_tick, 100, NULL);
}

void page_time_exit(void)
{
    if (s.fast) {
        lv_timer_delete(s.fast);
        s.fast = NULL;
    }
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_time_tick(void)
{
    if (!s.page.scr) return;

    app_datetime_t now = app_state_now();
    int stamp = now.year * 10000 + now.month * 100 + now.day;

    if (s.view == VIEW_CAL) {
        // 只在日期真正变化时重绘整月，避免每秒刷新 42 个标签。
        if (s.rendered_day != stamp) calendar_render();
        return;
    }
    if (s.view == VIEW_PROGRESS) progress_render();
}
