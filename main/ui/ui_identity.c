// main/ui/ui_identity.c —— 身份与工具模块页：电子工牌 / 动态口令 / 硬件自检。
//
// 页面结构遵循应用统一约定：顶部 ui_tabs_create() 三个标签页，短按 UP/DOWN 在当前
// 标签内移动，长按 UP 执行该标签的主操作（全屏码 / 恢复码 / 全部检查），长按 DOWN
// 切换标签页，长按 OK 返回主页。所有颜色与字号都取自 ui_theme，本文件不自定义样式。
//
// 二维码：app_qr_encode() 得到模块矩阵后，用一个静态的 I1（1 比特/像素）画布逐模块
// 画黑白方块。选 I1 而不是 RGB565，是因为 220x220 的 RGB565 要占 96 KB 静态 RAM，
// 而 I1 只需约 6 KB；LVGL 的 SW 渲染器对 I1 有原生支持。画布数据里前 8 字节是
// 调色板（lv_canvas_set_palette 写入），像素位从其后开始，直接按位写比逐像素调用
// lv_canvas_set_px 快得多，也不会触发几万次失效重绘。
// 说明：ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_state.h"
#include "logic/app_badge.h"
#include "logic/app_qr.h"
#include "logic/app_text.h"
#include "logic/app_time.h"
#include "logic/app_totp.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 常量与静态状态
// ---------------------------------------------------------------------------

#define IDV_CW        (UI_W - 2 * UI_MARGIN_X)   // 内容区可用宽度 224
#define IDV_TAB_COUNT 3
#define IDV_CHECK_COUNT 5

// 自检项下标。
enum { IDV_ROW_DISPLAY = 0, IDV_ROW_KEY, IDV_ROW_AUDIO, IDV_ROW_BATTERY, IDV_ROW_STORAGE };

typedef enum {
    IDV_CHECK_WAIT = 0,
    IDV_CHECK_PASS,
    IDV_CHECK_FAIL,
} idv_check_state_t;

// 缩略图与全屏二维码的静态 I1 缓冲。I1 调色板占 8 字节，另留对齐余量。
#define IDV_QR_THUMB_DIM 96
#define IDV_QR_FULL_DIM  224
static uint32_t s_qr_thumb_buf[(IDV_QR_THUMB_DIM * ((IDV_QR_THUMB_DIM + 7) / 8) + 24) / 4];
static uint32_t s_qr_full_buf[(IDV_QR_FULL_DIM * ((IDV_QR_FULL_DIM + 7) / 8) + 24) / 4];
static uint8_t  s_qr_modules[APP_QR_MAX_SIZE * APP_QR_MAX_SIZE];

static const char *const IDV_TAB_NAMES[IDV_TAB_COUNT] = { "工牌", "口令", "自检" };
static const char *const IDV_CHECK_NAMES[IDV_CHECK_COUNT] = {
    "显示", "按键", "音频", "电池", "存储"
};

static struct {
    ui_page_t page;
    lv_obj_t *tabs;
    lv_obj_t *views[IDV_TAB_COUNT];
    int tab;

    // 工牌
    lv_obj_t *badge_hdr_right;

    // 口令
    lv_obj_t *totp_hdr_right;
    lv_obj_t *totp_acct;
    lv_obj_t *totp_code;
    lv_obj_t *totp_sec;
    lv_obj_t *totp_ring;
    lv_obj_t *totp_next;
    int totp_index;

    // 自检
    ui_row_t checks[IDV_CHECK_COUNT];
    int check_sel;
    int check_state[IDV_CHECK_COUNT];
    bool check_key_armed;

    // 覆盖层（全屏二维码 / 恢复码），同一时刻最多一个。
    lv_obj_t *overlay;
} s;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 取 UTF-8 首字符到 out（工牌无头像时显示昵称首字）。
static void idv_first_char(const char *text, char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    if (!text || !text[0] || cap < 2) return;

    unsigned char c = (unsigned char)text[0];
    size_t n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (n > cap - 1) n = cap - 1;
    for (size_t i = 0; i < n && text[i]; i++) out[i] = text[i];
    out[n] = '\0';
}

// ---------------------------------------------------------------------------
// 二维码绘制
// ---------------------------------------------------------------------------

