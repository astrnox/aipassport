// main/ui/ui_focus.c —— 2 专注与效率：番茄钟 / 本地提醒。
//
// 一个页面上同时呈现两件事：上方是可一眼读到的番茄钟（大号等宽剩余时间 + 阶段进度），
// 下方是可操作的提醒列表。不做子视图切换——三键设备上的模式越少，用户越不用猜当前
// 按键含义。焦点在番茄钟卡与提醒行之间连续移动，底部提示条随焦点改变当前可用按键。
//
// 焦点与按键（提示条如实写出）：
//   焦点在番茄钟  短按 OK 开始/暂停/继续，长按 UP 改专注与休息时长
//   焦点在提醒行  短按 OK 开关，长按 UP 改钟点与重复，长按 DOWN 删除（二次确认）
//   焦点在新增行  短按 OK 或长按 UP 新增一条 08:00 每天
//   任意位置      长按 OK 返回主页
//
// 番茄钟的阶段推进由 ui_app.c 的全局秒级节拍负责，本页只负责显示，因此熄屏或停在
// 其它页面都不影响计时；提醒的增删改全部走 app_state_* 并立即持久化。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"
#include "ui_timeedit.h"

#include "app_state.h"
#include "logic/app_pomodoro.h"
#include "logic/app_reminder.h"
#include "logic/app_text.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define FOCUS_CW       (UI_W - 2 * UI_MARGIN_X)     // 224
#define POMO_CARD_H    94
#define REMINDER_PRESET_DAILY   0x7F                // bit0=周日 .. bit6=周六
#define REMINDER_PRESET_WORKDAY 0x3E                // 周一至周五

static const char *const REPEAT_NAMES[3] = { "每天", "工作日", "一次性" };

// 番茄钟预设：专注/短休息分钟对。最后一项"自定义"不套用数值，而是提示改用长按↑ 逐项设置。
static const int POMO_PRESETS[][2] = { { 25, 5 }, { 45, 10 }, { 50, 10 }, { 90, 20 } };
#define POMO_PRESET_COUNT ((int)(sizeof(POMO_PRESETS) / sizeof(POMO_PRESETS[0])))
static const char *const POMO_PRESET_NAMES[POMO_PRESET_COUNT + 1] = {
    "25/5", "45/10", "50/10", "90/20", "自定义",
};
static const char *const POMO_AUTO_NAMES[2] = { "关闭", "开启" };

static struct {
    ui_page_t page;

    lv_obj_t *pomo_card;
    lv_obj_t *pomo_time;
    lv_obj_t *pomo_state;
    lv_obj_t *pomo_bar;
    lv_obj_t *pomo_info;
    lv_obj_t *pomo_stats;

    lv_obj_t *section;          // "提醒 N/16"
    lv_obj_t *list;
    ui_row_t  rows[APP_REMINDER_MAX + 1];   // 末行为"新增提醒"
    int       row_count;                    // 已创建的提醒行数

    int       focus;            // 0 = 番茄钟卡，1..count = 提醒行，count+1 = 新增行
    int       sig;              // 提醒列表签名，变化时重建行文本
    int       edit_index;       // 正在编辑的提醒下标
    int       edit_values[5];   // 提醒用前 3 项，番茄钟设置用全部 5 项
} s;

// ---------------------------------------------------------------------------
// 番茄钟渲染
// ---------------------------------------------------------------------------

