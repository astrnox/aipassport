// main/ui/ui_vault.c —— 密码本页：身份与工具模块下的全屏子页面。
//
// 密码本完全离线可用，条目只存本机 NVS。设备只有 UP/DOWN/OK 三个键、没有键盘，所以
// 本页"能不用字母输入就不用"：新增与编辑一律引导去手机配网页，设备端只做删除；加密本
// 用 UP/DOWN 两种敲击组成手势解锁；31 位恢复码设备端无法输入，只给"去配网页解锁"的引导。
// 解锁状态由 app_vault 自己维护：本页重建（从详情返回、息屏唤醒后重进）不重置解锁状态，
// 只有 app_vault_lock() 或重启才回到锁定。
// 口令只在详情页短暂存在：离开详情立即清零本地缓冲，且绝不写任何日志。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_sound.h"
#include "ui_theme.h"

#include "app_state.h"

#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define VV_CW         (UI_W - 2 * UI_MARGIN_X)   // 内容区可用宽度 224
#define VV_PASS_MASK  "********"
#define VV_MODE_ROWS  4                          // 加密模式下模式页的行数上限

// 加密本闲置多久自动回锁。运行态参数，每次进入本页时设置，不进 NVS：设备重启本来就会
// 回到锁定，无需持久化。
#define VV_AUTOLOCK_S 120

typedef enum {
    VV_UNLOCK = 0,   // 加密且未解锁：敲击手势
    VV_LIST,         // 条目列表
    VV_DETAIL,       // 单条详情
    VV_MODES,        // 加密与数据
    VV_GESTURE,      // 设置手势
} vault_view_t;

typedef enum {
    OV_NONE = 0,
    OV_RECOVERY_CODE,   // 展示新生成的恢复码
    OV_RECOVERY_HELP,   // 忘记手势的引导
} vault_overlay_t;

static const char *const VV_PLAIN_NAMES[] = { "开启加密保护", "在配网页添加条目" };
static const char *const VV_PLAIN_HELP[]  = { "现场设置手势", "手机配置页" };
static const char *const VV_ENC_NAMES[] = {
    "退回明文保存", "重新生成恢复码", "立即上锁", "在配网页添加条目"
};
static const char *const VV_ENC_HELP[] = {
    "退回即免解锁", "旧码随即失效", "清空内存口令", "手机配置页"
};

static struct {
    ui_page_t    page;
    bool         active;
    vault_view_t view;

    app_vault_knock_t knock;    // 解锁与设置手势共用的敲击输入缓冲

    // 详情页：解密口令只在本缓冲里短暂存在，离开详情立即清零。
    char pass_buf[APP_VAULT_PASSWORD_LEN];
    bool pass_visible;

    // 列表
    ui_row_t rows[APP_VAULT_MAX];
    int      row_count;
    int      built_count;

    // 详情
    ui_row_t d_label;
    ui_row_t d_account;
    ui_row_t d_pass;

    // 模式页
    ui_row_t modes[VV_MODE_ROWS];
    int      mode_count;
    int      mode_sel;

    // 解锁 / 设置手势的计数标签
    lv_obj_t *unlock_count;
    lv_obj_t *gesture_count;
    lv_obj_t *gesture_right;

    // 覆盖层
    lv_obj_t       *overlay;
    vault_overlay_t overlay_kind;
    vault_view_t    overlay_return;
} s;

static void set_view(vault_view_t view);
static void build_unlock(void);
static void build_list(void);
static void build_detail(void);
static void build_modes(void);
static void build_gesture(void);
static void open_overlay(vault_overlay_t kind, vault_view_t back);
static void close_overlay(void);

// ---- 小工具 ----

static app_vault_t *vault(void) { return app_state_vault(); }

// 单调毫秒。自动回锁与失败退避都只关心时间差，用 esp_timer 的单调时钟即可，
// 与墙钟是否校准无关。
static uint32_t mono_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// 把自动回锁时长套到当前密码本上；明文模式下该设置无副作用（永不回锁）。
static void apply_autolock(void)
{
    app_vault_t *v = vault();
    if (v) app_vault_set_autolock(v, VV_AUTOLOCK_S);
}

// 清掉详情缓冲。任何离开详情页或整页销毁的路径都必须经过它。
static void pass_clear(void)
{
    memset(s.pass_buf, 0, sizeof(s.pass_buf));
    s.pass_visible = false;
}

