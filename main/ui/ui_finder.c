// main/ui/ui_finder.c —— 工具页子页：找设备（蓝牙查找器）。
//
// 普通用户不理解"BLE 广播扫描"，但理解"我的耳机丢哪了"。所以本页把信号强度翻译成
// 0..100 的接近度加一句人话（"就在附近"），并允许把常找的东西存成"我的设备"——走到
// 附近时它们排在最上面、实时显示信号，不用在一堆"未知设备"里猜哪一个才是自己的。
//
// 交互取舍：
//  - "我的设备"常驻列表最上，即使暂时没搜到也保留一行"没搜到"。若搜不到就从列表消失，
//    用户会以为设备被删掉了，反而更慌。
//  - 只有三个键、没有输入法：收藏就是把当前选中项存起来，用长按 ↑ 一键完成（再长按一次
//    取消）。存错可以再存，代价远小于弹一个需要选"是/否"的对话框来打断找东西。
//  - 长按 OK 恒为本项目统一的"返回工具页"；长按 ↑ 才是本页的"存/取消"，两者不混用。
//    唯一的例外是蓝牙压根没打开（配网占用、或信道体检正在扫 Wi-Fi）：此时扫描没在跑、
//    没有可收藏的项，页内直接给出具体原因，长按 ↑ 改为"重试打开蓝牙"，用户不必退出
//    再进来。提示行与长按↑ 的实际行为永远一致，不会写着"重试"结果去存收藏。
//
// 蓝牙协议栈的拉起/拆除要几百毫秒，直接在按键回调里做会卡住界面，所以开启与停止都走
// net/app_ble 的异步请求；本页只读快照，绝不直接触碰 NimBLE。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"

#include "net/app_ble.h"

#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define FD_CW         (UI_W - 2 * UI_MARGIN_X)              // 内容区可用宽度 224
#define FD_MAX        (APP_FINDER_SAVED_MAX + APP_FINDER_MAX)  // 最多 8 + 16 行
#define FD_REFRESH_MS 800                                    // 刷新节拍：够跟手，又不至于狂重建

static uint32_t mono_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

typedef enum {
    FD_LIST = 0,
    FD_TRACK,
} fd_view_t;

// 列表一行要显示的全部信息。收藏项与附近项放进同一个数组，收藏在数组前段。
typedef struct {
    uint8_t addr[6];
    char    name[APP_FINDER_NAME_LEN];
    app_finder_cat_t category;   // 启发式类别；UNKNOWN 表示没有可显示的判断
    bool    saved;
    bool    present;    // 本次快照里是否搜到
    int     rssi;       // present 时有效
    int     closeness;  // 0..100
} fd_entry_t;

static struct {
    bool      active;
    ui_page_t page;
    fd_view_t view;

    // 快照约 1KB，放静态区，不占界面任务（LVGL 任务）的栈。
    app_finder_t snap;
    fd_entry_t   entries[FD_MAX];
    int          count;
    int          focus;

    ui_row_t  rows[FD_MAX];
    lv_obj_t *hdr_right;

    uint8_t sel_addr[6];      // 按地址记选中项：列表每秒重排，用下标会跟丢
    bool    sel_valid;

    uint8_t  track_addr[6];   // 追踪屏正在看的设备
    lv_obj_t *t_pct;
    lv_obj_t *t_level;
    lv_obj_t *t_bar;
    lv_obj_t *t_addr;

    uint32_t last_ms;
} s;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static void display_name(const uint8_t addr[6], const char *name, char *out, size_t cap)
{
    if (name && name[0]) {
        snprintf(out, cap, "%.12s", name);
        return;
    }
    // 没有广播名的设备靠地址后两字节区分，否则一行行全是"未知设备"没法选。
    snprintf(out, cap, "未知设备 %02X%02X", addr[4], addr[5]);
}

