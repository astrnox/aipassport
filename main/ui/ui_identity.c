// main/ui/ui_identity.c —— 身份模块页：只放电子工牌。
//
// 原本工牌、动态口令、密码本挤在一个"身份与工具"页里，用户要按长按↓ 反复换页才能找到
// 某个功能。现已按用户要求拆分：身份页只留工牌（数字名片 + 二维码），动态口令与密码本
// 移入 ui_tools.c 的工具列表。
//
// 按键（底部提示条如实写出）：
//   ↑↓ 切换工牌   OK 选码 / 切到下一张   长按↑ 全屏二维码   长按OK 返回主页
//
// 工牌动图：手机端上传的 RGB565 帧序列存在 assets 分区，这里用 app_assets_map() 零
// 拷贝映射，再交给 LVGL 9 的 lv_animimg 播放。帧指针指向映射区，映射必须与 animimg
// 同生命周期：只有先删掉 animimg，才允许解映射。
//
// 二维码：app_qr_encode() 得到模块矩阵后，用一个静态的 I1（1 比特/像素）画布逐模块
// 画黑白方块。选 I1 而不是 RGB565，是因为 220x220 的 RGB565 要占 96 KB 静态 RAM，
// 而 I1 只需约 6 KB；LVGL 的 SW 渲染器对 I1 有原生支持。
// 说明：ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_assets.h"
#include "app_state.h"
#include "logic/app_anim.h"
#include "logic/app_badge.h"
#include "logic/app_qr.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 常量与静态状态
// ---------------------------------------------------------------------------

#define IDV_CW        (UI_W - 2 * UI_MARGIN_X)   // 内容区可用宽度 224

// 缩略图与全屏二维码的静态 I1 缓冲。I1 调色板占 8 字节，另留对齐余量。
#define IDV_QR_THUMB_DIM 96
#define IDV_QR_FULL_DIM  224
static uint32_t s_qr_thumb_buf[(IDV_QR_THUMB_DIM * ((IDV_QR_THUMB_DIM + 7) / 8) + 24) / 4];
static uint32_t s_qr_full_buf[(IDV_QR_FULL_DIM * ((IDV_QR_FULL_DIM + 7) / 8) + 24) / 4];
static uint8_t  s_qr_modules[APP_QR_MAX_SIZE * APP_QR_MAX_SIZE];

static struct {
    ui_page_t page;

    lv_obj_t *hdr_right;
    int       qr_sel;            // 当前工牌里选中的二维码下标

    // 工牌动图。anim_dsc/anim_src 必须与 animimg 同生命周期：LVGL 只保存数组指针而
    // 不复制，帧指针又指向映射区，任何一个先失效都会让下一帧访问非法地址。
    lv_obj_t *anim;
    lv_image_dsc_t anim_dsc[APP_ANIM_MAX_FRAMES];
    const void *anim_src[APP_ANIM_MAX_FRAMES];
    app_anim_header_t anim_header;
    esp_partition_mmap_handle_t anim_handle;
    bool anim_mapped;