static void unlock_refresh(void)
{
    if (s.unlock_count) lv_label_set_text_fmt(s.unlock_count, "已敲 %d 次", s.knock.len);
}

static void gesture_refresh(void)
{
    if (s.gesture_count) lv_label_set_text_fmt(s.gesture_count, "已敲 %d 次", s.knock.len);
    if (s.gesture_right) {
        lv_label_set_text_fmt(s.gesture_right, "%d / %d", s.knock.len, APP_VAULT_KNOCK_MAX);
    }
}

// 列表选中项：只改行样式与滚动位置，不重建整表。
static void list_select(void)
{
    int sel = app_vault_selected(vault());
    for (int i = 0; i < s.row_count; i++) ui_row_set_selected(s.rows[i], i == sel);
    if (sel >= 0 && sel < s.row_count && s.rows[sel].obj) {
        ui_scroll_into_view(s.rows[sel].obj);
    }
}

static void detail_pass_refresh(void)
{
    if (!s.d_pass.value) return;
    if (s.pass_visible) {
        ui_row_set_value(s.d_pass, s.pass_buf[0] ? s.pass_buf : "未填写");
        lv_obj_set_style_text_color(s.d_pass.value, lv_color_hex(ui_c_warn()), 0);
    } else {
        ui_row_set_value(s.d_pass, VV_PASS_MASK);
        lv_obj_set_style_text_color(s.d_pass.value, lv_color_hex(ui_c_dim()), 0);
    }
}

static void modes_select(void)
{
    if (s.mode_count <= 0) return;
    if (s.mode_sel < 0) s.mode_sel = 0;
    if (s.mode_sel >= s.mode_count) s.mode_sel = s.mode_count - 1;
    for (int i = 0; i < s.mode_count; i++) ui_row_set_selected(s.modes[i], i == s.mode_sel);
    if (s.modes[s.mode_sel].obj) ui_scroll_into_view(s.modes[s.mode_sel].obj);
}

static void set_view(vault_view_t view)
{
    // 锁定态不允许出现任何会读到条目的视图：统一折回解锁视图。
    if (view != VV_UNLOCK && (!vault() || app_vault_is_locked(vault()))) view = VV_UNLOCK;
    pass_clear();
    s.view = view;
    switch (view) {
    case VV_LIST:    build_list();    break;
    case VV_DETAIL:  build_detail();  break;
    case VV_MODES:   build_modes();   break;
    case VV_GESTURE: build_gesture(); break;
    default:         build_unlock();  break;
    }
}

// ---- 解锁视图：敲击手势 ----
// UP/DOWN 各是一种敲击，OK 提交；长按 UP 退格、长按 DOWN 清空，空着长按 DOWN 进恢复码引导。
// 界面只显示已敲次数，不回显具体序列，避免旁人从屏幕读出用户手势。
static void build_unlock(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    s.unlock_count = NULL;

    ui_header_create(c, "密码本", "加密保护", NULL, NULL);
    lv_obj_t *card = ui_card_create(c, 0, 0, VV_CW, 196, ui_c_warn());
    lv_obj_t *title = ui_label_create(card, "敲击手势解锁", ui_font_title, ui_c_text());
    lv_obj_set_pos(title, 12, 8);

    lv_obj_t *desc = ui_label_create(card,
        "本机只有三个键，用一串敲击当手势：短按 ↑ 记上敲，短按 ↓ 记下敲，"
        "短按 OK 尝试解锁。长按 ↑ 退格，长按 ↓ 清空。",
        ui_font_hint, ui_c_dim());
    lv_obj_set_width(desc, VV_CW - 24);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(desc, 12, 44);

    s.unlock_count = ui_label_create(card, "", ui_font_body, ui_c_accent());
    lv_obj_set_pos(s.unlock_count, 12, 128);

    lv_obj_t *note = ui_label_create(card, "忘了手势？清空后长按 ↓ 用恢复码解锁。",
                                     ui_font_hint, ui_c_dim());
    lv_obj_set_width(note, VV_CW - 24);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(note, 12, 164);

    unlock_refresh();
    // 长按↑退格 / 长按↓清空（空着时是恢复码入口）都是 unlock_key 里已实现的动作，
    // 原先只写在卡内说明里，提示条没提。提示条才是用户随时能看到的按键约定，补齐。
    ui_page_set_hint("↑↓ 敲击  OK 解锁  长按↑ 退格  长按↓ 清空  长按OK 返回");
}

