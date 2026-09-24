// main/ui/ui_routine.c —— 3 作息与倒计时：今日时间轴 / 一周概览 / 配置。
//
// 页面围绕两个问题组织：现在该做什么、还有多久。顶部大字倒计时回答"还有多久"，
// 下方时间轴回答"今天一整天怎么排"。三个标签页各自独立：
//   今日  大字倒计时 + 今日节点时间轴，当前节点高亮、已过节点弱化
//   一周  七天的节点数与首末时间一览，用于快速核对整周安排
//   设置  套用走读/住校模板、清空今日或全部，以及从手机配置页导入的入口说明
//
// 按键（与全局约定一致）：
//   短按 UP/DOWN  今日=滚动时间轴  一周=选择星期  设置=选择条目
//   短按 OK       今日=定位当前节点 一周=读出该天概要 设置=执行选中项
//   长按 UP       今日=刷新  一周=刷新  设置=执行选中项
//   长按 DOWN     切换标签页
//   长按 OK       返回主页
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_state.h"
#include "logic/app_routine.h"
#include "logic/app_text.h"
#include "logic/app_time.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define RT_CW        (UI_W - 2 * UI_MARGIN_X)   // 224
#define RT_TAB_COUNT 3
#define RT_CD_H      78
#define RT_OPT_COUNT 5

enum {
    RT_OPT_TEMPLATE_DAY = 0,
    RT_OPT_TEMPLATE_BOARD,
    RT_OPT_CLEAR_TODAY,
    RT_OPT_CLEAR_ALL,
    RT_OPT_IMPORT_HELP,
};

static const char *const RT_TAB_NAMES[RT_TAB_COUNT] = { "今日", "一周", "设置" };
static const char *const RT_OPT_NAMES[RT_OPT_COUNT] = {
    "套用走读模板", "套用住校模板", "清空今日作息", "清空全部作息", "从手机导入"
};

// 一周视图按"周一..周日"排列，映射到数据模型（0=周日）。
static const int RT_WEEK_ORDER[7] = { 1, 2, 3, 4, 5, 6, 0 };
static const char *const RT_WEEK_NAMES[7] = {
    "周一", "周二", "周三", "周四", "周五", "周六", "周日"
};

static struct {
    ui_page_t page;
    lv_obj_t *tabs;
    lv_obj_t *views[RT_TAB_COUNT];
    int tab;

    // 今日
    lv_obj_t *cd_title;
    lv_obj_t *cd_value;
    lv_obj_t *cd_sub;
    lv_obj_t *today_hdr_right;
    lv_obj_t *today_list;
    ui_row_t  nodes[APP_ROUTINE_MAX_NODES];
    int       node_count;
    int       focus;          // -1 表示"自动定位到当前节点"
    int       built_weekday;  // 已构建的星期，跨天时重建

    // 一周
    ui_row_t  week[7];
    int       week_sel;

    // 设置
    ui_row_t  opts[RT_OPT_COUNT];
    int       opt_sel;
} s;

// ---------------------------------------------------------------------------
// 今日视图
// ---------------------------------------------------------------------------

static int today_weekday(void)
{
    app_datetime_t now = app_state_now();
    int wd = app_time_weekday(now.year, now.month, now.day);
    if (wd < 0 || wd >= APP_ROUTINE_DAYS) wd = 0;
    return wd;
}

static const app_routine_day_t *today_day(void)
{
    return &app_state_routine()->days[today_weekday()];
}

// 当前节点下标；处于空档则返回下一个节点；今日已结束返回最后一个节点。
static int current_node_index(const app_routine_day_t *day)
{
    if (!day || day->count <= 0) return 0;
    app_datetime_t now = app_state_now();
    app_routine_status_t st;
    app_routine_status(&*day, now.hour * 60 + now.minute, now.second, &st);
    if (st.current_index >= 0) return st.current_index;
    if (st.next_index >= 0) return st.next_index;
    return day->count - 1;
}

