// main/ui/ui_home.c —— 主页：个人名片卡 + 模块轮播。
//
// 用户要求主页"只有个人名片与时钟，而无其他"：时钟由全局状态栏始终显示在左上角（很小、
// 常驻），主页内容区只放个人名片卡。原先的主页把大号时间卡、作息卡、赛事卡、竖排模块
// 列表全塞在一起，信息过载、要滑很久才能看到模块入口，因此整体删掉。
//
// 六个模块入口改为横向轮播：UP 看上一个、DOWN 看下一个，到两端再按会循环回另一端
// （第一个再按 UP 跳到最后一个，最后一个再按 DOWN 回到第一个），OK 进入当前模块。
// 轮播卡内用左右箭头、页码与圆点同时表达"还有其它模块"，不让用户以为只有一张卡。
//
// 个人名片卡把选中工牌的身份直接搬到首页：左侧是头像，有动图时用 lv_animimg 播放
// 手机端上传的 RGB565 帧序列，右侧是昵称与一行补充信息。动图帧不复制进 RAM，而是
// 用 app_assets_map() 把 assets 分区零拷贝映射出来，LVGL 直接把它们当图源渲染；
// 因此映射必须在整个播放期内保持有效，只有删掉 animimg 之后才允许解映射。
//
// 快捷面板挂在主页屏幕之上，承载静音 / 主题 / 亮度 / 开始番茄钟四项。面板打开时
// 由控制器把按键转交 home_quick_key()，关闭后恢复主页按键。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_assets.h"
#include "app_state.h"
#include "logic/app_anim.h"
#include "logic/app_badge.h"
#include "logic/app_pomodoro.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define HOME_CW        (UI_W - 2 * UI_MARGIN_X)   // 224
#define HOME_QUICK_N   5

// 个人名片卡几何：头像框是正方形，文字区占右侧剩余宽度。高度压到 64：正好容纳 56
// 头像与两行文字（16px 行高 31 + 12px 行高 23）。
#define HOME_AVATAR    56
#define HOME_CARD_H    64
#define HOME_TEXT_W    136

// 模块轮播卡：中间是大号模块名，上方左右箭头，下方页码与圆点。
#define HOME_CAROUSEL_H  120
#define HOME_MODULE_MAX  6

enum { QUIET_MUTE = 0, QUIET_THEME, QUIET_BRIGHT, QUIET_POMO, QUIET_DND };

static const char *const QUIET_NAMES[HOME_QUICK_N] = {
    "静音", "主题", "亮度", "开始番茄钟", "免打扰"
};

static struct {
    ui_page_t page;

    // 个人名片卡动图。anim_dsc/anim_src 必须与 animimg 同生命周期：LVGL 只保存数组
    // 指针而不复制，帧指针又指向映射区，任何一个先失效都会让下一帧访问非法地址。
    lv_obj_t *anim_obj;
    lv_image_dsc_t anim_dsc[APP_ANIM_MAX_FRAMES];
    const void *anim_src[APP_ANIM_MAX_FRAMES];
    app_anim_header_t anim_header;
    esp_partition_mmap_handle_t anim_handle;
    bool anim_mapped;

    // 模块轮播
    lv_obj_t *mod_card;
    lv_obj_t *mod_title;
    lv_obj_t *mod_page;
    lv_obj_t *mod_dots[HOME_MODULE_MAX];
    int module_count;
    int focus;

    lv_obj_t *quick;
    int quick_sel;
    lv_obj_t *quick_rows[HOME_QUICK_N];
    lv_obj_t *quick_values[HOME_QUICK_N];
} s;

// ---------------------------------------------------------------------------
// 个人名片卡：动图头像
// ---------------------------------------------------------------------------

// 释放动图：先删 animimg，再解映射。顺序不能反——对象存活期间 lv_animimg 持有的
// 图源指针还指向映射区，提前解映射会让动画的下一帧读到非法地址。
static void avatar_anim_release(void)
{
    if (s.anim_obj) {
        lv_obj_delete(s.anim_obj);
        s.anim_obj = NULL;
    }
    if (s.anim_mapped) {
        app_assets_unmap(s.anim_handle);
        s.anim_mapped = false;
    }
}

// 取 UTF-8 首字符（无动图时用昵称首字占位）。
static void home_first_char(const char *text, char *out, size_t cap)
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