static void try_unlock(void)
{
    if (!app_vault_knock_valid(&s.knock)) {
        ui_hint_flash("手势至少敲 4 次", 1500);
        return;
    }
    // 失败时只反馈状态文案，不透露条目数量、长度或任何内容。走 try_ 版本：连续失败会
    // 进入冷却，冷却期内连一次密钥派生都不做，暴力尝试得不到额外信息。
    app_vault_status_t st = app_vault_try_unlock_knock(vault(), &s.knock, mono_ms());
    app_vault_knock_clear(&s.knock);
    if (st == APP_VAULT_OK) {
        ui_sound_beep();
        set_view(VV_LIST);
        ui_hint_flash("已解锁", 1200);
    } else {
        unlock_refresh();
        ui_hint_flash(app_vault_status_text(st), 2000);
    }
}

static void unlock_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (!app_vault_knock_pop(&s.knock)) ui_hint_flash("还没有敲过", 1200);
        unlock_refresh();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        // 有输入时"重来"；已经空着时，这一下才是"忘了手势"的入口。
        if (s.knock.len > 0) {
            app_vault_knock_clear(&s.knock);
            unlock_refresh();
            ui_hint_flash("已清空，可重新敲击", 1400);
        } else {
            open_overlay(OV_RECOVERY_HELP, VV_UNLOCK);
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int tap = (btn == BSP_BTN_UP) ? APP_VAULT_TAP_UP : APP_VAULT_TAP_DOWN;
        if (!app_vault_knock_push(&s.knock, tap)) ui_hint_flash("最多敲 16 次", 1200);
        unlock_refresh();
        return;
    }
    if (btn == BSP_BTN_OK) try_unlock();
}

// ---- 列表视图 ----

static void build_list(void)
{
    app_vault_t *v = vault();
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    s.row_count = 0;
    memset(s.rows, 0, sizeof(s.rows));
    if (!v || app_vault_is_locked(v)) return;   // set_view 已保证不会到这里

    ui_header_create(c, "密码本", app_vault_mode_name(v->mode), NULL, NULL);
    if (v->count <= 0) {
        ui_empty_create(c, "密码本为空",
                        "请用手机连上设备热点，在配置页添加条目；本机只有三个键，不能逐字输入。");
        ui_page_set_hint("长按↓ 加密设置  长按OK 返回");
        s.built_count = 0;
        return;
    }

    lv_obj_t *list = ui_list_create(c);
    s.row_count = v->count;
    for (int i = 0; i < v->count && i < APP_VAULT_MAX; i++) {
        const app_vault_entry_t *e = app_vault_at(v, i);
        const char *label = (e && e->label[0]) ? e->label : "未命名";
        const char *acct = (e && e->account[0]) ? e->account : "无账号";
        s.rows[i] = ui_row_create(list, label, acct);
    }
    list_select();
    s.built_count = v->count;
    // 列表页长按 OK 会退出密码本（见 page_vault_key），原先提示没写，用户以为出不去。
    ui_page_set_hint("↑↓ 选择  OK 查看  长按↑ 删除  长按↓ 模式  长按OK 返回");
}

static void remove_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;
    app_vault_t *v = vault();
    int idx = app_vault_selected(v);
    if (idx < 0) return;
    if (app_vault_remove(v, idx) != APP_VAULT_OK) return;
    app_state_save_vault();   // 改数据立即落盘
    build_list();
    ui_hint_flash("已删除条目", 1200);
}

static void request_remove(void)
{
    app_vault_t *v = vault();
    const app_vault_entry_t *e = app_vault_at(v, app_vault_selected(v));
    if (!e) {
        ui_hint_flash("密码本为空", 1200);
        return;
    }
    char body[80];
    snprintf(body, sizeof(body), "删除“%s”？删除后无法恢复。",
             e->label[0] ? e->label : "未命名");
    ui_dialog_open(s.page.scr, "删除条目", body, "删除", remove_confirm, NULL);
}

static void list_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    app_vault_t *v = vault();
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        request_remove();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        s.mode_sel = 0;
        set_view(VV_MODES);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (v->count <= 0) {
        if (btn == BSP_BTN_OK) ui_hint_flash("请在配置页添加条目", 1800);
        return;
    }
    if (btn == BSP_BTN_UP) {
        app_vault_cycle(v, -1);
        list_select();
    } else if (btn == BSP_BTN_DOWN) {
        app_vault_cycle(v, 1);
        list_select();
    } else if (btn == BSP_BTN_OK) {
        set_view(VV_DETAIL);
    }
}