static void render_pomodoro(void)
{
    if (!s.pomo_time) return;

    app_pomodoro_t *p = app_state_pomodoro();

    int remain = p->remaining_seconds;
    if (p->state == APP_POMO_IDLE) remain = p->focus_minutes * 60;
    if (remain < 0) remain = 0;
    lv_label_set_text_fmt(s.pomo_time, "%02d:%02d", remain / 60, remain % 60);

    lv_label_set_text(s.pomo_state, app_pomodoro_state_name(p->state));
    // 沿用主题里的语义色，不新增颜色：专注=强调色，两种休息=正常绿，暂停=即将橙，空闲=灰。
    uint32_t state_color;
    switch (p->state) {
    case APP_POMO_FOCUS:      state_color = ui_c_accent(); break;
    case APP_POMO_BREAK:      state_color = ui_c_ok();     break;
    case APP_POMO_LONG_BREAK: state_color = ui_c_ok();     break;
    case APP_POMO_PAUSED:     state_color = ui_c_soon();   break;
    default:                  state_color = ui_c_dim();    break;
    }
    lv_obj_set_style_text_color(s.pomo_state, lv_color_hex(state_color), 0);

    ui_progress_set(s.pomo_bar, app_pomodoro_progress_permille(p));

    // 第一行给长休息周期：还没开始过时改为三段时长摘要，方便一眼确认当前设置。
    if (p->state == APP_POMO_IDLE && p->completed_focus == 0 &&
        p->cycles_since_long_break == 0) {
        lv_label_set_text_fmt(s.pomo_info, "专注 %d · 短休 %d · 长休 %d 分",
                              p->focus_minutes, p->break_minutes, p->long_break_minutes);
    } else {
        lv_label_set_text_fmt(s.pomo_info, "本轮 %d/%d 段后长休",
                              p->cycles_since_long_break, p->cycles_per_long_break);
    }

    // 第二行统计：今日与历史累计分开写，避免把累计数字误读成今日成绩。
    lv_label_set_text_fmt(s.pomo_stats, "今日 %d 分 %d 段 · 累计 %d 段 %d 分",
                          p->focus_minutes_today, p->today_sessions,
                          p->total_focus_sessions, p->total_focus_minutes);
}

static void render_pomodoro_focus(void)
{
    if (!s.pomo_card) return;
    bool on = (s.focus == 0);
    lv_obj_set_style_border_color(s.pomo_card,
        lv_color_hex(on ? ui_c_accent() : ui_c_border()), 0);
    lv_obj_set_style_border_width(s.pomo_card, on ? 2 : 1, 0);
}

// ---------------------------------------------------------------------------
// 提醒列表
// ---------------------------------------------------------------------------

// 重复方式的显示名。预设之外的掩码（如从手机配置页导入）交给逻辑层格式化。
static void repeat_text(const app_reminder_t *r, char *out, size_t cap)
{
    if (!r->repeat_weekly) {
        snprintf(out, cap, "%d/%d", r->month, r->day);
        return;
    }
    if (r->weekday_mask == REMINDER_PRESET_DAILY) {
        snprintf(out, cap, "每天");
        return;
    }
    if (r->weekday_mask == REMINDER_PRESET_WORKDAY) {
        snprintf(out, cap, "工作日");
        return;
    }
    if (app_reminder_schedule_text(r, out, cap) < 0) snprintf(out, cap, "每周");
}

static int reminder_sig(void)
{
    app_reminder_list_t *list = app_state_reminders();
    int sig = list->count;
    for (int i = 0; i < list->count; i++) {
        const app_reminder_t *r = &list->items[i];
        sig = sig * 31 + r->hour * 60 + r->minute;
        sig = sig * 31 + (r->enabled ? 1 : 0);
        sig = sig * 31 + (r->repeat_weekly ? r->weekday_mask : 0);
        sig = sig * 31 + (r->repeat_weekly ? 0 : r->month * 100 + r->day);
    }
    return sig;
}