// 为工牌动图建立帧描述符并把帧序列挂到 box 上。返回 true 表示已创建 animimg；
// 槽位为空、分区不可用或映射失败时返回 false，由调用方退回首字占位。
static bool avatar_anim_mount(lv_obj_t *box, const app_badge_t *b)
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
    if (count < 1) { avatar_anim_release(); return false; }
    if (count > APP_ANIM_MAX_FRAMES) count = APP_ANIM_MAX_FRAMES;

    uint32_t frame_bytes = app_anim_frame_bytes(s.anim_header.width, s.anim_header.height);
    if (frame_bytes == 0) { avatar_anim_release(); return false; }

    for (int i = 0; i < count; i++) {
        const uint8_t *px = app_assets_frame(&s.anim_header, i);
        if (!px) { avatar_anim_release(); return false; }

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
    // 对象保持帧的原始尺寸、居中放在头像框里，再用缩放把它压到框内：缩放围绕图像
    // 中心进行，压小后的可见区域正好落在头像框中央，不需要额外裁剪。
    lv_obj_set_size(img, s.anim_header.width, s.anim_header.height);
    lv_obj_center(img);

    int longest = s.anim_header.width > s.anim_header.height ? s.anim_header.width
                                                             : s.anim_header.height;
    lv_animimg_set_src(img, s.anim_src, (size_t)count);
    lv_image_set_scale(img, (uint32_t)(256 * HOME_AVATAR / longest));
    lv_animimg_set_duration(img, app_anim_frame_ms_get(&s.anim_header) * (uint32_t)count);
    lv_animimg_set_repeat_count(img, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(img);

    s.anim_obj = img;
    return true;
}

// 建个人名片卡：取当前选中工牌，左侧头像（有动图就播，否则显首字），右侧昵称与
// 第一条非空文本行。没有工牌时给出引导文案。
static void build_badge_card(void)
{
    app_badge_list_t *list = app_state_badges();
    app_badge_t *b = NULL;
    if (list && list->count > 0) {
        int sel = app_state_badge_selected();
        if (sel < 0 || sel >= list->count) sel = 0;
        b = &list->items[sel];
    }

    lv_obj_t *card = ui_card_create(s.page.content, 0, 0, HOME_CW, HOME_CARD_H,
                                    ui_c_accent());

    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, 8, (HOME_CARD_H - HOME_AVATAR) / 2);
    lv_obj_set_size(box, HOME_AVATAR, HOME_AVATAR);
    lv_obj_set_style_bg_color(box, lv_color_hex(ui_c_accent()), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    if (!b || !avatar_anim_mount(box, b)) {
        char ch[8];
        home_first_char(b ? b->nickname : NULL, ch, sizeof(ch));
        lv_obj_t *ava = ui_label_create(box, ch[0] ? ch : "?", ui_font_title, ui_c_bg());
        lv_obj_center(ava);
    }

    lv_obj_t *nick = ui_label_create(card,
        (b && b->nickname[0]) ? b->nickname : "还没有工牌",
        ui_font_body, ui_c_text());
    lv_obj_set_width(nick, HOME_TEXT_W);
    lv_obj_set_pos(nick, 74, 6);

    const char *sub = NULL;
    if (b) {
        for (int i = 0; i < APP_BADGE_MAX_LINES; i++) {
            if (b->lines[i][0]) { sub = b->lines[i]; break; }
        }
    }
    if (!sub) sub = b ? "身份与工具可编辑" : "在身份与工具中添加";

    lv_obj_t *sub_lbl = ui_label_create(card, sub, ui_font_hint, ui_c_dim());
    lv_obj_set_width(sub_lbl, HOME_TEXT_W);
    lv_obj_set_pos(sub_lbl, 74, 6 + 31);
}

// ---------------------------------------------------------------------------
// 模块轮播
// ---------------------------------------------------------------------------

// 刷新轮播卡：当前模块名、页码与圆点。焦点永远在 0..module_count-1 之间。
static void module_render(void)
{
    if (!s.mod_card || s.module_count <= 0) return;

    lv_label_set_text(s.mod_title, ui_app_module_title(s.focus));

    char page[16];
    snprintf(page, sizeof(page), "%d / %d", s.focus + 1, s.module_count);
    lv_label_set_text(s.mod_page, page);

    for (int i = 0; i < HOME_MODULE_MAX; i++) {
        if (!s.mod_dots[i]) continue;
        if (i >= s.module_count) {
            lv_obj_add_flag(s.mod_dots[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s.mod_dots[i], LV_OBJ_FLAG_HIDDEN);
        bool on = (i == s.focus);
        lv_obj_set_style_bg_color(s.mod_dots[i],
            lv_color_hex(on ? ui_c_accent() : ui_c_border()), 0);
        lv_obj_set_size(s.mod_dots[i], on ? 8 : 6, on ? 8 : 6);
    }
}

// 循环移动焦点：到两端再按会绕回另一端。
static void module_move(int delta)
{
    if (s.module_count <= 0) return;
    s.focus = (s.focus + delta) % s.module_count;
    if (s.focus < 0) s.focus += s.module_count;
    module_render();
    app_state_settings()->home_focus = s.focus;
}

static void build_module_carousel(void)
{
    s.module_count = ui_app_module_count();
    if (s.module_count > HOME_MODULE_MAX) s.module_count = HOME_MODULE_MAX;
    if (s.module_count < 1) s.module_count = 1;

    s.mod_card = ui_card_create(s.page.content, 0, 0, HOME_CW, HOME_CAROUSEL_H,
                                ui_c_accent());

    lv_obj_t *left = ui_label_create(s.mod_card, LV_SYMBOL_LEFT, ui_font_title, ui_c_dim());
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 8, -8);
    lv_obj_t *right = ui_label_create(s.mod_card, LV_SYMBOL_RIGHT, ui_font_title, ui_c_dim());
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -8, -8);

    s.mod_title = ui_label_create(s.mod_card, "", ui_font_title, ui_c_text());
    lv_obj_set_width(s.mod_title, HOME_CW - 24);
    lv_obj_set_style_text_align(s.mod_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s.mod_title, LV_LABEL_LONG_WRAP);
    lv_obj_align(s.mod_title, LV_ALIGN_CENTER, 0, -12);

    s.mod_page = ui_label_create(s.mod_card, "", ui_font_hint, ui_c_dim());
    lv_obj_align(s.mod_page, LV_ALIGN_CENTER, 0, 22);

    // 圆点排成一行，居中放在卡片底部，直观表达"六个模块"。
    lv_obj_t *dots = lv_obj_create(s.mod_card);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 12);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 6, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < HOME_MODULE_MAX; i++) {
        lv_obj_t *dot = lv_obj_create(dots);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(ui_c_border()), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        s.mod_dots[i] = dot;
    }

    int focus = app_state_settings()->home_focus;
    if (focus < 0 || focus >= s.module_count) focus = 0;
    s.focus = focus;
    module_render();
}