// ---- 详情视图 ----
// 详情只读：设备端不逐字编辑，改条目请去配网页。息屏计时由控制器负责，本页不实现，
// 但口令默认掩码显示，减少旁人瞥见的机会。
static void build_detail(void)
{
    app_vault_t *v = vault();
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    s.d_label   = (ui_row_t){0};
    s.d_account = (ui_row_t){0};
    s.d_pass    = (ui_row_t){0};

    int idx = app_vault_selected(v);
    const app_vault_entry_t *e = app_vault_at(v, idx);
    if (!e) {
        set_view(VV_LIST);
        return;
    }
    // 口令拷进本页缓冲，离开详情由 pass_clear() 抹掉，绝不比详情页活得更久。
    memset(s.pass_buf, 0, sizeof(s.pass_buf));
    strncpy(s.pass_buf, e->password, sizeof(s.pass_buf) - 1);
    s.pass_visible = false;

    lv_obj_t *hdr_right = NULL;
    ui_header_create(c, "条目详情", NULL, NULL, &hdr_right);
    if (hdr_right) lv_label_set_text_fmt(hdr_right, "%d / %d", idx + 1, v->count);

    lv_obj_t *list = ui_list_create(c);
    s.d_label = ui_row_create(list, "标题", e->label[0] ? e->label : "未命名");
    s.d_account = ui_row_create(list, "账号", e->account[0] ? e->account : "未填写");
    s.d_pass = ui_row_create(list, "口令", "");
    detail_pass_refresh();

    ui_banner_create(c, "口令默认隐藏；看完请长按 OK 返回列表。", ui_c_ok());
    ui_page_set_hint("短按OK 显示/隐藏  长按↑ 切换  长按OK 返回");
}

static void detail_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 短按 OK 与长按 UP 都用于切换显示；长按 OK 由上层统一处理为"返回列表"。
    if ((ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) ||
        (ev == BSP_BTN_LONG && btn == BSP_BTN_UP)) {
        s.pass_visible = !s.pass_visible;
        detail_pass_refresh();
    }
}

// ---- 模式视图：加密与数据 ----
// 设备端只做"开关加密"这类粗粒度动作：退回明文需要已解锁，重新开启加密需要现场设置手势。
static void build_modes(void)
{
    app_vault_t *v = vault();
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.modes, 0, sizeof(s.modes));

    bool enc = app_vault_is_encrypted(v);
    const char *const *names = enc ? VV_ENC_NAMES : VV_PLAIN_NAMES;
    const char *const *help = enc ? VV_ENC_HELP : VV_PLAIN_HELP;
    s.mode_count = enc ? VV_MODE_ROWS : 2;

    ui_header_create(c, "加密设置", enc ? "加密保护" : "明文保存", NULL, NULL);
    lv_obj_t *list = ui_list_create(c);
    for (int i = 0; i < s.mode_count; i++) {
        s.modes[i] = ui_row_create(list, names[i], help[i]);
    }
    if (s.modes[0].value) {
        lv_obj_set_style_text_color(s.modes[0].value, lv_color_hex(ui_c_accent()), 0);
    }
    modes_select();
    ui_page_set_hint("↑↓ 选择  OK 执行  长按OK 返回");
}

static void disable_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;
    app_vault_status_t st = app_vault_disable_encryption(vault());
    if (st != APP_VAULT_OK) {
        ui_hint_flash(app_vault_status_text(st), 2000);
        return;
    }
    app_state_save_vault();
    set_view(VV_LIST);
    ui_hint_flash("已退回明文保存", 1500);
}

static void request_disable(void)
{
    ui_dialog_open(s.page.scr, "退回明文保存",
                   "退回后条目以明文保存，拿到设备的人可直接看到，恢复码也会失效。",
                   "退回", disable_confirm, NULL);
}

static void regenerate_recovery(void)
{
    app_vault_status_t st = app_vault_new_recovery_code(vault());
    if (st != APP_VAULT_OK) {
        ui_hint_flash(app_vault_status_text(st), 2000);
        return;
    }
    app_state_save_vault();
    open_overlay(OV_RECOVERY_CODE, VV_MODES);
}