static void reminder_row_text(int index)
{
    app_reminder_list_t *list = app_state_reminders();
    if (index < 0 || index >= list->count) return;

    const app_reminder_t *r = &list->items[index];
    char when[16];
    repeat_text(r, when, sizeof(when));

    char title[40];
    snprintf(title, sizeof(title), "%02d:%02d %s", r->hour, r->minute, when);

    ui_row_t row = s.rows[index];
    lv_obj_t *title_lbl = lv_obj_get_child(row.obj, 1);
    if (title_lbl) lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl,
        lv_color_hex(r->enabled ? ui_c_text() : ui_c_dim()), 0);
    ui_row_set_value(row, r->enabled ? "开" : "关");
    if (row.value) {
        lv_obj_set_style_text_color(row.value,
            lv_color_hex(r->enabled ? ui_c_ok() : ui_c_dim()), 0);
    }
}

// 重建行：提醒条数变化时调用。列表末尾始终保留"新增提醒"行。
static void reminder_rows_build(void)
{
    app_reminder_list_t *list = app_state_reminders();
    if (!s.list) return;

    for (int i = 0; i < s.row_count + 1 && i <= APP_REMINDER_MAX; i++) {
        if (s.rows[i].obj) {
            lv_obj_delete(s.rows[i].obj);
            s.rows[i].obj = NULL;
        }
    }

    s.row_count = list->count;
    for (int i = 0; i < list->count; i++) {
        s.rows[i] = ui_row_create(s.list, "", "");
        reminder_row_text(i);
    }
    s.rows[list->count] = ui_row_create(s.list, "＋ 新增提醒", "");
    if (s.rows[list->count].value) {
        lv_obj_set_style_text_color(s.rows[list->count].value,
            lv_color_hex(ui_c_accent()), 0);
    }

    if (s.focus > list->count) s.focus = list->count;
    if (s.focus < 0) s.focus = 0;
}

static void render_section(void)
{
    if (!s.section) return;
    app_reminder_list_t *list = app_state_reminders();
    if (list->count >= APP_REMINDER_MAX) {
        lv_label_set_text_fmt(s.section, "提醒 %d/%d · 已满，长按↓ 删除",
                              list->count, APP_REMINDER_MAX);
    } else {
        lv_label_set_text_fmt(s.section, "提醒 %d/%d", list->count, APP_REMINDER_MAX);
    }
    lv_obj_set_style_text_color(s.section,
        lv_color_hex(list->count >= APP_REMINDER_MAX ? ui_c_warn() : ui_c_dim()), 0);
}

static void render_focus(void)
{
    render_pomodoro_focus();
    app_reminder_list_t *list = app_state_reminders();
    for (int i = 0; i <= list->count; i++) {
        ui_row_set_selected(s.rows[i], i + 1 == s.focus);
    }
    if (s.focus > 0 && s.focus <= list->count + 1) {
        ui_scroll_into_view(s.rows[s.focus - 1].obj);
    }
}

static void update_hint(void)
{
    app_reminder_list_t *list = app_state_reminders();
    if (s.focus == 0) {
        ui_page_set_hint("OK 开始/暂停  长按↑↓ 设置/预设  长按OK 返回");
    } else if (s.focus == list->count + 1) {
        ui_page_set_hint("OK 新增提醒  长按OK 返回");
    } else {
        ui_page_set_hint("OK 开关  ↑↓选  长按↑ 修改  长按↓ 删除");
    }
}

static void move_focus(int delta)
{
    app_reminder_list_t *list = app_state_reminders();
    int max = list->count + 1;
    s.focus += delta;
    if (s.focus < 0) s.focus = 0;
    if (s.focus > max) s.focus = max;
    render_focus();
    update_hint();
}

// ---------------------------------------------------------------------------
// 编辑与增删
// ---------------------------------------------------------------------------

static int repeat_mode_of(const app_reminder_t *r)
{
    if (!r->repeat_weekly) return 2;
    if (r->weekday_mask == REMINDER_PRESET_WORKDAY) return 1;
    return 0;
}

static void apply_repeat(app_reminder_t *r, int mode, const app_datetime_t *today)
{
    switch (mode) {
    case 1:
        r->repeat_weekly = true;
        r->weekday_mask = REMINDER_PRESET_WORKDAY;
        break;
    case 2:
        r->repeat_weekly = false;
        r->year = today->year;
        r->month = today->month;
        r->day = today->day;
        break;
    default:
        r->repeat_weekly = true;
        r->weekday_mask = REMINDER_PRESET_DAILY;
        break;
    }
}

