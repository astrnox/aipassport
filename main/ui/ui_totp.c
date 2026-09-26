// main/ui/ui_totp.c —— 工具页子页：动态口令（TOTP）。
//
// 从原"身份与工具"页拆出来，单独占一屏：一屏只做一件事，用户不必再记"口令在第几页"。
// 口令由 app_totp 纯逻辑按时间算出，本页只负责显示与刷新；时间未校准时给醒目横幅提醒，
// 因为未校时算出的口令基本无效。
//
// 按键：↑↓ 切换账户  长按↑ 查看恢复码  OK 提示 长按OK 返回工具页。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_state.h"
#include "logic/app_totp.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static struct {
    bool active;
    ui_page_t page;

    lv_obj_t *hdr_right;
    lv_obj_t *acct;
    lv_obj_t *code;
    lv_obj_t *sec;
    lv_obj_t *ring;
    lv_obj_t *next;

    lv_obj_t *overlay;   // 恢复码覆盖层
    int index;
} s;

// ---------------------------------------------------------------------------
// 恢复码：从密钥做 Base32 编码（RFC 4648，无填充），再 4 字符一组。
// ---------------------------------------------------------------------------

static const char TOTP_B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static size_t base32_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t oi = 0;
    int bits = 0;
    uint32_t acc = 0;
    if (cap == 0) return 0;

    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (oi + 1 >= cap) { out[oi] = '\0'; return oi; }
            out[oi++] = TOTP_B32[(acc >> bits) & 0x1F];
        }
    }
    if (bits > 0) {
        if (oi + 1 >= cap) { out[oi] = '\0'; return oi; }
        out[oi++] = TOTP_B32[(acc << (5 - bits)) & 0x1F];
    }
    out[oi] = '\0';
    return oi;
}