static void start_encryption_flow(void)
{
    app_vault_knock_clear(&s.knock);
    set_view(VV_GESTURE);
    ui_hint_flash("请敲出手势后按 OK", 1800);
}

static void modes_run(int index)
{
    app_vault_t *v = vault();
    if (!app_vault_is_encrypted(v)) {
        if (index == 0) start_encryption_flow();
        else ui_hint_flash("手机连上热点后在配置页添加条目", 2200);
        return;
    }
    switch (index) {
    case 0: request_disable(); break;
    case 1: regenerate_recovery(); break;
    case 2:
        app_vault_lock(v);
        set_view(VV_UNLOCK);
        ui_hint_flash("已上锁", 1200);
        break;
    default:
        ui_hint_flash("手机连上热点后在配置页添加条目", 2200);
        break;
    }
}

static void modes_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        modes_run(s.mode_sel);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s.mode_sel--;
        modes_select();
    } else if (btn == BSP_BTN_DOWN) {
        s.mode_sel++;
        modes_select();
    } else if (btn == BSP_BTN_OK) {
        modes_run(s.mode_sel);
    }
}

// ---- 设置手势视图 ----

static void build_gesture(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    s.gesture_count = NULL;
    s.gesture_right = NULL;

    ui_header_create(c, "设置手势", NULL, NULL, &s.gesture_right);
    lv_obj_t *card = ui_card_create(c, 0, 0, VV_CW, 166, ui_c_accent());
    lv_obj_t *desc = ui_label_create(card,
        "短按 ↑ 记上敲，短按 ↓ 记下敲，短按 OK 确认。长按 ↑ 退格，长按 ↓ 清空，"
        "长按 OK 取消。需要敲 4 到 16 次，请务必牢记。",
        ui_font_hint, ui_c_dim());
    lv_obj_set_width(desc, VV_CW - 24);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(desc, 12, 12);

    s.gesture_count = ui_label_create(card, "", ui_font_body, ui_c_accent());
    lv_obj_set_pos(s.gesture_count, 12, 120);
    gesture_refresh();
    ui_page_set_hint("↑↓ 敲击  OK 确认  长按↑ 退格  长按↓ 清空  长按OK 取消");
}

static void confirm_gesture(void)
{
    if (!app_vault_knock_valid(&s.knock)) {
        ui_hint_flash("手势要敲 4 到 16 次", 1800);
        return;
    }
    app_vault_status_t st = app_vault_enable_encryption(vault(), &s.knock);
    app_vault_knock_clear(&s.knock);
    if (st != APP_VAULT_OK) {
        gesture_refresh();
        ui_hint_flash(app_vault_status_text(st), 2200);
        return;
    }
    app_state_save_vault();   // 模式与密文都已改变，立即落盘
    ui_sound_beep();
    // 恢复码只在本次运行内可取，必须现在就展示给用户抄写。
    open_overlay(OV_RECOVERY_CODE, VV_LIST);
}

static void gesture_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (!app_vault_knock_pop(&s.knock)) ui_hint_flash("还没有敲过", 1200);
        gesture_refresh();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        app_vault_knock_clear(&s.knock);
        gesture_refresh();
        ui_hint_flash("已清空", 1000);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int tap = (btn == BSP_BTN_UP) ? APP_VAULT_TAP_UP : APP_VAULT_TAP_DOWN;
        if (!app_vault_knock_push(&s.knock, tap)) ui_hint_flash("最多敲 16 次", 1200);
        gesture_refresh();
        return;
    }
    if (btn == BSP_BTN_OK) confirm_gesture();
}

// ---- 覆盖层：恢复码展示 / 恢复码引导 ----

static void close_overlay(void)
{
    if (s.overlay) {
        lv_obj_delete(s.overlay);
        s.overlay = NULL;
    }
    s.overlay_kind = OV_NONE;
}

static void overlay_hint(lv_obj_t *ov, const char *text)
{
    lv_obj_t *lbl = ui_label_create(ov, text, ui_font_hint, ui_c_dim());
    lv_obj_set_width(lbl, UI_W - 8);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    // 与页面提示条同宽同高：允许折行，长按键说明才不会被裁掉。
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl, 4, UI_H - UI_HINT_H);
}