static void render_countdown(void)
{
    if (!s.cd_title) return;

    app_datetime_t now = app_state_now();
    const app_routine_day_t *day = today_day();
    if (!day || day->count <= 0) {
        lv_label_set_text(s.cd_title, "今日无作息");
        lv_label_set_text(s.cd_value, "--:--");
        lv_label_set_text(s.cd_sub, "到设置里套用一套模板即可开始");
        return;
    }

    app_routine_status_t st;
    app_routine_status(&*day, now.hour * 60 + now.minute, now.second, &st);
    char cd[16];

    if (st.pos == APP_ROUTINE_IN_NODE && st.current_index >= 0) {
        const app_routine_node_t *cur = &day->nodes[st.current_index];
        char t1[8], t2[8];
        app_fmt_hhmm(t1, sizeof(t1), cur->start_min);
        app_fmt_hhmm(t2, sizeof(t2), cur->end_min);
        if (st.next_index >= 0) {
            char title[40];
            snprintf(title, sizeof(title), "距离%s", day->nodes[st.next_index].name);
            lv_label_set_text(s.cd_title, title);
            app_fmt_countdown(cd, sizeof(cd), st.seconds_to_next);
        } else {
            lv_label_set_text(s.cd_title, "距离本节结束");
            app_fmt_countdown(cd, sizeof(cd), st.seconds_to_end);
        }
        lv_label_set_text(s.cd_value, cd);
        char sub[48];
        snprintf(sub, sizeof(sub), "%s %s - %s", cur->name, t1, t2);
        lv_label_set_text(s.cd_sub, sub);
    } else if (st.pos == APP_ROUTINE_BETWEEN && st.next_index >= 0) {
        const app_routine_node_t *nx = &day->nodes[st.next_index];
        char title[40];
        snprintf(title, sizeof(title), "距离%s", nx->name);
        lv_label_set_text(s.cd_title, title);
        app_fmt_countdown(cd, sizeof(cd), st.seconds_to_next);
        lv_label_set_text(s.cd_value, cd);
        char sub[48];
        char t1[8];
        app_fmt_hhmm(t1, sizeof(t1), nx->start_min);
        snprintf(sub, sizeof(sub), "下一个 %s %s", nx->name, t1);
        lv_label_set_text(s.cd_sub, sub);
    } else {
        lv_label_set_text(s.cd_title, "今日作息已结束");
        lv_label_set_text(s.cd_value, "--:--");
        lv_label_set_text(s.cd_sub, "好好休息，明天见");
    }
}

// 刷新时间轴各行的文本与颜色；行数与构建时一致，不做增删。
static void render_today_rows(void)
{
    if (!s.today_list || s.node_count <= 0) return;

    const app_routine_day_t *day = today_day();
    app_datetime_t now = app_state_now();
    int now_min = now.hour * 60 + now.minute;

    for (int i = 0; i < s.node_count && i < day->count; i++) {
        const app_routine_node_t *n = &day->nodes[i];
        char t1[8], t2[8];
        app_fmt_hhmm(t1, sizeof(t1), n->start_min);
        app_fmt_hhmm(t2, sizeof(t2), n->end_min);

        char title[40];
        snprintf(title, sizeof(title), "%s %s", t1, n->name);

        bool done = (n->end_min <= now_min);
        bool live = (n->start_min <= now_min && now_min < n->end_min);

        ui_row_t row = s.nodes[i];
        lv_obj_t *title_lbl = lv_obj_get_child(row.obj, 1);
        if (title_lbl) {
            lv_label_set_text(title_lbl, title);
            lv_obj_set_style_text_color(title_lbl,
                lv_color_hex(done ? ui_c_dim() : ui_c_text()), 0);
        }
        ui_row_set_value(row, done ? "已过" : live ? "进行中" : t2);
        if (row.value) {
            lv_obj_set_style_text_color(row.value,
                lv_color_hex(live ? ui_c_accent() : done ? ui_c_done() : ui_c_dim()), 0);
        }
    }
}

static void today_focus(int index)
{
    if (s.node_count <= 0) return;
    if (index < 0) index = 0;
    if (index >= s.node_count) index = s.node_count - 1;
    s.focus = index;
    for (int i = 0; i < s.node_count; i++) {
        ui_row_set_selected(s.nodes[i], i == index);
    }
    ui_scroll_into_view(s.nodes[index].obj);
}

