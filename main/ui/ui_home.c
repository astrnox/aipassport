// main/ui/ui_home.c —— 主页：个人名片卡 + 三张信息卡 + 六个模块入口 + 快捷面板。
//
// 主页是开机第一眼的内容，遵循"信息先于菜单"：名片卡、时间、下一作息节点倒计时、
// 赛事卡排在模块列表之前，用户不点进任何一层就能获得主要价值。信息卡片不参与焦点
// 移动，OK 只作用于模块列表。
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
#include "logic/app_esports.h"
#include "logic/app_pomodoro.h"
#include "logic/app_routine.h"
#include "logic/app_text.h"
#include "logic/app_time.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define HOME_CW        (UI_W - 2 * UI_MARGIN_X)   // 224
#define HOME_QUICK_N   4

// 个人名片卡几何：头像框是正方形，文字区占右侧剩余宽度。
#define HOME_AVATAR    56
#define HOME_CARD_H    72
#define HOME_TEXT_W    136

enum { QUIET_MUTE = 0, QUIET_THEME, QUIET_BRIGHT, QUIET_POMO };

static const char *const QUIET_NAMES[HOME_QUICK_N] = {
    "静音", "主题", "亮度", "开始番茄钟"
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

    lv_obj_t *time_lbl;
    lv_obj_t *date_lbl;
    lv_obj_t *lunar_lbl;

    lv_obj_t *routine_title;
    lv_obj_t *routine_sub;

    lv_obj_t *esport_title;
    lv_obj_t *esport_sub;

    ui_row_t rows[6];
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
// 第一条非空文本行。没有工牌时给出引导文案。卡片不参与焦点，OK 仍只作用于模块列表。
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
// 信息卡片
// ---------------------------------------------------------------------------

static void render_time(void)
{
    if (!s.time_lbl) return;
    app_datetime_t now = app_state_now();
    lv_label_set_text_fmt(s.time_lbl, "%02d:%02d", now.hour, now.minute);

    int wd = app_time_weekday(now.year, now.month, now.day);
    char date[32];
    app_fmt_date_short(date, sizeof(date), now.month, now.day, wd);
    lv_label_set_text(s.date_lbl, date);

    app_lunar_t lunar;
    if (app_lunar_from_solar(now.year, now.month, now.day, &lunar)) {
        const char *term = app_solar_term_name(now.year, now.month, now.day);
        char buf[40];
        if (term) snprintf(buf, sizeof(buf), "%s%s · %s", lunar.leap ? "闰" : "",
                           lunar.month_name, term);
        else snprintf(buf, sizeof(buf), "%s%s%s", lunar.leap ? "闰" : "",
                      lunar.month_name, lunar.day_name);
        lv_label_set_text(s.lunar_lbl, buf);
    } else {
        lv_label_set_text(s.lunar_lbl, "");
    }
}

static void render_routine(void)
{
    if (!s.routine_title) return;

    app_datetime_t now = app_state_now();
    int wd = app_time_weekday(now.year, now.month, now.day);
    const app_routine_day_t *day = app_state_routine_day(wd);
    if (!day) {
        lv_label_set_text(s.routine_title, "暂无作息表");
        lv_label_set_text(s.routine_sub, "进入作息模块可套用模板");
        return;
    }

    app_routine_status_t st;
    app_routine_status(day, now.hour * 60 + now.minute, now.second, &st);

    char cd[16];
    if (st.pos == APP_ROUTINE_IN_NODE) {
        const app_routine_node_t *cur = &day->nodes[st.current_index];
        if (st.next_index >= 0) {
            const app_routine_node_t *nx = &day->nodes[st.next_index];
            char title[40];
            snprintf(title, sizeof(title), "距离%s", nx->name);
            lv_label_set_text(s.routine_title, title);
            app_fmt_countdown(cd, sizeof(cd), st.seconds_to_next);
        } else {
            lv_label_set_text(s.routine_title, "距离本节结束");
            app_fmt_countdown(cd, sizeof(cd), st.seconds_to_end);
        }
        char sub[48];
        char t1[8], t2[8];
        app_fmt_hhmm(t1, sizeof(t1), cur->start_min);
        app_fmt_hhmm(t2, sizeof(t2), cur->end_min);
        snprintf(sub, sizeof(sub), "%s %s - %s", cur->name, t1, t2);
        lv_label_set_text(s.routine_sub, sub);
    } else if (st.pos == APP_ROUTINE_BETWEEN && st.next_index >= 0) {
        const app_routine_node_t *nx = &day->nodes[st.next_index];
        char title[40];
        snprintf(title, sizeof(title), "距离%s", nx->name);
        lv_label_set_text(s.routine_title, title);
        app_fmt_countdown(cd, sizeof(cd), st.seconds_to_next);
        char sub[48];
        char t1[8];
        app_fmt_hhmm(t1, sizeof(t1), nx->start_min);
        snprintf(sub, sizeof(sub), "下一个 %s %s", nx->name, t1);
        lv_label_set_text(s.routine_sub, sub);
    } else {
        lv_label_set_text(s.routine_title, "今日作息已结束");
        char sub[32];
        app_fmt_date_short(sub, sizeof(sub), now.month, now.day, wd);
        lv_label_set_text(s.routine_sub, sub);
        return;
    }
    lv_label_set_text(s.time_lbl, lv_label_get_text(s.time_lbl));   // 占位，无副作用
}

static void render_esports(void)
{
    if (!s.esport_title) return;

    app_esport_cache_t *cache = app_state_esports();
    int now_utc = (int)app_state_now_unix();

    if (!cache->valid || cache->match_count <= 0) {
        lv_label_set_text(s.esport_title, "暂无赛事数据");
        lv_label_set_text(s.esport_sub, "进入赛事中心联网获取");
        return;
    }

    int pick = app_esport_home_pick(cache->matches, cache->match_count, now_utc);
    if (pick < 0) {
        lv_label_set_text(s.esport_title, "今日暂无赛事");
        lv_label_set_text(s.esport_sub, "进入赛事中心查看本周赛程");
        return;
    }

    const app_esport_match_t *m = &cache->matches[pick];
    if (m->state == APP_MATCH_LIVE) {
        char title[48];
        snprintf(title, sizeof(title), "%s %d : %d %s",
                 m->team_a, m->score_a, m->score_b, m->team_b);
        lv_label_set_text(s.esport_title, title);
        char sub[32];
        snprintf(sub, sizeof(sub), "进行中 · 第%d局", m->score_a + m->score_b + 1);
        lv_label_set_text(s.esport_sub, sub);
        lv_obj_set_style_text_color(s.esport_sub, lv_color_hex(ui_c_live()), 0);
        return;
    }

    char title[48];
    snprintf(title, sizeof(title), "%s vs %s", m->team_a, m->team_b);
    lv_label_set_text(s.esport_title, title);

    app_settings_t *st = app_state_settings();
    int local_min = (m->start_utc + st->utc_offset_minutes * 60) % 86400;
    if (local_min < 0) local_min += 86400;
    int hh = local_min / 3600;
    int mm = (local_min % 3600) / 60;

    int delta = m->start_utc - now_utc;
    char sub[48];
    if (delta >= 0 && delta <= 24 * 3600) {
        char cd[16];
        app_fmt_countdown(cd, sizeof(cd), delta);
        snprintf(sub, sizeof(sub), "%02d:%02d 开赛 · 还有 %s", hh, mm, cd);
    } else {
        snprintf(sub, sizeof(sub), "%02d:%02d 开赛", hh, mm);
    }
    lv_label_set_text(s.esport_sub, sub);
    lv_obj_set_style_text_color(s.esport_sub,
        lv_color_hex(delta >= 0 && delta <= 30 * 60 ? ui_c_soon() : ui_c_dim()), 0);
}

// ---------------------------------------------------------------------------
// 模块列表
// ---------------------------------------------------------------------------

static void module_focus(int index)
{
    int count = ui_app_module_count();
    if (count <= 0) return;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;

    s.focus = index;
    for (int i = 0; i < count; i++) {
        ui_row_set_selected(s.rows[i], i == index);
    }
    ui_scroll_into_view(s.rows[index].obj);

    app_state_settings()->home_focus = index;
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
        app_pomodoro_t *p = app_state_pomodoro();
        const char *txt = (p->state == APP_POMO_FOCUS || p->state == APP_POMO_BREAK ||
                           p->state == APP_POMO_PAUSED) ? "进行中" : "开始";
        lv_label_set_text(s.quick_values[QUIET_POMO], txt);
    }
}