static void group4(const char *src, char *out, size_t cap)
{
    size_t oi = 0;
    int n = 0;
    if (cap == 0) return;
    for (size_t i = 0; src[i] && oi + 2 < cap; i++) {
        if (n == 4) { out[oi++] = ' '; n = 0; }
        out[oi++] = src[i];
        n++;
    }
    out[oi] = '\0';
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

static void totp_show(void)
{
    int n = app_state_totp_count();
    if (n <= 0) return;
    if (s.index >= n) s.index = 0;
    if (s.index < 0) s.index = 0;

    app_totp_account_t *a = app_state_totp_at(s.index);
    if (!a) return;

    if (s.hdr_right) lv_label_set_text_fmt(s.hdr_right, "%d / %d", s.index + 1, n);
    if (s.acct) lv_label_set_text(s.acct, a->label[0] ? a->label : "未命名账户");

    uint64_t now = app_state_now_unix();
    char code[16];
    char fmt[16];
    if (app_totp_code(a, now, code, sizeof(code)) &&
        app_totp_format(code, fmt, sizeof(fmt)) >= 0) {
        lv_label_set_text(s.code, fmt);
    } else {
        lv_label_set_text(s.code, "------");
    }

    int period = (a->period > 0) ? a->period : 30;
    char next[16];
    char nfmt[16];
    if (app_totp_code(a, now + (uint64_t)period, next, sizeof(next)) &&
        app_totp_format(next, nfmt, sizeof(nfmt)) >= 0) {
        lv_label_set_text_fmt(s.next, "下一个  %s", nfmt);
    } else {
        lv_label_set_text(s.next, "下一个  ------");
    }

    int rem = app_totp_remaining(a, now);
    if (rem < 0) rem = 0;
    uint32_t col = (rem <= 5) ? ui_c_live() : ui_c_accent();
    lv_label_set_text_fmt(s.sec, "%d", rem);
    lv_obj_set_style_text_color(s.sec, lv_color_hex(col), 0);
    ui_ring_set(s.ring, rem * 1000 / period, col);
}

static void close_overlay(void)
{
    if (s.overlay) {
        lv_obj_delete(s.overlay);
        s.overlay = NULL;
    }
}

static void open_recovery_overlay(void)
{
    int n = app_state_totp_count();
    if (n <= 0) return;
    if (s.index >= n) s.index = 0;
    app_totp_account_t *a = app_state_totp_at(s.index);
    if (!a) return;

    char b32[128];
    char grouped[192];
    size_t blen = base32_encode(a->secret, a->secret_len, b32, sizeof(b32));
    if (blen == 0) {
        ui_hint_flash("该账户没有可导出的密钥", 1800);
        return;
    }
    group4(b32, grouped, sizeof(grouped));

    close_overlay();
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

    lv_obj_t *title = ui_label_create(ov, "恢复码", ui_font_title, ui_c_text());
    lv_obj_set_width(title, UI_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, 8);

    lv_obj_t *acct = ui_label_create(ov, a->label[0] ? a->label : "未命名账户",
                                     ui_font_body, ui_c_dim());
    lv_obj_set_width(acct, UI_W);
    lv_obj_set_style_text_align(acct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(acct, 0, 48);

    lv_obj_t *banner = ui_banner_create(ov, "恢复码可离线重建全部口令，请勿让他人看到",
                                        ui_c_warn());
    lv_obj_set_pos(banner, 10, 84);
    lv_obj_set_width(banner, 220);

    // 密钥最长 64 字节，Base32 分组后可达 128 字符，用提示字号并允许换行。
    lv_obj_t *code = ui_label_create(ov, grouped, ui_font_hint, ui_c_text());
    lv_obj_set_pos(code, 10, 132);
    lv_obj_set_width(code, 220);
    lv_label_set_long_mode(code, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = ui_label_create(ov, "长按 OK 关闭", ui_font_hint, ui_c_dim());
    lv_obj_set_width(hint, UI_W - 8);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 4, UI_H - UI_HINT_H);
}

static void recover_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;
    open_recovery_overlay();
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_totp_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.page = ui_page_create(NULL);

    int n = app_state_totp_count();
    if (n <= 0) {
        ui_empty_create(s.page.content, "还没有动态口令",
                        "请在手机配置页粘贴 otpauth 链接导入，或到设置中开启配网后导入密钥");
        ui_page_set_hint("长按OK 返回工具页");
        return;
    }

    ui_header_create(s.page.content, "动态口令", NULL, NULL, &s.hdr_right);

    // 时间同步状态：未同步给醒目警示但不禁用，已同步给一行辅助信息。
    if (!app_state_settings()->time_synced) {
        ui_banner_create(s.page.content, "时间未同步，口令可能无效，请在设置中校准",
                         ui_c_warn());
    } else {
        app_datetime_t now = app_state_now();
        lv_obj_t *sync = ui_label_create(s.page.content, NULL, ui_font_hint, ui_c_dim());
        lv_label_set_text_fmt(sync, "时间已同步 · %02d:%02d", now.hour, now.minute);
    }

    s.acct = ui_label_create(s.page.content, NULL, ui_font_body, ui_c_text());
    lv_obj_set_width(s.acct, LV_PCT(100));

    s.code = ui_label_create(s.page.content, NULL, ui_font_display_s, ui_c_accent());
    lv_obj_set_width(s.code, LV_PCT(100));
    lv_obj_set_style_text_align(s.code, LV_TEXT_ALIGN_CENTER, 0);

    // 环形进度内叠剩余秒数，右侧给出"剩余时间"与下一个口令（均按字体行高排布）。
    lv_obj_t *row = lv_obj_create(s.page.content);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    s.ring = ui_ring_create(row, 64, 0, 0, ui_c_accent());
    s.sec = ui_label_create(row, NULL, ui_font_display_s, ui_c_accent());
    lv_obj_align_to(s.sec, s.ring, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *remain = ui_label_create(row, "剩余时间", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(remain, 74, 0);

    s.next = ui_label_create(row, NULL, ui_font_body, ui_c_dim());
    lv_obj_set_width(s.next, 150);
    lv_obj_set_pos(s.next, 74, 26);

    totp_show();
    ui_page_set_hint("↑↓ 切换账户  长按↑ 恢复码   长按OK 返回");
}

void page_totp_exit(void)
{
    if (!s.active && !s.page.scr) return;
    ui_dialog_close();
    close_overlay();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_totp_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 恢复码覆盖层独占按键：任意 OK 关闭。
    if (s.overlay) {
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            close_overlay();
        }
        return;
    }

    int n = app_state_totp_count();

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        page_totp_exit();
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (n <= 0) return;
        ui_dialog_open(s.page.scr, "查看恢复码",
                       "恢复码可离线重建全部口令，请勿让他人看到。",
                       "查看", recover_confirm, NULL);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (n <= 0) {
        if (btn == BSP_BTN_OK) ui_hint_flash("请在设置中开启配网导入", 1500);
        return;
    }
    if (btn == BSP_BTN_UP) {
        s.index = (s.index + n - 1) % n;
        totp_show();
    } else if (btn == BSP_BTN_DOWN) {
        s.index = (s.index + 1) % n;
        totp_show();
    } else if (btn == BSP_BTN_OK) {
        ui_hint_flash("口令每 30 秒自动刷新", 1500);
    }
}

void page_totp_tick(void)
{
    if (!s.active || !s.page.scr || s.overlay) return;
    if (app_state_totp_count() > 0) totp_show();
}

bool page_totp_active(void)
{
    return s.active;
}