// ---------------------------------------------------------------------------
// 快捷面板
// ---------------------------------------------------------------------------

static void quick_update_values(void)
{
    app_settings_t *st = app_state_settings();

    if (s.quick_values[QUIET_MUTE]) {
        lv_label_set_text(s.quick_values[QUIET_MUTE], st->sound_muted ? "已静音" : "开");
    }
    if (s.quick_values[QUIET_THEME]) {
        const char *t = st->theme == APP_THEME_FIXED_LIGHT ? "浅色"
                      : st->theme == APP_THEME_AUTO ? "自动" : "深色";
        lv_label_set_text(s.quick_values[QUIET_THEME], t);
    }
    if (s.quick_values[QUIET_BRIGHT]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", st->backlight);
        lv_label_set_text(s.quick_values[QUIET_BRIGHT], buf);
    }
    if (s.quick_values[QUIET_POMO]) {
        // 空闲显示"开始"，其余阶段借逻辑层的统一名称，短休息与长休息因此可以区分，
        // 新增的长休息状态也不会漏判成"未开始"。
        app_pomodoro_t *p = app_state_pomodoro();
        const char *txt = (p->state == APP_POMO_IDLE)
                              ? "开始"
                              : app_pomodoro_state_name(p->state);
        lv_label_set_text(s.quick_values[QUIET_POMO], txt);
    }
    if (s.quick_values[QUIET_DND]) {
        app_pomodoro_t *p = app_state_pomodoro();
        lv_label_set_text(s.quick_values[QUIET_DND], p->do_not_disturb ? "开" : "关");
        lv_obj_set_style_text_color(s.quick_values[QUIET_DND],
            lv_color_hex(p->do_not_disturb ? ui_c_ok() : ui_c_dim()), 0);
    }
}

static void quick_focus(int index)
{
    // 与主页模块轮播、设置列表同一套循环：最后一项再按 DOWN 回到第一项。
    index %= HOME_QUICK_N;
    if (index < 0) index += HOME_QUICK_N;
    s.quick_sel = index;
    for (int i = 0; i < HOME_QUICK_N; i++) {
        lv_obj_t *row = s.quick_rows[i];
        if (!row) continue;
        lv_obj_t *title = lv_obj_get_child(row, 1);   // 0 是指示条，1 是标题
        if (title) {
            lv_obj_set_style_text_color(title,
                lv_color_hex(i == index ? ui_c_accent() : ui_c_text()), 0);
        }
    }
}

// 快捷面板几何。卡片高度由行数推出来，而不是写死一个数：原先写死 196，五行按
// 38 + i*36 排到最后一行下沿已到 214，第五项"免打扰"整个落在卡片外被裁掉，
// 用户怎么按都看不到——行数一多就会复发，所以这里让它随 HOME_QUICK_N 自动长高。
#define QUICK_TOP      38
#define QUICK_ROW_H    32
#define QUICK_STEP     36
#define QUICK_PAD_B    8
#define QUICK_CARD_H   (QUICK_TOP + (HOME_QUICK_N - 1) * QUICK_STEP + QUICK_ROW_H + QUICK_PAD_B)