    lv_obj_t *overlay;           // 全屏二维码覆盖层
    int qr_overlay_index;
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
// 工牌动图：assets 分区零拷贝映射 + lv_animimg 播放
// ---------------------------------------------------------------------------

// 释放动图：先删 animimg，再解映射。顺序不能反——对象存活期间 lv_animimg 持有的帧
// 指针还指向映射区，提前解映射会让动画的下一帧读到非法地址。
static void idv_anim_release(void)
{
    if (s.anim) {
        lv_obj_delete(s.anim);
        s.anim = NULL;
    }
    if (s.anim_mapped) {
        app_assets_unmap(s.anim_handle);
        s.anim_mapped = false;
    }
}

// 为工牌动图建立帧描述符并把帧序列挂到 box 上。返回 true 表示已创建 animimg；
// 槽位为空、分区不可用或映射失败时返回 false，由调用方退回首字占位。
static bool idv_anim_mount(lv_obj_t *box, const app_badge_t *b)
{
    if (!b || b->anim_slot < 0 || !app_assets_ready()) return false;
    if (!app_assets_slot_present(b->anim_slot)) return false;
    if (app_assets_slot_header(b->anim_slot, &s.anim_header) != ESP_OK) return false;

    const uint8_t *frames = NULL;
    esp_partition_mmap_handle_t handle = 0;
    if (app_assets_map(b->anim_slot, &s.anim_header, &frames, &handle) != ESP_OK) {
        return false;
    }
    s.anim_mapped = true;
    s.anim_handle = handle;

    int count = s.anim_header.frame_count;
    if (count < 1) { idv_anim_release(); return false; }
    if (count > APP_ANIM_MAX_FRAMES) count = APP_ANIM_MAX_FRAMES;

    uint32_t frame_bytes = app_anim_frame_bytes(s.anim_header.width,
                                                s.anim_header.height);
    if (frame_bytes == 0) { idv_anim_release(); return false; }

    for (int i = 0; i < count; i++) {
        const uint8_t *px = app_assets_frame(&s.anim_header, i);
        if (!px) { idv_anim_release(); return false; }

        // 手机端按 RGB565 小端写入，这里只需把几何与数据长度如实填进描述符；
        // magic 决定 lv_image_src_get_type() 把它认成变量图源而不是文件路径。
        s.anim_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s.anim_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s.anim_dsc[i].header.flags = 0;
        s.anim_dsc[i].header.w = s.anim_header.width;
        s.anim_dsc[i].header.h = s.anim_header.height;
        s.anim_dsc[i].header.stride = (uint16_t)(s.anim_header.width * 2);
        s.anim_dsc[i].data_size = frame_bytes;
        s.anim_dsc[i].data = px;
        s.anim_dsc[i].reserved = NULL;
        s.anim_dsc[i].reserved_2 = NULL;
        s.anim_src[i] = &s.anim_dsc[i];
    }

    lv_obj_t *img = lv_animimg_create(box);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    // 对象保持帧的原始尺寸、居中放进头像框，再用缩放压到框内：缩放围绕图像中心进行，
    // 压小后的可见区域正好落在头像框中央，不需要额外裁剪。
    lv_obj_set_size(img, s.anim_header.width, s.anim_header.height);
    lv_obj_center(img);

    int longest = s.anim_header.width > s.anim_header.height
                    ? s.anim_header.width : s.anim_header.height;
    lv_animimg_set_src(img, s.anim_src, (size_t)count);
    lv_image_set_scale(img, (uint32_t)(256 * 56 / longest));
    lv_animimg_set_duration(img, app_anim_frame_ms_get(&s.anim_header) * (uint32_t)count);
    lv_animimg_set_repeat_count(img, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(img);

    s.anim = img;
    return true;
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
// 页面内容
// ---------------------------------------------------------------------------

static void build_badge(void)
{
    lv_obj_t *v = s.page.content;
    // 先释放上一轮动图再清视图：lv_obj_clean 会顺带删掉 animimg，若先清空，s.anim
    // 就成了悬空指针，解映射也就无从谈起。
    idv_anim_release();
    lv_obj_clean(v);
    s.hdr_right = NULL;

    app_badge_list_t *list = app_state_badges();
    if (!list || list->count <= 0) {
        ui_empty_create(v, "还没有工牌",
                        "请用手机配置页添加，或按 OK 先添加一张默认工牌");
        ui_page_set_hint("OK 添加默认工牌  长按OK 返回");
        return;
    }

    int sel = app_state_badge_selected();
    if (sel < 0) sel = 0;
    if (sel >= list->count) sel = list->count - 1;
    app_badge_t *b = &list->items[sel];

    ui_header_create(v, "电子工牌", NULL, NULL, &s.hdr_right);
    if (s.hdr_right) {
        lv_label_set_text_fmt(s.hdr_right, "%d / %d", sel + 1, list->count);
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

    // 有绑定动图槽位就播动图，否则退回首字占位——绝不出现空头像框。
    if (!idv_anim_mount(avatar, b)) {
        char ch[8];
        idv_first_char(b->nickname, ch, sizeof(ch));
        lv_obj_t *ava_lbl = ui_label_create(avatar, ch[0] ? ch : "?",
                                            ui_font_title, ui_c_bg());
        lv_obj_center(ava_lbl);
    }

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

    // 二维码卡：左侧是当前选中码的缩略图，右侧紧凑列出全部标签，选中项用主题色标出。
    int qr_h = 214 - card_h;
    if (qr_h > 100) qr_h = 100;
    if (qr_h < 72) qr_h = 72;
    int box = qr_h - 10;

    lv_obj_t *qcard = ui_card_create(v, 0, 0, IDV_CW, qr_h, 0);
    int qn = app_badge_qr_count(b);
    if (s.qr_sel >= qn) s.qr_sel = 0;

    if (qn > 0) {
        lv_obj_t *white = lv_obj_create(qcard);
        lv_obj_remove_flag(white, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(white, 6, 5);
        lv_obj_set_size(white, box, box);
        lv_obj_set_style_bg_color(white, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(white, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(white, 4, 0);
        lv_obj_set_style_border_width(white, 0, 0);
        lv_obj_set_style_pad_all(white, 0, 0);

        lv_obj_t *cv = qr_make(white, app_badge_qr_text(b, s.qr_sel), box - 6,
                               s_qr_thumb_buf, sizeof(s_qr_thumb_buf));
        if (cv) lv_obj_center(cv);

        int lx = 12 + box;
        for (int i = 0; i < qn; i++) {
            char scratch[APP_BADGE_QR_LABEL_LEN];
            const char *label = app_badge_qr_label(b, i, scratch, sizeof(scratch));
            if (!label) continue;
            lv_obj_t *lbl = ui_label_create(qcard, label, ui_font_hint,
                i == s.qr_sel ? ui_c_accent() : ui_c_dim());
            lv_obj_set_width(lbl, IDV_CW - lx - 6);
            lv_obj_set_pos(lbl, lx, 8 + i * 23);
        }
    } else {
        lv_obj_t *empty = ui_label_create(qcard, "此卡未设置二维码\n可在手机配置页添加",
                                          ui_font_hint, ui_c_dim());
        lv_obj_set_width(empty, IDV_CW);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    }

    if (qn > 1) ui_page_set_hint("↑↓ 换牌  OK 选码  长按↑ 全屏  长按OK 返回");
    else        ui_page_set_hint("↑↓ 切换工牌  长按↑ 全屏  长按OK 返回");
}

// ---------------------------------------------------------------------------
// 覆盖层：全屏二维码
// ---------------------------------------------------------------------------

static void close_overlay(void)
{
    if (s.overlay) {
        lv_obj_delete(s.overlay);
        s.overlay = NULL;
    }
}

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

// 全屏展示工牌的第 qr_index 个二维码。标题取该码的标签；同一张工牌有多个码时，
// 提示条告诉用户用 ↑↓ 在码之间切换。
static void open_qr_overlay(const app_badge_t *b, int qr_index)
{
    int n = app_badge_qr_count(b);
    if (n <= 0) return;
    if (qr_index < 0 || qr_index >= n) qr_index = 0;

    const char *text = app_badge_qr_text(b, qr_index);
    if (!text) return;

    char scratch[APP_BADGE_QR_LABEL_LEN];
    const char *label = app_badge_qr_label(b, qr_index, scratch, sizeof(scratch));

    lv_obj_t *ov = overlay_begin();
    s.qr_overlay_index = qr_index;

    lv_obj_t *title = ui_label_create(ov, label ? label : "二维码",
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

    lv_obj_t *cv = qr_make(white, text, 200, s_qr_full_buf, sizeof(s_qr_full_buf));
    if (cv) lv_obj_center(cv);

    lv_obj_t *hint = ui_label_create(ov, n > 1 ? "↑↓ 切换二维码  短按 OK 或长按 OK 退出"
                                               : "短按 OK 或长按 OK 退出",
                                     ui_font_hint, ui_c_dim());
    lv_obj_set_width(hint, UI_W - 8);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 4, UI_H - UI_HINT_H);
}

// ---------------------------------------------------------------------------
// 按键
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

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_identity_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.page = ui_page_create(NULL);
    build_badge();
}

void page_identity_exit(void)
{
    idv_anim_release();
    close_overlay();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_identity_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 覆盖层打开时独占按键：全屏二维码可用 ↑↓ 在同一张工牌的多个码之间切换，OK 关闭。
    if (s.overlay) {
        app_badge_list_t *list = app_state_badges();
        int sel = app_state_badge_selected();
        if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) &&
            list && sel >= 0 && sel < list->count) {
            app_badge_t *b = &list->items[sel];
            int n = app_badge_qr_count(b);
            if (n > 1) {
                s.qr_overlay_index = (s.qr_overlay_index +
                    (btn == BSP_BTN_UP ? n - 1 : 1)) % n;
                open_qr_overlay(b, s.qr_overlay_index);
            }
        } else if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            close_overlay();
        }
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }

    app_badge_list_t *list = app_state_badges();

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        if (!list || list->count <= 0) return;
        int sel = app_state_badge_selected();
        if (sel < 0) return;
        int qn = app_badge_qr_count(&list->items[sel]);
        if (qn <= 0) {
            ui_hint_flash("此卡未设置二维码", 1500);
            return;
        }
        if (s.qr_sel >= qn) s.qr_sel = 0;
        open_qr_overlay(&list->items[sel], s.qr_sel);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (!list || list->count <= 0) {
        if (btn == BSP_BTN_OK) badge_add_default();
        return;
    }
    if (btn == BSP_BTN_UP) {
        app_badge_cycle(list, -1);
    } else if (btn == BSP_BTN_DOWN) {
        app_badge_cycle(list, 1);
    } else if (btn == BSP_BTN_OK) {
        // 一张工牌带多个码时，OK 在码之间切换；只有单码或无码才沿用原来的"下一张工牌"，
        // 这样既补齐了多码选择，又不改变老用户的按键手感。
        int sel = app_state_badge_selected();
        int qn = (sel >= 0 && sel < list->count) ? app_badge_qr_count(&list->items[sel]) : 0;
        if (qn > 1) {
            s.qr_sel = (s.qr_sel + 1) % qn;
            build_badge();
            return;
        }
        app_badge_cycle(list, 1);
    } else {
        return;
    }
    app_state_save_badges();
    build_badge();
}

void page_identity_tick(void)
{
    // 工牌是静态内容，没有需要按节拍刷新的东西。
}