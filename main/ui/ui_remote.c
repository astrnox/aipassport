// main/ui/ui_remote.c —— 工具页子页：万能遥控（BLE HID 外设）。
//
// 设备对外是一个蓝牙键盘 + 一个消费类媒体控制器，主机（手机 / 电脑 / 电视盒子）不需要
// 装任何 App、也不需要联网：配对一次之后，三个键就是遥控器。本页只负责"显示当前模式
// 的键位"和"把按键转成 HID 报文"，具体发什么码在 logic/app_remote 里登记、可主机测试。
//
// 交互取舍：
//  - 分两级：先选模式（一屏四行，写明这个模式能干嘛），再进按键页。这样"三个键既是
//    遥控键、又要用来切模式"就不会打架。
//  - 长按 OK 恒为返回（先回模式选择、再回工具页），与其它子页一致。这条是硬约定：
//    按键回调在任何模式下发报文之前就把它消费掉，所以逻辑层也不再为长按 OK 登记任何
//    HID 映射——界面上每个"短按 / 长按"都必须是真能发出去的。次级动作因此统一落在
//    长按 ↑ / ↓：PPT 用长按 ↓ 退出放映、长按 ↑ 黑屏，拍照快门在 ↑ 短按（音量+，
//    绝大多数相机都认）。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"

#include "net/app_ble.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define RM_CW (UI_W - 2 * UI_MARGIN_X)

typedef enum {
    RM_PICK = 0,
    RM_ACTIVE,
} rm_view_t;

// 每个模式在列表里的一句话说明：普通用户先看懂"这套键在什么场景用"。
static const char *const RM_DESC[APP_REMOTE_MODE_COUNT] = {
    "翻页 / 返回书架",
    "音量 / 静音 / 播放",
    "翻页 / 放映 / 退出",
    "播放 / 音量 / 快门",
};

static struct {
    bool      active;
    ui_page_t page;
    rm_view_t view;
    int       mode;      // 模式选择页的焦点，也是当前进入的模式
    int       focus;
    bool      conn;
    bool      started;   // 是否已同步过一次连接状态，避免首帧误判为"变化"
    ui_row_t  rows[APP_REMOTE_MODE_COUNT];
    lv_obj_t *hdr_right;
} s;

static const char *conn_text(void)
{
    return s.conn ? "已连接" : "未连接";
}

// ---------------------------------------------------------------------------
// 模式选择页
// ---------------------------------------------------------------------------

static void pick_render_focus(void)
{
    for (int i = 0; i < APP_REMOTE_MODE_COUNT; i++) ui_row_set_selected(s.rows[i], i == s.focus);
    if (s.focus >= 0 && s.focus < APP_REMOTE_MODE_COUNT) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }
}

static void build_pick(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));

    ui_header_create(c, "万能遥控", conn_text(), NULL, &s.hdr_right);
    if (!s.conn) {
        char banner[96];
        snprintf(banner, sizeof(banner),
                 "在手机或电脑蓝牙设置里连接 %s，连上后三键就是遥控器",
                 app_ble_remote_name());
        ui_banner_create(c, banner, ui_c_warn());
    }

    lv_obj_t *list = ui_list_create(c);
    for (int i = 0; i < APP_REMOTE_MODE_COUNT; i++) {
        s.rows[i] = ui_row_create(list, app_remote_mode_name((app_remote_mode_t)i), RM_DESC[i]);
    }
    if (s.focus < 0) s.focus = 0;
    if (s.focus >= APP_REMOTE_MODE_COUNT) s.focus = APP_REMOTE_MODE_COUNT - 1;
    pick_render_focus();
    ui_page_set_hint("↑↓ 选择  OK 进入  长按OK 返回");
}

// ---------------------------------------------------------------------------
// 按键页
// ---------------------------------------------------------------------------

// 把某个键在本模式下的短按 / 长按动作拼成一行，如"短按 上一页 · 长按 黑屏"。
// 没有映射的那一档不写，两档都没有则写"无功能"，用户一眼知道这个键有没有用。
static void key_line(app_remote_btn_t bt, const char *name, char *out, size_t cap)
{
    const char *clk = app_remote_action_text((app_remote_mode_t)s.mode, bt, APP_REMOTE_CLICK);
    const char *lng = app_remote_action_text((app_remote_mode_t)s.mode, bt, APP_REMOTE_LONG);
    if (clk && lng) {
        snprintf(out, cap, "%s  短按 %s · 长按 %s", name, clk, lng);
    } else if (clk) {
        snprintf(out, cap, "%s  短按 %s", name, clk);
    } else if (lng) {
        snprintf(out, cap, "%s  长按 %s", name, lng);
    } else {
        snprintf(out, cap, "%s  无功能", name);
    }
}