static void quick_focus(int index)
{
    if (index < 0) index = 0;
    if (index >= HOME_QUICK_N) index = HOME_QUICK_N - 1;
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

    lv_obj_t *card = ui_card_create(ov, 16, 62, UI_W - 32, 196, ui_c_accent());

    lv_obj_t *title = ui_label_create(card, "快捷面板", ui_font_title, ui_c_text());
    lv_obj_set_pos(title, 12, 8);

    for (int i = 0; i < HOME_QUICK_N; i++) {
        ui_row_t row = ui_row_create(card, QUIET_NAMES[i], "");
        lv_obj_set_pos(row.obj, 8, 38 + i * 36);
        lv_obj_set_width(row.obj, UI_W - 48);
        lv_obj_set_height(row.obj, 32);
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
    ui_page_set_hint("↑↓ 选择  OK 进入  长按↑ 面板  长按OK 熄屏");
    // 关闭面板后刷新主页卡片，反映静音/主题等变化。
    render_time();
    render_routine();
    render_esports();
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
    default: {
        app_pomodoro_t *p = app_state_pomodoro();
        if (p->state == APP_POMO_IDLE) app_pomodoro_start(p);
        else app_pomodoro_toggle(p);
        app_state_save_pomodoro();
        quick_update_values();
        ui_hint_flash("番茄钟已启动", 1500);
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
    s.page = ui_page_create("↑↓ 选择  OK 进入  长按↑ 面板  长按OK 熄屏");

    // 时间卡。
    lv_obj_t *card = ui_card_create(s.page.content, 0, 0, HOME_CW, 64, ui_c_accent());
    s.time_lbl = ui_label_create(card, "--:--", ui_font_display, ui_c_text());
    lv_obj_set_pos(s.time_lbl, 10, 6);
    s.date_lbl = ui_label_create(card, "", ui_font_body, ui_c_text());
    lv_obj_align(s.date_lbl, LV_ALIGN_TOP_RIGHT, -10, 8);
    s.lunar_lbl = ui_label_create(card, "", ui_font_hint, ui_c_dim());
    lv_obj_align(s.lunar_lbl, LV_ALIGN_TOP_RIGHT, -10, 34);

    // 作息卡。
    card = ui_card_create(s.page.content, 0, 0, HOME_CW, 56, ui_c_soon());
    s.routine_title = ui_label_create(card, "", ui_font_body, ui_c_text());
    lv_obj_set_pos(s.routine_title, 10, 8);
    s.routine_sub = ui_label_create(card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(s.routine_sub, 10, 32);

    // 赛事卡。
    card = ui_card_create(s.page.content, 0, 0, HOME_CW, 56, ui_c_live());
    s.esport_title = ui_label_create(card, "", ui_font_body, ui_c_text());
    lv_obj_set_pos(s.esport_title, 10, 8);
    s.esport_sub = ui_label_create(card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(s.esport_sub, 10, 32);

    // 模块列表。
    int count = ui_app_module_count();
    if (count > 6) count = 6;
    lv_obj_t *list = ui_list_create(s.page.content);
    for (int i = 0; i < count; i++) {
        s.rows[i] = ui_row_create(list, ui_app_module_title(i), "");
    }

    render_time();
    render_routine();
    render_esports();

    int focus = app_state_settings()->home_focus;
    module_focus(focus);
}

void page_home_exit(void)
{
    // 焦点位置在这里落盘：每按一次 UP/DOWN 都写 NVS 会放大闪存写入，而离开主页必然
    // 经过这里（进入模块、回到主页都会先退出主页），写一次就够。
    if (s.page.scr) {
        app_state_settings()->home_focus = s.focus;
        app_state_save_settings();
    }

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

    if (btn == BSP_BTN_UP) module_focus(s.focus - 1);
    else if (btn == BSP_BTN_DOWN) module_focus(s.focus + 1);
    else if (btn == BSP_BTN_OK) ui_app_open_module(s.focus);
}

void page_home_tick(void)
{
    if (!s.page.scr) return;
    render_time();
    render_routine();
    render_esports();
    if (s.quick) quick_update_values();
}