static void open_overlay(vault_overlay_t kind, vault_view_t back)
{
    close_overlay();
    s.overlay_kind = kind;
    s.overlay_return = back;

    lv_obj_t *ov = lv_obj_create(s.page.scr);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_size(ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(ui_c_bg()), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    s.overlay = ov;

    lv_obj_t *title = ui_label_create(ov,
        kind == OV_RECOVERY_CODE ? "恢复码" : "用恢复码解锁",
        ui_font_title, ui_c_text());
    lv_obj_set_width(title, UI_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, 8);

    if (kind == OV_RECOVERY_CODE) {
        lv_obj_t *banner = ui_banner_create(ov,
            "请立刻抄下或拍照。忘了手势又丢掉恢复码，条目将无法找回。", ui_c_warn());
        lv_obj_set_pos(banner, 10, 44);
        lv_obj_set_width(banner, 220);

        const char *code = app_vault_pending_recovery(vault());
        lv_obj_t *code_lbl = ui_label_create(ov,
            (code && code[0]) ? code : "本机当前没有可展示的恢复码",
            ui_font_body, ui_c_accent());
        lv_obj_set_width(code_lbl, 216);
        lv_label_set_long_mode(code_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(code_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(code_lbl, 12, 122);
        overlay_hint(ov, "短按 OK 已抄好并关闭");
    } else {
        lv_obj_t *body = ui_label_create(ov,
            "本机只有三个键，无法输入 31 位恢复码。\n\n"
            "请用手机连上设备热点，打开配置页，在配置页里用恢复码解锁。"
            "解锁后设备即处于解锁态，可在本页退回明文再重新设置手势。",
            ui_font_hint, ui_c_text());
        lv_obj_set_width(body, 208);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(body, 16, 56);
        overlay_hint(ov, "短按 OK 关闭");
    }
}

// 用户读完覆盖层：确认已抄写后恢复码即可从运行态清掉；随后回到进入前的视图。
static void overlay_ok(void)
{
    vault_overlay_t kind = s.overlay_kind;
    vault_view_t back = s.overlay_return;
    close_overlay();
    if (kind == OV_RECOVERY_CODE) app_vault_clear_pending_recovery(vault());
    set_view(back);
}

// ---- 页面接口 ----

// 关闭本页。调用方随后重建身份页；未进入时调用无副作用。
void page_vault_exit(void)
{
    if (!s.active && !s.page.scr) return;
    ui_dialog_close();   // 对话框是本页屏幕的子对象，先收掉避免悬空静态指针
    close_overlay();
    pass_clear();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_vault_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.page = ui_page_create(NULL);
    apply_autolock();
    // 锁定态由 set_view 统一折回解锁视图；解锁态直接进列表。
    set_view(VV_LIST);
}

void page_vault_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 覆盖层期间独占按键，任意 OK 关闭。
    if (s.overlay) {
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) overlay_ok();
        return;
    }
    // 解锁态下的任何按键都算一次活动，把自动回锁的倒计时往后推。
    {
        app_vault_t *v = vault();
        if (v && !app_vault_is_locked(v)) app_vault_touch(v, mono_ms());
    }
    // 长按 OK 返回上一级：详情/模式回列表，设置手势回模式，列表/解锁则退出本页。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        switch (s.view) {
        case VV_DETAIL:
        case VV_MODES:
            set_view(VV_LIST);
            return;
        case VV_GESTURE:
            app_vault_knock_clear(&s.knock);
            set_view(VV_MODES);
            return;
        default:
            page_vault_exit();
            return;
        }
    }

    switch (s.view) {
    case VV_UNLOCK: unlock_key(btn, ev); break;
    case VV_LIST:   list_key(btn, ev);   break;
    case VV_DETAIL: detail_key(btn, ev); break;
    case VV_MODES:  modes_key(btn, ev);  break;
    default:        gesture_key(btn, ev); break;
    }
}

void page_vault_tick(void)
{
    if (!s.active || !s.page.scr || s.overlay) return;
    app_vault_t *v = vault();
    if (!v) return;
    // 闲置到点自动回锁：先把明文界面折回解锁页，再提示，避免任何仍显示口令的瞬间。
    if (app_vault_poll_autolock(v, mono_ms())) {
        set_view(VV_UNLOCK);
        ui_hint_flash("闲置太久，已自动上锁", 2000);
        return;
    }
    if (app_vault_is_locked(v)) return;
    // 条目数可能被配网页或网络侧改动：变了就重建列表。
    if (s.view == VV_LIST && v->count != s.built_count) build_list();
}

bool page_vault_active(void)
{
    return s.active;
}