static void reminder_edit_done(bool saved, void *user)
{
    (void)user;
    if (!saved) return;

    app_reminder_list_t *list = app_state_reminders();
    if (s.edit_index < 0 || s.edit_index >= list->count) return;

    app_reminder_t *r = &list->items[s.edit_index];
    app_datetime_t today = app_state_now();
    r->hour = s.edit_values[0];
    r->minute = s.edit_values[1];
    apply_repeat(r, s.edit_values[2], &today);
    r->enabled = true;

    app_state_save_reminders();
    reminder_row_text(s.edit_index);
    render_section();
    ui_hint_flash("提醒已保存", 1500);
}

static void reminder_edit_open(void)
{
    app_reminder_list_t *list = app_state_reminders();
    int index = s.focus - 1;
    if (index < 0 || index >= list->count) return;
    if (ui_timeedit_active()) return;

    app_reminder_t *r = &list->items[index];
    s.edit_index = index;
    s.edit_values[0] = r->hour;
    s.edit_values[1] = r->minute;
    s.edit_values[2] = repeat_mode_of(r);

    static const ui_timeedit_field_t fields[3] = {
        { "时",   0, 23, 5, NULL },
        { "分",   0, 59, 10, NULL },
        { "重复", 0, 2, 1, REPEAT_NAMES },
    };
    ui_timeedit_open(s.page.scr, "提醒时间", fields, s.edit_values, 3,
                     reminder_edit_done, NULL);
}

static void reminder_delete_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;

    app_reminder_list_t *list = app_state_reminders();
    int index = s.focus - 1;
    if (index < 0 || index >= list->count) return;

    if (!app_reminder_remove(list, index)) return;
    app_state_save_reminders();

    reminder_rows_build();
    if (s.focus > list->count) s.focus = list->count > 0 ? list->count : 0;
    render_section();
    render_focus();
    update_hint();
    ui_hint_flash("提醒已删除", 1500);
}

static void reminder_delete_open(void)
{
    app_reminder_list_t *list = app_state_reminders();
    if (s.focus <= 0 || s.focus > list->count) return;

    const app_reminder_t *r = &list->items[s.focus - 1];
    char body[64];
    snprintf(body, sizeof(body), "%02d:%02d 的提醒将被移除，无法恢复。",
             r->hour, r->minute);
    ui_dialog_open(s.page.scr, "删除这条提醒？", body, "删除",
                   reminder_delete_confirm, NULL);
}

static void reminder_add(void)
{
    app_reminder_list_t *list = app_state_reminders();
    if (list->count >= APP_REMINDER_MAX) {
        ui_hint_flash("提醒已满 16 条，请先删除", 2000);
        return;
    }

    app_reminder_t seed;
    memset(&seed, 0, sizeof(seed));
    seed.enabled = true;
    seed.hour = 8;
    seed.minute = 0;
    seed.repeat_weekly = true;
    seed.weekday_mask = REMINDER_PRESET_DAILY;
    snprintf(seed.label, sizeof(seed.label), "提醒");

    int index = app_reminder_add(list, &seed);
    if (index < 0) {
        ui_hint_flash("新增失败", 1500);
        return;
    }
    app_state_save_reminders();
    s.focus = index + 1;
    reminder_rows_build();
    render_section();
    render_focus();
    update_hint();
    ui_hint_flash("已新增 08:00 提醒", 1500);
}