// 结构可能已变：清空后按当前数据重建整个今日标签页。
static void build_today(void)
{
    lv_obj_t *v = s.views[0];
    lv_obj_clean(v);

    s.today_hdr_right = NULL;
    s.today_list = NULL;
    s.node_count = 0;
    memset(s.nodes, 0, sizeof(s.nodes));

    const app_routine_day_t *day = today_day();
    s.built_weekday = today_weekday();

    ui_header_create(v, "作息", NULL, NULL, &s.today_hdr_right);
    if (s.today_hdr_right) {
        app_datetime_t now = app_state_now();
        char d[24];
        app_fmt_date_short(d, sizeof(d), now.month, now.day, s.built_weekday);
        lv_label_set_text(s.today_hdr_right, d);
    }

    lv_obj_t *card = ui_card_create(v, 0, 0, RT_CW, RT_CD_H, ui_c_accent());
    s.cd_title = ui_label_create(card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(s.cd_title, 12, 8);
    s.cd_value = ui_label_create(card, "--:--", ui_font_display_s, ui_c_text());
    lv_obj_set_pos(s.cd_value, 12, 24);
    s.cd_sub = ui_label_create(card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(s.cd_sub, 12, 58);

    render_countdown();

    if (!day || day->count <= 0) {
        ui_empty_create(v, "还没有作息表",
                        "到"设置"里一键套用走读或住校模板，"
                        "也可以在手机配置页粘贴自己的作息文本");
        ui_page_set_hint("长按↓ 到设置套用模板  长按OK 返回");
        return;
    }

    s.today_list = ui_list_create(v);
    s.node_count = day->count;
    for (int i = 0; i < s.node_count; i++) {
        s.nodes[i] = ui_row_create(s.today_list, "", "");
    }
    if (s.focus < 0) s.focus = current_node_index(day);
    render_today_rows();
    today_focus(s.focus);
    ui_page_set_hint("↑↓ 滚动  OK 定位当前  长按↓ 换页");
}

// ---------------------------------------------------------------------------
// 一周视图
// ---------------------------------------------------------------------------

static void week_refresh(void)
{
    for (int i = 0; i < 7; i++) {
        const app_routine_day_t *d = &app_state_routine()->days[RT_WEEK_ORDER[i]];
        char val[32];
        if (d->count <= 0) {
            snprintf(val, sizeof(val), "无安排");
        } else {
            char t1[8], t2[8];
            app_fmt_hhmm(t1, sizeof(t1), d->nodes[0].start_min);
            app_fmt_hhmm(t2, sizeof(t2), d->nodes[d->count - 1].end_min);
            snprintf(val, sizeof(val), "%d 节 %s-%s", d->count, t1, t2);
        }
        ui_row_set_value(s.week[i], val);
        if (s.week[i].value) {
            lv_obj_set_style_text_color(s.week[i].value,
                lv_color_hex(d->count <= 0 ? ui_c_dim() : ui_c_text()), 0);
        }
    }
}

static void week_select(int index)
{
    if (index < 0) index = 0;
    if (index > 6) index = 6;
    s.week_sel = index;
    for (int i = 0; i < 7; i++) {
        ui_row_set_selected(s.week[i], i == index);
    }
    ui_scroll_into_view(s.week[index].obj);
}

static void build_week(void)
{
    lv_obj_t *v = s.views[1];
    lv_obj_clean(v);

    ui_header_create(v, "一周作息", NULL, NULL, NULL);
    lv_obj_t *list = ui_list_create(v);
    for (int i = 0; i < 7; i++) {
        s.week[i] = ui_row_create(list, RT_WEEK_NAMES[i], "");
    }
    week_refresh();

    int wd = today_weekday();
    week_select(wd == 0 ? 6 : wd - 1);
    ui_page_set_hint("↑↓ 选择  OK 读出概要  长按↓ 换页");
}

// ---------------------------------------------------------------------------
// 设置视图
// ---------------------------------------------------------------------------

static void opt_select(int index)
{
    if (index < 0) index = 0;
    if (index >= RT_OPT_COUNT) index = RT_OPT_COUNT - 1;
    s.opt_sel = index;
    for (int i = 0; i < RT_OPT_COUNT; i++) {
        ui_row_set_selected(s.opts[i], i == index);
    }
    ui_scroll_into_view(s.opts[index].obj);
}

static void build_opts(void)
{
    lv_obj_t *v = s.views[2];
    lv_obj_clean(v);

    ui_header_create(v, "作息配置", NULL, NULL, NULL);
    lv_obj_t *list = ui_list_create(v);
    for (int i = 0; i < RT_OPT_COUNT; i++) {
        s.opts[i] = ui_row_create(list, RT_OPT_NAMES[i], "");
    }
    if (s.opts[RT_OPT_IMPORT_HELP].value) {
        lv_obj_set_style_text_color(s.opts[RT_OPT_IMPORT_HELP].value,
            lv_color_hex(ui_c_accent()), 0);
    }
    opt_select(s.opt_sel);
    ui_page_set_hint("↑↓ 选择  OK 执行  长按↓ 换页");
}

static void after_data_change(void)
{
    build_today();
    week_refresh();
}

static void apply_template(bool boarding)
{
    app_routine_load_template(app_state_routine(), boarding);
    app_state_save_routine();
    s.focus = -1;              // 重新自动定位到当前节点
    after_data_change();
    ui_hint_flash(boarding ? "已套用住校模板" : "已套用走读模板", 1500);
}

static void clear_today_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;
    app_routine_day_t *day = &app_state_routine()->days[today_weekday()];
    memset(day, 0, sizeof(*day));
    app_state_save_routine();
    s.focus = -1;
    after_data_change();
    ui_hint_flash("已清空今日作息", 1500);
}

static void clear_all_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;
    app_routine_init(app_state_routine());
    app_state_save_routine();
    s.focus = -1;
    after_data_change();
    ui_hint_flash("已清空全部作息", 1500);
}