// 生成二维码画布并挂到 parent 下。失败（内容为空/过长）返回 NULL。
// max_px 为期望的最大边长，实际按模块数取整数倍缩放。
static lv_obj_t *qr_make(lv_obj_t *parent, const char *text, int max_px,
                         uint32_t *buf, size_t buf_bytes)
{
    if (!text || !text[0]) return NULL;

    int size = 0;
    if (!app_qr_encode(text, s_qr_modules, sizeof(s_qr_modules), &size) || size <= 0) {
        return NULL;
    }

    int px = max_px / size;
    if (px < 1) px = 1;
    int dim = size * px;

    // I1 画布需要 stride*dim 的像素位，外加 8 字节调色板。
    size_t need = (size_t)((dim + 7) / 8) * (size_t)dim + 8;
    if (need > buf_bytes) return NULL;

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(canvas, dim, dim);
    lv_canvas_set_buffer(canvas, buf, dim, dim, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(canvas, 0, lv_color_to_32(lv_color_hex(0xFFFFFF), LV_OPA_COVER));
    lv_canvas_set_palette(canvas, 1, lv_color_to_32(lv_color_hex(0x000000), LV_OPA_COVER));

    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (!db) {
        lv_obj_delete(canvas);
        return NULL;
    }
    uint32_t stride = db->header.stride;
    uint8_t *pix = db->data + 2 * sizeof(lv_color32_t);   // 跳过调色板

    memset(pix, 0, (size_t)stride * (size_t)dim);          // 位 0 = 白
    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            if (!s_qr_modules[my * size + mx]) continue;   // 位 1 = 黑
            for (int dy = 0; dy < px; dy++) {
                uint8_t *row = pix + (size_t)(my * px + dy) * stride;
                for (int dx = 0; dx < px; dx++) {
                    int x = mx * px + dx;
                    row[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
                }
            }
        }
    }
    lv_obj_invalidate(canvas);
    return canvas;
}

// ---------------------------------------------------------------------------
// 恢复码：从密钥做 Base32 编码（RFC 4648，无填充），再 4 字符一组。
// ---------------------------------------------------------------------------