// 番茄钟设置：长按↑ 打开全部时长/循环/接续字段。字段偏多，名字压到 2~3 字以适配 5 列。
static void pomodoro_edit_done(bool saved, void *user)
{
    (void)user;
    if (!saved) return;

    app_pomodoro_t *p = app_state_pomodoro();

    // 自动接续不影响当前段长度，任何状态下都可改；时长与循环只有空闲时逻辑层才接受。
    bool changed = app_pomodoro_set_auto_next(p, s.edit_values[4] != 0);
    if (p->state == APP_POMO_IDLE) {
        bool dur = app_pomodoro_set_durations(p, s.edit_values[0], s.edit_values[1]);
        bool lng = app_pomodoro_set_long_break(p, s.edit_values[2], s.edit_values[3]);
        changed = dur || lng || changed;
        if (!changed) {
            ui_hint_flash("设置没有变化", 1500);
            return;
        }
        app_state_save_pomodoro();
        render_pomodoro();
        ui_hint_flash("设置已保存", 1500);
        return;
    }

    // 运行中被逻辑层拒绝时如实告知，而不是静默失败让人以为改成功了。
    if (changed) {
        app_state_save_pomodoro();
        render_pomodoro();
    }
    ui_hint_flash(changed ? "计时中：仅接续已更新" : "计时中不可改时长与循环", 2000);
}

static void pomodoro_edit_open(void)
{
    if (ui_timeedit_active()) return;
    app_pomodoro_t *p = app_state_pomodoro();

    s.edit_values[0] = p->focus_minutes;
    s.edit_values[1] = p->break_minutes;
    s.edit_values[2] = p->long_break_minutes;
    s.edit_values[3] = p->cycles_per_long_break;
    s.edit_values[4] = p->auto_next ? 1 : 0;

    static const ui_timeedit_field_t fields[5] = {
        { "专注", APP_POMO_FOCUS_MIN_MIN, APP_POMO_FOCUS_MIN_MAX, 5, NULL },
        { "短休", APP_POMO_BREAK_MIN_MIN, APP_POMO_BREAK_MIN_MAX, 5, NULL },
        { "长休", APP_POMO_LONG_MIN_MIN,  APP_POMO_LONG_MIN_MAX,  5, NULL },
        { "循环", APP_POMO_CYCLES_MIN,    APP_POMO_CYCLES_MAX,    1, NULL },
        { "接续", 0, 1, 1, POMO_AUTO_NAMES },
    };
    ui_timeedit_open(s.page.scr, "番茄钟设置", fields, s.edit_values, 5,
                     pomodoro_edit_done, NULL);
}

// 当前专注/短休息时长命中的预设下标；都不命中则返回"自定义"。
static int preset_index_of(int focus_minutes, int break_minutes)
{
    for (int i = 0; i < POMO_PRESET_COUNT; i++) {
        if (POMO_PRESETS[i][0] == focus_minutes && POMO_PRESETS[i][1] == break_minutes) {
            return i;
        }
    }
    return POMO_PRESET_COUNT;
}

static void pomodoro_preset_done(bool saved, void *user)
{
    (void)user;
    if (!saved) return;

    int index = s.edit_values[0];
    if (index >= POMO_PRESET_COUNT) {
        ui_hint_flash("自定义请长按↑ 逐项设置", 2000);
        return;
    }

    app_pomodoro_t *p = app_state_pomodoro();
    if (!app_pomodoro_set_durations(p, POMO_PRESETS[index][0], POMO_PRESETS[index][1])) {
        ui_hint_flash(p->state == APP_POMO_IDLE ? "与当前设置相同" : "计时中不可改时长", 1800);
        return;
    }
    app_state_save_pomodoro();
    render_pomodoro();
    ui_hint_flash("已套用预设", 1500);
}

static void pomodoro_preset_open(void)
{
    if (ui_timeedit_active()) return;
    app_pomodoro_t *p = app_state_pomodoro();
    if (p->state != APP_POMO_IDLE) {
        ui_hint_flash("计时中不可改时长", 1800);
        return;
    }

    s.edit_values[0] = preset_index_of(p->focus_minutes, p->break_minutes);

    static const ui_timeedit_field_t fields[1] = {
        { "预设", 0, POMO_PRESET_COUNT, 1, POMO_PRESET_NAMES },
    };
    ui_timeedit_open(s.page.scr, "番茄钟预设", fields, s.edit_values, 1,
                     pomodoro_preset_done, NULL);
}