#define HINT_HOME "↑↓ 切换模块  OK 进入  长按↑ 面板  长按OK 熄屏"

void home_quick_open(void)
{
    if (s.quick) return;

    lv_obj_t *ov = lv_obj_create(s.page.scr);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_size(ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);

    lv_obj_t *card = ui_card_create(ov, 16, (UI_H - QUICK_CARD_H) / 2, UI_W - 32,
                                    QUICK_CARD_H, ui_c_accent());

    lv_obj_t *title = ui_label_create(card, "快捷面板", ui_font_title, ui_c_text());
    lv_obj_set_pos(title, 12, 8);

    for (int i = 0; i < HOME_QUICK_N; i++) {
        ui_row_t row = ui_row_create(card, QUIET_NAMES[i], "");
        lv_obj_set_pos(row.obj, 8, QUICK_TOP + i * QUICK_STEP);
        lv_obj_set_width(row.obj, UI_W - 48);
        lv_obj_set_height(row.obj, QUICK_ROW_H);
        s.quick_rows[i] = row.obj;
        s.quick_values[i] = row.value;
    }
    s.quick = card;
    s.quick_sel = 0;
    quick_update_values();
    quick_focus(0);
    ui_page_set_hint("↑↓ 选择  OK 切换  长按OK 关闭");
}

void home_quick_close(void)
{
    if (!s.quick) return;
    lv_obj_delete(s.quick);
    s.quick = NULL;
    ui_page_set_hint(HINT_HOME);
}

bool home_quick_active(void) { return s.quick != NULL; }

static void quick_activate(void)
{
    app_settings_t *st = app_state_settings();
    switch (s.quick_sel) {
    case QUIET_MUTE:
        st->sound_muted = !st->sound_muted;
        app_state_save_settings();
        quick_update_values();
        break;
    case QUIET_THEME:
        st->theme = (app_theme_choice_t)((st->theme + 1) % 3);
        app_state_save_settings();
        // 主题切换即时生效需要重建页面；这里给出提示，由退出重进生效。
        ui_hint_flash("主题已切换，返回后生效", 1500);
        quick_update_values();
        break;
    case QUIET_BRIGHT:
        st->backlight = (uint8_t)(st->backlight >= 100 ? 10 : st->backlight + 10);
        app_state_save_settings();
        quick_update_values();
        break;
    case QUIET_POMO: {
        app_pomodoro_t *p = app_state_pomodoro();
        if (p->state == APP_POMO_IDLE) app_pomodoro_start(p);
        else app_pomodoro_toggle(p);
        app_state_save_pomodoro();
        quick_update_values();
        ui_hint_flash("番茄钟已启动", 1500);
        break;
    }
    default: {   // QUIET_DND：免打扰只改开关，不动计时与时长。
        app_pomodoro_t *p = app_state_pomodoro();
        bool on = !p->do_not_disturb;
        app_pomodoro_set_dnd(p, on);
        app_state_save_pomodoro();
        quick_update_values();
        ui_hint_flash(on ? "专注时免打扰已开启" : "专注时免打扰已关闭", 1500);
        break;
    }
    }
}

void home_quick_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.quick) return;

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        home_quick_close();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) quick_focus(s.quick_sel - 1);
    else if (btn == BSP_BTN_DOWN) quick_focus(s.quick_sel + 1);
    else if (btn == BSP_BTN_OK) quick_activate();
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_home_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.page = ui_page_create(HINT_HOME);

    // 个人名片卡排在最前：开机第一眼先看到自己的身份。
    build_badge_card();

    // 模块轮播卡：六个模块入口。
    build_module_carousel();
}

void page_home_exit(void)
{
    // 焦点位置在这里落盘：每按一次 UP/DOWN 都写 NVS 会放大闪存写入，而离开主页必然
    // 经过这里（进入模块、回到主页都会先退出主页），写一次就够。
    if (s.page.scr) {
        app_state_settings()->home_focus = s.focus;
        app_state_save_settings();
    }

    // 先释放名片动图：animimg 持有的帧指针指向 assets 映射区，必须在删除屏幕之前
    // 删掉 animimg 再解映射，否则动画的下一帧会读到已经失效的地址，映射本身也会泄
    // 漏，导致后续页面无法再映射任何槽位。
    avatar_anim_release();
    if (s.quick) {
        lv_obj_delete(s.quick);
        s.quick = NULL;
    }
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_home_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) module_move(-1);
    else if (btn == BSP_BTN_DOWN) module_move(1);
    else if (btn == BSP_BTN_OK) ui_app_open_module(s.focus);
}

void page_home_tick(void)
{
    if (!s.page.scr) return;
    if (s.quick) quick_update_values();
}