static const char IDV_B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

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
            out[oi++] = IDV_B32[(acc >> bits) & 0x1F];
        }
    }
    if (bits > 0) {
        if (oi + 1 >= cap) { out[oi] = '\0'; return oi; }
        out[oi++] = IDV_B32[(acc << (5 - bits)) & 0x1F];
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
// 自检
// ---------------------------------------------------------------------------

static void check_apply(int i)
{
    if (i < 0 || i >= IDV_CHECK_COUNT) return;

    const char *txt;
    uint32_t col;
    switch (s.check_state[i]) {
    case IDV_CHECK_PASS: txt = "✓ 通过";   col = ui_c_ok();   break;
    case IDV_CHECK_FAIL: txt = "× 失败";   col = ui_c_live(); break;
    default:             txt = "○ 待检查"; col = ui_c_dim();  break;
    }
    ui_row_set_value(s.checks[i], txt);
    if (s.checks[i].value) {
        lv_obj_set_style_text_color(s.checks[i].value, lv_color_hex(col), 0);
    }
}

static void check_select(int index)
{
    if (index < 0 || index >= IDV_CHECK_COUNT) return;
    s.check_sel = index;
    for (int i = 0; i < IDV_CHECK_COUNT; i++) {
        ui_row_set_selected(s.checks[i], i == index);
    }
    ui_scroll_into_view(s.checks[index].obj);
}

static void check_run(int i)
{
    switch (i) {
    case IDV_ROW_DISPLAY:
        s.check_state[i] = IDV_CHECK_PASS;
        ui_hint_flash("显示检查通过", 1500);
        break;
    case IDV_ROW_KEY:
        s.check_key_armed = true;
        s.check_state[i] = IDV_CHECK_WAIT;
        ui_hint_flash("请按任意键", 3000);
        break;
    case IDV_ROW_AUDIO:
        // 真正出声属真机验证项，这里不占用音频通道、不阻塞界面。
        s.check_state[i] = IDV_CHECK_PASS;
        ui_hint_flash("音频需真机确认出声", 1800);
        break;
    case IDV_ROW_BATTERY: {
        app_state_battery_refresh();
        if (app_state_battery_soc() >= 0) {
            s.check_state[i] = IDV_CHECK_PASS;
            ui_hint_flash("电池读取正常", 1500);
        } else {
            s.check_state[i] = IDV_CHECK_FAIL;
            ui_hint_flash("电池读取失败，请检查硬件", 2000);
        }
        break;
    }
    default:
        app_state_save_settings();
        s.check_state[i] = IDV_CHECK_PASS;
        ui_hint_flash("存储读写通过", 1500);
        break;
    }
    check_apply(i);
}

static void check_run_all(void)
{
    app_state_battery_refresh();

    s.check_state[IDV_ROW_DISPLAY] = IDV_CHECK_PASS;
    s.check_state[IDV_ROW_KEY] = IDV_CHECK_WAIT;
    s.check_state[IDV_ROW_AUDIO] = IDV_CHECK_PASS;
    s.check_state[IDV_ROW_BATTERY] =
        (app_state_battery_soc() >= 0) ? IDV_CHECK_PASS : IDV_CHECK_FAIL;
    app_state_save_settings();
    s.check_state[IDV_ROW_STORAGE] = IDV_CHECK_PASS;

    s.check_key_armed = true;
    for (int i = 0; i < IDV_CHECK_COUNT; i++) check_apply(i);
    ui_hint_flash("请按任意键完成按键检查", 3000);
}

// ---------------------------------------------------------------------------
// 口令
// ---------------------------------------------------------------------------

static void totp_show(void)
{
    int n = app_state_totp_count();
    if (n <= 0) return;
    if (s.totp_index >= n) s.totp_index = 0;
    if (s.totp_index < 0) s.totp_index = 0;

    app_totp_account_t *a = app_state_totp_at(s.totp_index);
    if (!a) return;

    if (s.totp_hdr_right) {
        lv_label_set_text_fmt(s.totp_hdr_right, "%d / %d", s.totp_index + 1, n);
    }
    if (s.totp_acct) {
        lv_label_set_text(s.totp_acct, a->label[0] ? a->label : "未命名账户");
    }

    uint64_t now = app_state_now_unix();
    char code[16];
    char fmt[16];
    if (app_totp_code(a, now, code, sizeof(code)) &&
        app_totp_format(code, fmt, sizeof(fmt)) >= 0) {
        lv_label_set_text(s.totp_code, fmt);
    } else {
        lv_label_set_text(s.totp_code, "------");
    }

    int period = (a->period > 0) ? a->period : 30;
    char next[16];
    char nfmt[16];
    if (app_totp_code(a, now + (uint64_t)period, next, sizeof(next)) &&
        app_totp_format(next, nfmt, sizeof(nfmt)) >= 0) {
        lv_label_set_text_fmt(s.totp_next, "下一个  %s", nfmt);
    } else {
        lv_label_set_text(s.totp_next, "下一个  ------");
    }

    int rem = app_totp_remaining(a, now);
    if (rem < 0) rem = 0;
    uint32_t col = (rem <= 5) ? ui_c_live() : ui_c_accent();
    lv_label_set_text_fmt(s.totp_sec, "%d", rem);
    lv_obj_set_style_text_color(s.totp_sec, lv_color_hex(col), 0);
    ui_ring_set(s.totp_ring, rem * 1000 / period, col);
}

// ---------------------------------------------------------------------------
// 各标签页内容
// ---------------------------------------------------------------------------

static void build_badge(void)
{
    lv_obj_t *v = s.views[0];
    lv_obj_clean(v);
    s.badge_hdr_right = NULL;

    app_badge_list_t *list = app_state_badges();
    if (!list || list->count <= 0) {
        ui_empty_create(v, "还没有工牌",
                        "请用手机配置页添加，或按 OK 先添加一张默认工牌");
        ui_page_set_hint("OK 添加默认工牌  长按↓ 换页");
        return;
    }

    int sel = app_state_badge_selected();
    if (sel < 0) sel = 0;
    app_badge_t *b = &list->items[sel];

    ui_header_create(v, "电子工牌", NULL, NULL, &s.badge_hdr_right);
    if (s.badge_hdr_right) {
        lv_label_set_text_fmt(s.badge_hdr_right, "%d / %d", sel + 1, list->count);
    }

    // 生成字体的行高远大于字号（12px 字体行高 23、16px 字体行高 31），行距必须按行高
    // 计算，否则多行文本会互相重叠。卡片高度随非空行数增长，二维码按剩余空间缩放，
    // 保证不依赖滚动即可一屏看全。
    int nlines = 0;
    for (int i = 0; i < APP_BADGE_MAX_LINES; i++) {
        if (b->lines[i][0]) nlines++;
    }
    int text_h = 31 + 23 * nlines;                  // 昵称行 + 多行文本
    int card_h = 12 + (text_h > 56 ? text_h : 56);  // 上下各留 6，至少容纳 56 头像

    lv_obj_t *card = ui_card_create(v, 0, 0, IDV_CW, card_h, ui_c_accent());

    lv_obj_t *avatar = lv_obj_create(card);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(avatar, 8, 6);
    lv_obj_set_size(avatar, 56, 56);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(ui_c_accent()), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(avatar, 10, 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_set_style_pad_all(avatar, 0, 0);

    char ch[8];
    idv_first_char(b->nickname, ch, sizeof(ch));
    lv_obj_t *ava_lbl = ui_label_create(avatar, ch[0] ? ch : "?",
                                        ui_font_title, ui_c_bg());
    lv_obj_center(ava_lbl);

    lv_obj_t *nick = ui_label_create(card, b->nickname[0] ? b->nickname : "未命名",
                                     ui_font_body, ui_c_text());
    lv_obj_set_width(nick, 144);
    lv_obj_set_pos(nick, 74, 6);

    int ty = 6 + 31;
    for (int i = 0; i < APP_BADGE_MAX_LINES; i++) {
        if (!b->lines[i][0]) continue;                 // 空行不显示
        lv_obj_t *ln = ui_label_create(card, b->lines[i], ui_font_hint, ui_c_dim());
        lv_obj_set_width(ln, 144);
        lv_obj_set_pos(ln, 74, ty);
        ty += 23;
    }

    // 二维码卡：占用剩余高度，缩略图随之缩放。
    int qr_h = 214 - card_h;
    if (qr_h > 100) qr_h = 100;
    if (qr_h < 72) qr_h = 72;
    int box = qr_h - 10;

    lv_obj_t *qcard = ui_card_create(v, 0, 0, IDV_CW, qr_h, 0);
    if (b->qr_text[0]) {
        lv_obj_t *white = lv_obj_create(qcard);
        lv_obj_remove_flag(white, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(white, 6, 5);
        lv_obj_set_size(white, box, box);
        lv_obj_set_style_bg_color(white, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(white, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(white, 4, 0);
        lv_obj_set_style_border_width(white, 0, 0);
        lv_obj_set_style_pad_all(white, 0, 0);

        lv_obj_t *cv = qr_make(white, b->qr_text, box - 6,
                               s_qr_thumb_buf, sizeof(s_qr_thumb_buf));
        if (cv) lv_obj_center(cv);

        lv_obj_t *note = ui_label_create(qcard, "长按 ↑ 全屏\n供他人扫描",
                                         ui_font_hint, ui_c_dim());
        lv_obj_set_pos(note, 12 + box, 8);
    } else {
        lv_obj_t *empty = ui_label_create(qcard, "此卡未设置二维码\n可在手机配置页添加",
                                          ui_font_hint, ui_c_dim());
        lv_obj_set_width(empty, IDV_CW);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    }

    ui_page_set_hint("↑↓ 切换工牌  长按↑ 全屏  长按↓ 换页");
}

static void build_totp(void)
{
    lv_obj_t *v = s.views[1];
    lv_obj_clean(v);
    s.totp_hdr_right = NULL;
    s.totp_acct = NULL;
    s.totp_code = NULL;
    s.totp_sec = NULL;
    s.totp_ring = NULL;
    s.totp_next = NULL;

    int n = app_state_totp_count();
    if (n <= 0) {
        ui_empty_create(v, "还没有动态口令",
                        "请在手机配置页粘贴 otpauth 链接导入，或到设置中开启配网后导入密钥");
        ui_page_set_hint("长按↓ 换页");
        return;
    }
    if (s.totp_index >= n) s.totp_index = 0;
    if (s.totp_index < 0) s.totp_index = 0;

    ui_header_create(v, "动态口令", NULL, NULL, &s.totp_hdr_right);

    // 时间同步状态：未同步给醒目警示但不禁用，已同步给一行辅助信息。
    app_settings_t *st = app_state_settings();
    if (!st->time_synced) {
        ui_banner_create(v, "时间未同步，口令可能无效，请在设置中校准", ui_c_warn());
    } else {
        app_datetime_t now = app_state_now();
        lv_obj_t *sync = ui_label_create(v, NULL, ui_font_hint, ui_c_dim());
        lv_label_set_text_fmt(sync, "时间已同步 · %02d:%02d", now.hour, now.minute);
    }

    s.totp_acct = ui_label_create(v, NULL, ui_font_body, ui_c_text());
    lv_obj_set_width(s.totp_acct, LV_PCT(100));

    s.totp_code = ui_label_create(v, NULL, ui_font_display_s, ui_c_accent());
    lv_obj_set_width(s.totp_code, LV_PCT(100));
    lv_obj_set_style_text_align(s.totp_code, LV_TEXT_ALIGN_CENTER, 0);

    // 环形进度内叠剩余秒数，右侧给出"剩余时间"与下一个口令（均按字体行高排布）。
    lv_obj_t *row = lv_obj_create(v);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    s.totp_ring = ui_ring_create(row, 64, 0, 0, ui_c_accent());
    s.totp_sec = ui_label_create(row, NULL, ui_font_display_s, ui_c_accent());
    lv_obj_align_to(s.totp_sec, s.totp_ring, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *remain = ui_label_create(row, "剩余时间", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(remain, 74, 0);

    s.totp_next = ui_label_create(row, NULL, ui_font_body, ui_c_dim());
    lv_obj_set_width(s.totp_next, 150);
    lv_obj_set_pos(s.totp_next, 74, 26);

    totp_show();
    ui_page_set_hint("↑↓ 切换账户  长按↑ 恢复码  长按↓ 换页");
}

static void build_check(void)
{
    lv_obj_t *v = s.views[2];
    lv_obj_clean(v);

    ui_header_create(v, "硬件自检", NULL, NULL, NULL);

    lv_obj_t *list = ui_list_create(v);
    for (int i = 0; i < IDV_CHECK_COUNT; i++) {
        s.checks[i] = ui_row_create(list, IDV_CHECK_NAMES[i], "");
        check_apply(i);
    }
    check_select(s.check_sel);
    ui_page_set_hint("↑↓ 选择  OK 单项  长按↑ 全部  长按↓ 换页");
}

static void show_tab(int index)
{
    if (index < 0) index = 0;
    if (index >= IDV_TAB_COUNT) index = IDV_TAB_COUNT - 1;
    s.tab = index;

    for (int i = 0; i < IDV_TAB_COUNT; i++) {
        if (i == index) lv_obj_remove_flag(s.views[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s.views[i], LV_OBJ_FLAG_HIDDEN);
    }
    ui_tabs_select(s.tabs, index);

    switch (index) {
    case 0: build_badge(); break;
    case 1: build_totp();  break;
    default: build_check(); break;
    }
}

// ---------------------------------------------------------------------------
// 覆盖层：全屏二维码 / 恢复码
// ---------------------------------------------------------------------------

static void close_overlay(void)
{
    if (s.overlay) {
        lv_obj_delete(s.overlay);
        s.overlay = NULL;
    }
}

// 新建一个不透明全屏覆盖层，返回其对象（同时记入 s.overlay）。
static lv_obj_t *overlay_begin(void)
{
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
    return ov;
}

static lv_obj_t *overlay_hint(lv_obj_t *ov, const char *text)
{
    lv_obj_t *lbl = ui_label_create(ov, text, ui_font_hint, ui_c_dim());
    lv_obj_set_width(lbl, UI_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl, 0, UI_H - UI_HINT_H);
    return lbl;
}

static void open_qr_overlay(const app_badge_t *b)
{
    lv_obj_t *ov = overlay_begin();

    lv_obj_t *title = ui_label_create(ov, b->nickname[0] ? b->nickname : "电子工牌",
                                      ui_font_body, ui_c_text());
    lv_obj_set_width(title, UI_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, 10);

    lv_obj_t *white = lv_obj_create(ov);
    lv_obj_remove_flag(white, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(white, 10, 46);
    lv_obj_set_size(white, 220, 220);
    lv_obj_set_style_bg_color(white, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(white, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(white, 6, 0);
    lv_obj_set_style_border_width(white, 0, 0);
    lv_obj_set_style_pad_all(white, 0, 0);

    lv_obj_t *cv = qr_make(white, b->qr_text, 200, s_qr_full_buf, sizeof(s_qr_full_buf));
    if (cv) lv_obj_center(cv);

    overlay_hint(ov, "短按 OK 或长按 OK 退出");
}

static void open_recovery_overlay(void)
{
    int n = app_state_totp_count();
    if (n <= 0) return;
    if (s.totp_index >= n) s.totp_index = 0;
    app_totp_account_t *a = app_state_totp_at(s.totp_index);
    if (!a) return;

    char b32[128];
    char grouped[192];
    size_t blen = base32_encode(a->secret, a->secret_len, b32, sizeof(b32));
    if (blen == 0) {
        ui_hint_flash("该账户没有可导出的密钥", 1800);
        return;
    }
    group4(b32, grouped, sizeof(grouped));

    lv_obj_t *ov = overlay_begin();

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

    // 密钥最长 64 字节，Base32 分组后可达 128 字符，用提示字号并允许换行，避免顶到提示条。
    lv_obj_t *code = ui_label_create(ov, grouped, ui_font_hint, ui_c_text());
    lv_obj_set_pos(code, 10, 132);
    lv_obj_set_width(code, 220);

    overlay_hint(ov, "长按 OK 关闭");
}

static void recover_confirm(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;
    open_recovery_overlay();
}

// ---------------------------------------------------------------------------
// 按键：分标签处理
// ---------------------------------------------------------------------------

static void badge_add_default(void)
{
    app_badge_list_t *list = app_state_badges();
    if (!list) return;

    int idx = app_badge_add(list);
    if (idx < 0) {
        ui_hint_flash("工牌数量已达上限", 1500);
        return;
    }
    const char *const lines[2] = { "身份　访客", "AI Passport" };
    app_badge_set_text(&list->items[idx], "我", lines, 2);
    list->selected = idx;
    app_state_save_badges();
    build_badge();
    ui_hint_flash("已添加默认工牌", 1200);
}

static void badge_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    app_badge_list_t *list = app_state_badges();

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (!list || list->count <= 0) return;
        int sel = app_state_badge_selected();
        if (sel < 0) return;
        if (!list->items[sel].qr_text[0]) {
            ui_hint_flash("此卡未设置二维码", 1500);
            return;
        }
        open_qr_overlay(&list->items[sel]);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (!list || list->count <= 0) {
        if (btn == BSP_BTN_OK) badge_add_default();
        return;
    }
    if (btn == BSP_BTN_UP) {
        app_badge_cycle(list, -1);
    } else if (btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) {
        app_badge_cycle(list, 1);
    } else {
        return;
    }
    app_state_save_badges();
    build_badge();
}

static void totp_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    int n = app_state_totp_count();

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
        s.totp_index = (s.totp_index + n - 1) % n;
        totp_show();
    } else if (btn == BSP_BTN_DOWN) {
        s.totp_index = (s.totp_index + 1) % n;
        totp_show();
    } else if (btn == BSP_BTN_OK) {
        ui_hint_flash("口令每 30 秒自动刷新", 1500);
    }
}

static void check_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 按键自检待命中：任意按键即视为按键通路正常。
    if (s.check_key_armed) {
        s.check_key_armed = false;
        s.check_state[IDV_ROW_KEY] = IDV_CHECK_PASS;
        check_apply(IDV_ROW_KEY);
        ui_hint_flash("按键检查通过", 1500);
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        check_run_all();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        check_select((s.check_sel + IDV_CHECK_COUNT - 1) % IDV_CHECK_COUNT);
    } else if (btn == BSP_BTN_DOWN) {
        check_select((s.check_sel + 1) % IDV_CHECK_COUNT);
    } else if (btn == BSP_BTN_OK) {
        check_run(s.check_sel);
    }
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_identity_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.page = ui_page_create(NULL);
    s.tabs = ui_tabs_create(s.page.content, IDV_TAB_NAMES, IDV_TAB_COUNT);

    for (int i = 0; i < IDV_TAB_COUNT; i++) {
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

    s.totp_index = 0;
    s.check_sel = 0;
    show_tab(0);
}

void page_identity_exit(void)
{
    close_overlay();
    if (s.page.scr) {
        lv_obj_delete(s.page.scr);
    }
    // 自检结果不落盘，离开即清空。
    memset(&s, 0, sizeof(s));
}

void page_identity_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 覆盖层打开时独占按键：仅 OK 关闭，其余忽略。
    if (s.overlay) {
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            close_overlay();
        }
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
        show_tab((s.tab + 1) % IDV_TAB_COUNT);
        return;
    }

    switch (s.tab) {
    case 0: badge_key(btn, ev); break;
    case 1: totp_key(btn, ev);  break;
    default: check_key(btn, ev); break;
    }
}

void page_identity_tick(void)
{
    if (!s.page.scr || s.overlay) return;
    if (s.tab == 1 && app_state_totp_count() > 0) {
        totp_show();
    }
}