static void opt_run(int index)
{
    switch (index) {
    case RT_OPT_TEMPLATE_DAY:
        apply_template(false);
        break;
    case RT_OPT_TEMPLATE_BOARD:
        apply_template(true);
        break;
    case RT_OPT_CLEAR_TODAY:
        ui_dialog_open(s.page.scr, "清空今日作息",
                       "今天全部作息节点将被移除，无法恢复。",
                       "清空", clear_today_confirm, NULL);
        break;
    case RT_OPT_CLEAR_ALL:
        ui_dialog_open(s.page.scr, "清空全部作息",
                       "七天的作息节点将全部移除，无法恢复。",
                       "清空", clear_all_confirm, NULL);
        break;
    default:
        ui_hint_flash("手机连上热点后，在配置页粘贴作息文本即可导入", 2400);
        break;
    }
}

// ---------------------------------------------------------------------------
// 标签页切换
// ---------------------------------------------------------------------------

static void show_tab(int index)
{
    if (index < 0) index = 0;
    if (index >= RT_TAB_COUNT) index = RT_TAB_COUNT - 1;
    s.tab = index;

    for (int i = 0; i < RT_TAB_COUNT; i++) {
        if (i == index) lv_obj_remove_flag(s.views[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s.views[i], LV_OBJ_FLAG_HIDDEN);
    }
    ui_tabs_select(s.tabs, index);

    switch (index) {
    case 0: build_today(); break;
    case 1: build_week();  break;
    default: build_opts(); break;
    }
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_routine_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.focus = -1;
    s.page = ui_page_create(NULL);
    s.tabs = ui_tabs_create(s.page.content, RT_TAB_NAMES, RT_TAB_COUNT);

    for (int i = 0; i < RT_TAB_COUNT; i++) {
        lv_obj_t *v = lv_obj_create(s.page.content);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(v, LV_PCT(100));
        lv_obj_set_height(v, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(v, 0, 0);
        lv_obj_set_style_radius(v, 0, 0);
        lv_obj_set_style_pad_all(v, 0, 0);
        lv_obj_set_style_pad_row(v, 6, 0);
        lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
        s.views[i] = v;
    }

    show_tab(0);
}

void page_routine_exit(void)
{
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_routine_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        show_tab((s.tab + 1) % RT_TAB_COUNT);
        return;
    }

    switch (s.tab) {
    case 0: {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            const app_routine_day_t *day = today_day();
            if (!day || day->count <= 0) {
                apply_template(false);
            } else {
                s.focus = current_node_index(day);
                render_today_rows();
                today_focus(s.focus);
                ui_hint_flash("已定位到当前节点", 1200);
            }
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) today_focus(s.focus - 1);
        else if (btn == BSP_BTN_DOWN) today_focus(s.focus + 1);
        else if (btn == BSP_BTN_OK) {
            const app_routine_day_t *day = today_day();
            if (!day || day->count <= 0) {
                ui_hint_flash("今日无作息，长按↑ 套用模板", 1800);
                return;
            }
            s.focus = current_node_index(day);
            render_today_rows();
            today_focus(s.focus);
            ui_hint_flash("已定位到当前节点", 1200);
        }
        return;
    }
    case 1: {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            week_refresh();
            ui_hint_flash("已刷新", 1000);
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) week_select(s.week_sel - 1);
        else if (btn == BSP_BTN_DOWN) week_select(s.week_sel + 1);
        else if (btn == BSP_BTN_OK) {
            const app_routine_day_t *d =
                &app_state_routine()->days[RT_WEEK_ORDER[s.week_sel]];
            char msg[48];
            if (d->count <= 0) {
                snprintf(msg, sizeof(msg), "%s 无安排", RT_WEEK_NAMES[s.week_sel]);
            } else {
                char t1[8], t2[8];
                app_fmt_hhmm(t1, sizeof(t1), d->nodes[0].start_min);
                app_fmt_hhmm(t2, sizeof(t2), d->nodes[d->count - 1].end_min);
                snprintf(msg, sizeof(msg), "%s 共 %d 节 %s-%s",
                         RT_WEEK_NAMES[s.week_sel], d->count, t1, t2);
            }
            ui_hint_flash(msg, 2000);
        }
        return;
    }
    default: {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            opt_run(s.opt_sel);
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) opt_select(s.opt_sel - 1);
        else if (btn == BSP_BTN_DOWN) opt_select(s.opt_sel + 1);
        else if (btn == BSP_BTN_OK) opt_run(s.opt_sel);
        return;
    }
    }
}

void page_routine_tick(void)
{
    if (!s.page.scr) return;

    // 跨天时今日标签页内容整体失效，重建一次。
    if (s.tab == 0 && today_weekday() != s.built_weekday) {
        build_today();
        return;
    }

    if (s.tab == 0 && s.today_list && s.node_count > 0) {
        render_countdown();
        render_today_rows();
    }
}