static void build_active(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));

    ui_header_create(c, app_remote_mode_name((app_remote_mode_t)s.mode), conn_text(),
                     NULL, &s.hdr_right);
    if (!s.conn) {
        char banner[96];
        snprintf(banner, sizeof(banner),
                 "还没连接：请到手机或电脑蓝牙设置里找到 %s 并连接",
                 app_ble_remote_name());
        ui_banner_create(c, banner, ui_c_warn());
    }

    lv_obj_t *card = ui_card_create(c, 0, 0, RM_CW, 162, ui_c_accent());
    const app_remote_btn_t btns[3] = {
        APP_REMOTE_BTN_UP, APP_REMOTE_BTN_DOWN, APP_REMOTE_BTN_OK
    };
    const char *names[3] = { "↑ 上键", "↓ 下键", "OK 确认" };
    for (int i = 0; i < 3; i++) {
        char line[64];
        key_line(btns[i], names[i], line, sizeof(line));
        lv_obj_t *lbl = ui_label_create(card, line, ui_font_body, ui_c_text());
        lv_obj_set_width(lbl, RM_CW - 24);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(lbl, 12, 12 + i * 48);
    }

    ui_page_set_hint("短按发送  长按↑↓ 发组合  长按OK 返回");
}

static void build_current(void)
{
    if (s.view == RM_ACTIVE) build_active();
    else build_pick();
}

// 真正发报文。先查映射、再看连接，最后才发；任何一步不通过都给一句人话，
// 不做"按了没反应"的哑巴交互。
static void send_key(app_remote_btn_t bt, app_remote_press_t pr)
{
    const char *txt = app_remote_action_text((app_remote_mode_t)s.mode, bt, pr);
    if (!txt) {
        ui_hint_flash("此模式没有这个功能", 1200);
        return;
    }
    if (!app_ble_remote_connected()) {
        ui_hint_flash("先在蓝牙设置里连接本设备", 1800);
        return;
    }
    if (app_ble_remote_press((app_remote_mode_t)s.mode, bt, pr)) {
        ui_hint_flash(txt, 700);
    } else {
        ui_hint_flash("发送失败，请重试", 1200);
    }
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_remote_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.view = RM_PICK;
    s.page = ui_page_create(NULL);

    app_ble_remote_request_start();
    s.conn = app_ble_remote_connected();
    s.started = true;
    build_pick();
    ui_hint_flash("正在打开蓝牙…", 1200);
}

void page_remote_exit(void)
{
    if (!s.active && !s.page.scr) return;
    app_ble_remote_request_stop();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_remote_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s.view == RM_ACTIVE) {
            s.view = RM_PICK;      // 先回模式选择
            build_pick();
        } else {
            page_remote_exit();    // 再按一次才退出遥控
        }
        return;
    }

    if (s.view == RM_ACTIVE) {
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP)        send_key(APP_REMOTE_BTN_UP, APP_REMOTE_CLICK);
            else if (btn == BSP_BTN_DOWN) send_key(APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK);
            else                          send_key(APP_REMOTE_BTN_OK, APP_REMOTE_CLICK);
        } else if (ev == BSP_BTN_LONG) {
            if (btn == BSP_BTN_UP)        send_key(APP_REMOTE_BTN_UP, APP_REMOTE_LONG);
            else if (btn == BSP_BTN_DOWN) send_key(APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG);
        }
        return;
    }

    // ---- 模式选择页 ----
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s.focus = (s.focus + APP_REMOTE_MODE_COUNT - 1) % APP_REMOTE_MODE_COUNT;
        pick_render_focus();
    } else if (btn == BSP_BTN_DOWN) {
        s.focus = (s.focus + 1) % APP_REMOTE_MODE_COUNT;
        pick_render_focus();
    } else if (btn == BSP_BTN_OK) {
        s.mode = s.focus;
        s.view = RM_ACTIVE;
        build_active();
    }
}

void page_remote_tick(void)
{
    if (!s.active || !s.page.scr) return;
    bool conn = app_ble_remote_connected();
    if (conn != s.conn) {
        s.conn = conn;
        build_current();   // 连接状态一变就重画横幅与页眉，不留下"没连上还以为连上了"
    }
}

bool page_remote_active(void)
{
    return s.active;
}