static int focus_of_addr(const uint8_t addr[6])
{
    for (int i = 0; i < s.count; i++) {
        if (memcmp(s.entries[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

static uint32_t closeness_color(int closeness)
{
    if (closeness >= 55) return ui_c_ok();
    if (closeness >= 25) return ui_c_warn();
    return ui_c_live();
}

static const char *scan_state_text(void)
{
    if (app_ble_finder_last_error() != ESP_OK) return "未开启";
    return app_ble_finder_running() ? "扫描中" : "启动中";
}

// ---------------------------------------------------------------------------
// 数据：把一份快照摊成"我的设备 + 附近设备"的显示表
// ---------------------------------------------------------------------------

static int build_entries(void)
{
    app_ble_finder_snapshot(&s.snap);
    int n = 0;

    // 收藏项永远在前，即使这次没搜到也占一行。
    for (int i = 0; i < s.snap.saved_count && n < FD_MAX; i++) {
        fd_entry_t *e = &s.entries[n++];
        memset(e, 0, sizeof(*e));
        memcpy(e->addr, s.snap.saved_addr[i], 6);
        snprintf(e->name, sizeof(e->name), "%s", s.snap.saved_name[i]);
        e->saved = true;

        int di = app_finder_find(&s.snap, e->addr);
        if (di >= 0) {
            e->present = true;
            e->rssi = s.snap.devs[di].rssi;
            e->closeness = app_finder_closeness(e->rssi);
            e->category = s.snap.devs[di].category;   // 搜到了才有类别可显示
            if (!e->name[0] && s.snap.devs[di].has_name) {
                snprintf(e->name, sizeof(e->name), "%s", s.snap.devs[di].name);
            }
        }
    }

    // 附近设备里跳过已收藏的，避免同一台设备出现两次。
    for (int i = 0; i < s.snap.count && n < FD_MAX; i++) {
        if (app_finder_is_saved(&s.snap, s.snap.devs[i].addr)) continue;
        fd_entry_t *e = &s.entries[n++];
        memset(e, 0, sizeof(*e));
        memcpy(e->addr, s.snap.devs[i].addr, 6);
        if (s.snap.devs[i].has_name) {
            snprintf(e->name, sizeof(e->name), "%s", s.snap.devs[i].name);
        }
        e->present = true;
        e->rssi = s.snap.devs[i].rssi;
        e->closeness = app_finder_closeness(e->rssi);
        e->category = s.snap.devs[i].category;
    }
    return n;
}

// 按类别计数的汇总文案。只讲"有几台像什么"，不讲是谁的：MAC 会随机化、多数设备
// 不广播名字，这里永远只是启发式归类。只输出计数非零的类别，顺序固定，便于扫读。
static void category_summary(char *out, size_t cap)
{
    int counts[APP_FINDER_CAT_COUNT];
    app_finder_category_counts(&s.snap, counts);

    static const app_finder_cat_t order[] = {
        APP_FINDER_CAT_PHONE, APP_FINDER_CAT_EARBUDS, APP_FINDER_CAT_WATCH,
        APP_FINDER_CAT_TRACKER, APP_FINDER_CAT_AUDIO, APP_FINDER_CAT_COMPUTER,
        APP_FINDER_CAT_OTHER, APP_FINDER_CAT_UNKNOWN,
    };

    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        int cnt = counts[order[i]];
        if (cnt <= 0) continue;
        int w = snprintf(out + used, cap - used, "%s%s %d",
                         used == 0 ? "" : " · ",
                         app_finder_category_text(order[i]), cnt);
        if (w < 0 || (size_t)w >= cap - used) break;   // 放不下就停，宁可少显示也不溢出
        used += (size_t)w;
    }
}

// ---------------------------------------------------------------------------
// 列表视图
// ---------------------------------------------------------------------------

static void render_focus(void)
{
    for (int i = 0; i < s.count; i++) ui_row_set_selected(s.rows[i], i == s.focus);
    if (s.focus >= 0 && s.focus < s.count) ui_scroll_into_view(s.rows[s.focus].obj);
}

static void entry_text(const fd_entry_t *e, char *out, size_t cap)
{
    if (!e->present) {
        snprintf(out, cap, "没搜到");
        return;
    }
    snprintf(out, cap, "%d%% %s", e->closeness, app_finder_level(e->closeness));
}

static void build_list(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.t_pct = s.t_level = s.t_bar = s.t_addr = NULL;

    ui_header_create(c, "找设备", NULL, NULL, &s.hdr_right);
    if (s.hdr_right) lv_label_set_text(s.hdr_right, scan_state_text());

    bool failed = app_ble_finder_last_error() != ESP_OK;
    if (failed) {
        const char *why = app_ble_finder_error_text();
        ui_banner_create(c, why ? why : "蓝牙没能打开，请稍后重试", ui_c_warn());
    }

    // 类别计数汇总：放在标题下方作为副标题。措辞刻意只说"像什么"，不暗示设备归属。
    if (s.count > 0) {
        char summary[128];
        category_summary(summary, sizeof(summary));
        if (summary[0]) {
            char line[160];
            snprintf(line, sizeof(line), "类别统计：%s", summary);
            lv_obj_t *lbl = ui_label_create(c, line, ui_font_hint, ui_c_dim());
            if (lbl) {
                lv_obj_set_width(lbl, FD_CW);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            }
        }
    }

    if (s.count > 0) {
        lv_obj_t *list = ui_list_create(c);
        bool section_saved = false;
        bool section_near = false;
        for (int i = 0; i < s.count; i++) {
            if (s.entries[i].saved && !section_saved) {
                ui_label_create(list, "我的设备", ui_font_hint, ui_c_dim());
                section_saved = true;
            }
            if (!s.entries[i].saved && !section_near) {
                ui_label_create(list, section_saved ? "附近的其他设备" : "附近设备",
                                ui_font_hint, ui_c_dim());
                section_near = true;
            }

            char title[40];
            display_name(s.entries[i].addr, s.entries[i].name, title, sizeof(title));
            // 已知类别时把短标签拼在名字后（"AirPods · 耳机"）；未知则只显示名字，
            // 不给用户一个无意义的"未知"后缀。
            char title_cat[48];
            const char *cat_text = (s.entries[i].category != APP_FINDER_CAT_UNKNOWN)
                                       ? app_finder_category_text(s.entries[i].category)
                                       : NULL;
            if (cat_text) {
                snprintf(title_cat, sizeof(title_cat), "%s · %s", title, cat_text);
            }
            char val[32];
            entry_text(&s.entries[i], val, sizeof(val));
            s.rows[i] = ui_row_create(list, cat_text ? title_cat : title, val);

            uint32_t col = s.entries[i].present ? closeness_color(s.entries[i].closeness)
                                                : ui_c_dim();
            if (s.rows[i].value) lv_obj_set_style_text_color(s.rows[i].value, lv_color_hex(col), 0);
            if (s.entries[i].saved) ui_row_set_title_color(s.rows[i], ui_c_accent());
        }

        // 选中项：能按地址找到就跟着走，找不到（设备走了/被取消收藏）才退回同下标。
        if (s.sel_valid) {
            int fi = focus_of_addr(s.sel_addr);
            if (fi >= 0) s.focus = fi;
        }
        if (s.focus < 0) s.focus = 0;
        if (s.focus >= s.count) s.focus = s.count - 1;
        memcpy(s.sel_addr, s.entries[s.focus].addr, 6);
        s.sel_valid = true;
        render_focus();
    } else {
        ui_empty_create(c, "正在搜索蓝牙设备",
                        "打开耳机盒盖，或让要寻找的设备进入广播状态。"
                        "本页只显示正在发出蓝牙信号的设备。");
        s.focus = -1;
        s.sel_valid = false;
    }

    // 提示与长按↑ 的实际行为严格对应：蓝牙没打开时它是"重试"（此状态下扫描没跑，
    // 收藏 / 取消收藏都无从谈起）；否则才是本页正常的"存/取消"。
    if (failed) {
        ui_page_set_hint("长按↑ 重试打开蓝牙   长按OK 返回工具页");
    } else if (s.count <= 0) {
        ui_page_set_hint("长按OK 返回工具页");
    } else {
        ui_page_set_hint("↑↓ 选择  OK 追踪  长按↑ 存/取消  长按OK 返回");
    }
}

// ---------------------------------------------------------------------------
// 追踪视图：一屏只看一台，走动着找方向
// ---------------------------------------------------------------------------

static void track_update(void)
{
    if (!s.t_pct) return;   // 不在追踪屏

    int ti = focus_of_addr(s.track_addr);
    if (ti < 0 || !s.entries[ti].present) {
        lv_label_set_text(s.t_pct, "--");
        lv_obj_set_style_text_color(s.t_pct, lv_color_hex(ui_c_dim()), 0);
        lv_label_set_text(s.t_level, "还没搜到，往它可能在的地方走几步");
        lv_obj_set_style_text_color(s.t_level, lv_color_hex(ui_c_dim()), 0);
        ui_progress_set(s.t_bar, 0);
        if (s.t_addr) lv_label_set_text(s.t_addr, "");
        return;
    }

    int cl = s.entries[ti].closeness;
    uint32_t col = closeness_color(cl);
    lv_label_set_text_fmt(s.t_pct, "%d%%", cl);
    lv_obj_set_style_text_color(s.t_pct, lv_color_hex(col), 0);
    lv_label_set_text(s.t_level, app_finder_level(cl));
    lv_obj_set_style_text_color(s.t_level, lv_color_hex(col), 0);
    ui_progress_set(s.t_bar, cl * 10);
    if (s.t_addr) {
        char buf[40];
        snprintf(buf, sizeof(buf), "信号 %d dBm", s.entries[ti].rssi);
        lv_label_set_text(s.t_addr, buf);
    }
}

static void build_track(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.t_pct = s.t_level = s.t_bar = s.t_addr = NULL;

    int ti = focus_of_addr(s.track_addr);
    char name[40];
    display_name(s.track_addr, ti >= 0 ? s.entries[ti].name : "", name, sizeof(name));

    ui_header_create(c, "找设备", NULL, NULL, &s.hdr_right);
    if (s.hdr_right) lv_label_set_text(s.hdr_right, "追踪中");

    lv_obj_t *card = ui_card_create(c, 0, 0, FD_CW, 178, ui_c_accent());

    lv_obj_t *title = ui_label_create(card, name, ui_font_title, ui_c_text());
    lv_obj_set_width(title, FD_CW);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, 10);

    s.t_pct = ui_label_create(card, "--", ui_font_display_s, ui_c_accent());
    lv_obj_set_width(s.t_pct, FD_CW);
    lv_obj_set_style_text_align(s.t_pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.t_pct, 0, 42);

    s.t_level = ui_label_create(card, "", ui_font_body, ui_c_dim());
    lv_obj_set_width(s.t_level, FD_CW);
    lv_obj_set_style_text_align(s.t_level, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.t_level, 0, 90);

    s.t_bar = ui_progress_create(card, FD_CW - 32, 10, ui_c_accent());
    lv_obj_set_pos(s.t_bar, 16, 120);

    s.t_addr = ui_label_create(card, "", ui_font_hint, ui_c_dim());
    lv_obj_set_width(s.t_addr, FD_CW);
    lv_obj_set_style_text_align(s.t_addr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.t_addr, 0, 146);

    ui_banner_create(c, "边走边看百分比：越接近 100% 说明越近", ui_c_ok());
    ui_page_set_hint("↑↓ 换设备   OK 返回列表   长按OK 退出");

    track_update();
}

// ---------------------------------------------------------------------------
// 刷新与收藏
// ---------------------------------------------------------------------------

static void refresh(void)
{
    s.count = build_entries();
    if (s.view == FD_LIST) build_list();
    else track_update();
}

static void toggle_save(const uint8_t addr[6])
{
    int ei = focus_of_addr(addr);
    if (ei < 0) return;

    if (s.entries[ei].saved) {
        if (app_ble_finder_unsave(addr)) {
            ui_hint_flash("已从我的设备移除", 1400);
        }
    } else {
        const char *nm = s.entries[ei].name[0] ? s.entries[ei].name : NULL;
        if (app_ble_finder_save(addr, nm) < 0) {
            ui_hint_flash("我的设备最多存 8 个", 1800);
            return;
        }
        ui_hint_flash("已存为我的设备", 1400);
    }
    refresh();   // 立刻重排：收藏项要挪进"我的设备"区，用户马上看到结果
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_finder_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.view = FD_LIST;
    s.page = ui_page_create(NULL);

    app_ble_finder_request_start();
    refresh();
    ui_hint_flash("正在打开蓝牙…", 1200);
}

void page_finder_exit(void)
{
    if (!s.active && !s.page.scr) return;
    // 先请求停扫描，再删屏：擦掉屏幕后就不该再有蓝牙角色在跑。
    app_ble_finder_request_stop();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_finder_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 长按 OK：任何视图下都直接退出本页，交给上层工具页重建列表。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        page_finder_exit();
        return;
    }

    if (s.view == FD_TRACK) {
        if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            if (s.count <= 0) return;
            int ti = focus_of_addr(s.track_addr);
            if (ti < 0) ti = 0;
            ti = (ti + (btn == BSP_BTN_UP ? s.count - 1 : 1)) % s.count;
            memcpy(s.track_addr, s.entries[ti].addr, 6);
            build_track();
            return;
        }
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            toggle_save(s.track_addr);
            return;
        }
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            s.view = FD_LIST;
            refresh();
            return;
        }
        return;
    }

    // ---- 列表视图 ----
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        // 与提示一致：蓝牙没打开时长按↑ 是"重试"（此时扫描没跑，没有可收藏的项）；
        // 否则才是本页正常的"存/取消收藏"。
        if (app_ble_finder_last_error() != ESP_OK) {
            app_ble_finder_request_start();
            ui_hint_flash("正在重试打开蓝牙…", 1400);
            return;
        }
        if (s.focus < 0) {
            ui_hint_flash("还没有设备可收藏", 1500);
            return;
        }
        toggle_save(s.sel_addr);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (s.count <= 0) return;
        s.focus = (s.focus + (btn == BSP_BTN_UP ? s.count - 1 : 1)) % s.count;
        memcpy(s.sel_addr, s.entries[s.focus].addr, 6);
        s.sel_valid = true;
        render_focus();
    } else if (btn == BSP_BTN_OK) {
        if (s.focus < 0) {
            ui_hint_flash("还没有搜到设备", 1500);
            return;
        }
        memcpy(s.track_addr, s.sel_addr, 6);
        s.view = FD_TRACK;
        build_track();
    }
}

void page_finder_tick(void)
{
    if (!s.active || !s.page.scr) return;
    uint32_t now = mono_ms();
    if (now - s.last_ms < FD_REFRESH_MS) return;
    s.last_ms = now;
    refresh();
}

bool page_finder_active(void)
{
    return s.active;
}