static void activate(void)
{
    app_reminder_list_t *list = app_state_reminders();

    if (s.focus == 0) {
        app_pomodoro_t *p = app_state_pomodoro();
        if (p->state == APP_POMO_IDLE) {
            if (!app_pomodoro_start(p)) return;
            ui_hint_flash("专注开始", 1500);
        } else {
            app_pomodoro_toggle(p);
        }
        app_state_save_pomodoro();
        render_pomodoro();
        return;
    }

    if (s.focus == list->count + 1) {
        reminder_add();
        return;
    }

    int index = s.focus - 1;
    if (index < 0 || index >= list->count) return;
    app_reminder_t *r = &list->items[index];
    r->enabled = !r->enabled;
    app_state_save_reminders();
    reminder_row_text(index);
    ui_hint_flash(r->enabled ? "提醒已开启" : "提醒已关闭", 1200);
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_focus_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.page = ui_page_create("OK 开始/暂停  长按↑↓ 设置/预设  长按OK 返回");

    ui_header_create(s.page.content, "专注与效率", "", NULL, NULL);

    // 番茄钟卡：大号剩余时间 + 阶段 + 进度条 + 长休周期 + 统计。
    s.pomo_card = ui_card_create(s.page.content, 0, 0, FOCUS_CW, POMO_CARD_H,
                                 ui_c_accent());
    s.pomo_time = ui_label_create(s.pomo_card, "25:00", ui_font_display, ui_c_text());
    lv_obj_set_pos(s.pomo_time, 12, 4);
    s.pomo_state = ui_label_create(s.pomo_card, "", ui_font_hint, ui_c_dim());
    lv_obj_align(s.pomo_state, LV_ALIGN_TOP_RIGHT, -12, 14);
    s.pomo_bar = ui_progress_create(s.pomo_card, FOCUS_CW - 24, 6, ui_c_accent());
    lv_obj_set_pos(s.pomo_bar, 12, 48);
    s.pomo_info = ui_label_create(s.pomo_card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(s.pomo_info, 12, 60);
    s.pomo_stats = ui_label_create(s.pomo_card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(s.pomo_stats, 12, 75);

    s.section = ui_label_create(s.page.content, "提醒", ui_font_hint, ui_c_dim());
    s.list = ui_list_create(s.page.content);

    reminder_rows_build();
    render_section();
    render_pomodoro();

    s.focus = 0;
    s.sig = reminder_sig();
    render_focus();
    update_hint();
}

void page_focus_exit(void)
{
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_focus_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ui_timeedit_active()) {
        ui_timeedit_handle(btn, ev);
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (s.focus == 0) pomodoro_edit_open();
        else if (s.focus == app_state_reminders()->count + 1) reminder_add();
        else reminder_edit_open();
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        if (s.focus == 0) pomodoro_preset_open();
        else if (s.focus >= 1 && s.focus <= app_state_reminders()->count) {
            reminder_delete_open();
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) move_focus(-1);
    else if (btn == BSP_BTN_DOWN) move_focus(1);
    else if (btn == BSP_BTN_OK) activate();
}

void page_focus_tick(void)
{
    if (!s.page.scr) return;

    render_pomodoro();

    int sig = reminder_sig();
    if (sig != s.sig) {
        bool count_changed = (app_state_reminders()->count != s.row_count);
        s.sig = sig;
        if (count_changed) {
            reminder_rows_build();
            render_focus();
            update_hint();
        } else {
            for (int i = 0; i < app_state_reminders()->count; i++) reminder_row_text(i);
        }
        render_section();
    }
}
