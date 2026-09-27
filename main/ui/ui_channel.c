// main/ui/ui_channel.c —— 工具页子页：信道体检（Wi-Fi 信道扫描）。
//
// 普通用户看不懂"信道 6 上有 7 个 AP、RSSI -52"，只想知道"我家 Wi-Fi 为什么卡、要不要
// 动路由器"。所以默认这一屏只给结论：拥挤度、一句人话、建议改到哪个信道，并用 ↑↓ 在
// "建议信道"与"热点总览"两个条目间挪焦点，OK 进当前焦点对应的视图。往里有三条路：
//   · 逐信道：按拥挤度从低到高列出 13 条信道（最空的排最前），顶部横幅给一句大白话
//     环境结论（同频/邻频干扰来自哪条信道、建议改到哪条）；某条信道上再按 OK 看该信道
//     的热点（SSID、安全模式、信号强度）。
//   · 热点总览：本次扫描到的全部 AP，跨信道按信号从强到弱一次列完，每行同样带安全模式。
// 三级浏览共用三键：OK 进入 / 返回，长按 OK 逐级退回结论屏，长按 ↑ 重扫。
//
// 扫描由 net/app_net 在内部 worker 完成，本页只读状态与报告，绝不在按键回调里等。
//
// 射频互斥：蓝牙（找设备 / 万能遥控）正占用 2.4G 时，app_net 会直接拒绝扫描并把原因
// 放在 app_net_channel_error() 里。本页照原样显示，且不再写"正在扫描"——否则用户会
// 对着一个永远不会来的结果干等。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"

#include "net/app_ble.h"
#include "net/app_net.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define CH_CW (UI_W - 2 * UI_MARGIN_X)

typedef enum {
    CH_MAIN = 0,   // 结论（焦点可在"建议信道"与"热点总览"两个条目间移动）
    CH_DETAIL,     // 逐信道（按拥挤度排序）
    CH_APS,        // 某信道上的热点明细
    CH_INVENTORY,  // 热点总览：本次扫描到的全部 AP，按信号从强到弱
} ch_view_t;

static struct {
    bool      active;
    ui_page_t page;
    ch_view_t view;

    app_channel_report_t report;   // 本页自留副本：不直接遍历网络层正在写的结构
    bool                 have_report;
    app_fetch_state_t    last_state;
    bool                 retry_pending;   // 被"蓝牙拆栈中"顶掉，等射频空出后自动补扫

    // MAIN：结论条目下标（0=建议信道，1=热点总览）；DETAIL：order 位置；
    // APS/INVENTORY：热点下标。四种视图同一时刻只有一个在屏，复用同一个焦点变量。
    int                  focus;
    int                  aps_channel;     // APS 视图对应的信道
    int                  ap_idx[APP_CHANNEL_MAX_APS];
    int                  ap_count;
    int                  inv_idx[APP_CHANNEL_MAX_APS];   // 热点总览的全部热点下标
    int                  inv_count;
    ui_row_t             rows[APP_CHANNEL_MAX_APS];   // 明细最多 13、热点最多 48，取大者
} s;

static const char *state_text(void)
{
    switch (app_net_channel_scan_state()) {
    case APP_FETCH_RUNNING: return "扫描中…";
    case APP_FETCH_OK:      return "已扫描";
    case APP_FETCH_FAILED:  return "扫描失败";
    default:                return "准备中";
    }
}

static uint32_t congestion_color(int c)
{
    if (c < 20) return ui_c_ok();
    if (c < 45) return ui_c_accent();
    if (c < 70) return ui_c_warn();
    return ui_c_live();
}

static uint32_t score_color(int score)
{
    if (score <= 0) return ui_c_dim();
    if (score < 30) return ui_c_ok();
    if (score < 70) return ui_c_warn();
    return ui_c_live();
}

// RSSI 强弱配色：越强越"好"（绿），越弱越"远"（灰）。
static uint32_t rssi_color(int rssi)
{
    if (rssi >= -55) return ui_c_ok();
    if (rssi >= -70) return ui_c_accent();
    if (rssi >= -85) return ui_c_warn();
    return ui_c_dim();
}

static void refresh_report(void)
{
    const app_channel_report_t *r = app_net_channel_report();
    if (!r) return;
    memcpy(&s.report, r, sizeof(s.report));
    s.have_report = true;
}

// ---------------------------------------------------------------------------
// 主视图：只讲结论
// ---------------------------------------------------------------------------

// 结论屏只有两个条目，焦点用行自身的选中条表达：用户一眼能看出按 OK 会进哪一个。
static void main_render_focus(void)
{
    ui_row_set_selected(s.rows[0], s.focus == 0);
    ui_row_set_selected(s.rows[1], s.focus == 1);
}

static void build_main(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));

    ui_header_create(c, "信道体检", state_text(), NULL, NULL);

    if (app_net_channel_scan_state() == APP_FETCH_FAILED) {
        const char *err = app_net_channel_error();
        ui_banner_create(c, err ? err : "扫描失败，请稍后重试",
                         ui_c_warn());
    }

    if (!s.have_report) {
        if (app_net_channel_scan_state() == APP_FETCH_FAILED) {
            // 失败（最常见是被蓝牙占用）时不能再写"正在扫描"，否则用户会一直干等一个
            // 永远不会来的结果。原因就在上方横幅里。
            ui_empty_create(c, "没能开始扫描",
                            "上方黄色提示写明了原因，照它处理后长按 ↑ 重试。");
            ui_page_set_hint("长按↑ 重试扫描   长按OK 返回");
        } else {
            ui_empty_create(c, "正在扫描附近的 Wi-Fi",
                            "大约需要几秒。扫完会告诉你 2.4G 挤不挤、路由器该用哪个信道。");
            ui_page_set_hint("长按↑ 重新扫描   长按OK 返回");
        }
        return;
    }

    int cong = s.report.congestion;
    uint32_t col = congestion_color(cong);

    lv_obj_t *card = ui_card_create(c, 0, 0, CH_CW, 112, col);
    lv_obj_t *ring = ui_ring_create(card, 72, 16, 20, col);
    ui_ring_set(ring, cong * 10, col);
    lv_obj_t *num = ui_label_create(card, NULL, ui_font_display_s, col);
    lv_label_set_text_fmt(num, "%d", cong);
    lv_obj_align_to(num, ring, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *cap = ui_label_create(card, "2.4G 拥挤度", ui_font_hint, ui_c_dim());
    lv_obj_set_pos(cap, 100, 26);
    lv_obj_t *verdict = ui_label_create(card, app_channel_verdict(cong), ui_font_title, col);
    lv_obj_set_pos(verdict, 100, 48);

    char advice[80];
    app_channel_advice(&s.report, advice, sizeof(advice));
    ui_banner_create(c, advice, col);

    lv_obj_t *list = ui_list_create(c);

    // 64 而不是 32：下面"热点总览"那行要塞下 "N 个热点 · M 开放" 两个整数加中文和间隔点，
    // 编译器按 int 最宽 10 位起算能到 40 多字节，32 会触发 -Werror=format-truncation。
    char val[64];
    // 条目 0：推荐信道。按 OK 进入"逐信道"视图（先看每条信道挤不挤）。
    s.rows[0] = ui_row_create(list, "建议信道", NULL);
    snprintf(val, sizeof(val), "%d 信道", s.report.best_channel);
    ui_row_set_value(s.rows[0], val);
    if (s.rows[0].value) {
        lv_obj_set_style_text_color(s.rows[0].value, lv_color_hex(ui_c_accent()), 0);
    }

    // 条目 1：热点总览。按 OK 一次看完本次扫到的全部 AP。右侧顺带给出"开放网络"台数：
    // 这只是对信标里公开字段的只读统计（开放=无密码），既不连接也不探测，但用户顺手
    // 就能知道周围有没有不安全的网络。
    s.rows[1] = ui_row_create(list, "热点总览", NULL);
    int open_n = 0, wep_n = 0;
    app_channel_security_counts(&s.report, &open_n, &wep_n, NULL);
    if (open_n > 0) {
        snprintf(val, sizeof(val), "%d 个热点 · %d 开放", s.report.ap_total, open_n);
    } else if (wep_n > 0) {
        snprintf(val, sizeof(val), "%d 个热点 · %d 个 WEP", s.report.ap_total, wep_n);
    } else {
        snprintf(val, sizeof(val), "%d 个热点", s.report.ap_total);
    }
    ui_row_set_value(s.rows[1], val);

    if (s.focus < 0 || s.focus > 1) s.focus = 0;   // 从明细/总览返回时把焦点收回第一个条目
    main_render_focus();
    ui_page_set_hint("↑↓ 选条目   OK 进入   长按↑ 重扫   长按OK 返回");
}

// ---------------------------------------------------------------------------
// 明细视图：逐信道（按拥挤度从低到高，即"最空的排最前"）
// ---------------------------------------------------------------------------

static void detail_render_focus(void)
{
    for (int i = 0; i < APP_CHANNEL_COUNT; i++) ui_row_set_selected(s.rows[i], i == s.focus);
    if (s.focus >= 0 && s.focus < APP_CHANNEL_COUNT) ui_scroll_into_view(s.rows[s.focus].obj);
}

static void build_detail(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));

    char right[24];
    snprintf(right, sizeof(right), "共 %d 个热点", s.report.ap_total);
    ui_header_create(c, "每信道明细", right, NULL, NULL);
    // 顶部横幅给一句大白话环境结论：不只说"该换哪条信道"，还说清"干扰是从哪条信道
    // 漏过来的"，普通用户看完能理解为什么要换。下面按拥挤度排序，最空的排最前。
    char env[128];
    app_channel_env_summary(&s.report, env, sizeof(env));
    ui_banner_create(c, env, ui_c_accent());

    lv_obj_t *list = ui_list_create(c);
    for (int pos = 0; pos < APP_CHANNEL_COUNT; pos++) {
        int ch = app_channel_order_channel(&s.report, pos);
        const app_channel_slot_t *slot = &s.report.ch[ch - APP_CHANNEL_MIN];

        char title[16];
        snprintf(title, sizeof(title), "信道 %d", ch);
        char val[28];
        if (slot->aps <= 0) {
            snprintf(val, sizeof(val), "空闲");
        } else {
            snprintf(val, sizeof(val), "%d 个 · %d%%", slot->aps, slot->score);
        }
        s.rows[pos] = ui_row_create(list, title, val);
        ui_row_set_title_color(s.rows[pos], score_color(slot->score));
    }

    if (s.focus < 0) s.focus = 0;
    if (s.focus >= APP_CHANNEL_COUNT) s.focus = APP_CHANNEL_COUNT - 1;
    detail_render_focus();
    ui_page_set_hint("↑↓ 选信道   OK 看该信道热点   长按OK 返回结论");
}

// ---------------------------------------------------------------------------
// 热点视图：某条信道上有哪些热点
// ---------------------------------------------------------------------------

static void aps_render_focus(void)
{
    for (int i = 0; i < s.ap_count; i++) ui_row_set_selected(s.rows[i], i == s.focus);
    if (s.focus >= 0 && s.focus < s.ap_count) ui_scroll_into_view(s.rows[s.focus].obj);
}

static void build_aps(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));

    s.ap_count = app_channel_aps_on(&s.report, s.aps_channel, s.ap_idx,
                                    APP_CHANNEL_MAX_APS);

    char title[24];
    snprintf(title, sizeof(title), "信道 %d 的热点", s.aps_channel);
    char right[24];
    snprintf(right, sizeof(right), "共 %d 个", s.ap_count);
    ui_header_create(c, title, right, NULL, NULL);

    if (s.ap_count <= 0) {
        ui_empty_create(c, "这条信道上没有热点",
                        "说明它很空，适合把路由器改到这里。按 OK 返回信道列表。");
        ui_page_set_hint("OK/长按OK 返回");
        return;
    }

    ui_banner_create(c, "按信号从强到弱排列；越靠上说明离你越近", ui_c_accent());

    lv_obj_t *list = ui_list_create(c);
    for (int i = 0; i < s.ap_count; i++) {
        const app_channel_ap_t *ap = &s.report.aps[s.ap_idx[i]];
        const char *name = ap->ssid[0] ? ap->ssid : "隐藏网络";
        // 右侧值带上安全模式（"WPA2 · -52 dBm"）：用户顺带能看出哪台是开放网络，
        // 不用再点进去问。缓冲区按最长组合（中文"未知"+ dBm 三位数）留够。
        char val[24];
        snprintf(val, sizeof(val), "%s · %d dBm",
                 app_channel_sec_text(ap->sec), (int)ap->rssi);
        s.rows[i] = ui_row_create(list, name, val);
        ui_row_set_title_color(s.rows[i], rssi_color(ap->rssi));
    }

    if (s.focus < 0) s.focus = 0;
    if (s.focus >= s.ap_count) s.focus = s.ap_count - 1;
    aps_render_focus();
    ui_page_set_hint("↑↓ 查看热点   OK/长按OK 返回信道列表");
}

static void aps_open(int channel)
{
    s.aps_channel = channel;
    s.view = CH_APS;
    s.focus = 0;
    build_aps();
}

// ---------------------------------------------------------------------------
// 热点总览：本次扫描到的全部 AP，跨信道按信号从强到弱一次列完
// ---------------------------------------------------------------------------

static void inventory_render_focus(void)
{
    for (int i = 0; i < s.inv_count; i++) ui_row_set_selected(s.rows[i], i == s.focus);
    if (s.focus >= 0 && s.focus < s.inv_count) ui_scroll_into_view(s.rows[s.focus].obj);
}

static void build_inventory(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));

    s.inv_count = app_channel_aps_sorted(&s.report, s.inv_idx, APP_CHANNEL_MAX_APS);

    char right[24];
    snprintf(right, sizeof(right), "共 %d 个", s.inv_count);
    ui_header_create(c, "热点总览", right, NULL, NULL);

    if (s.inv_count <= 0) {
        ui_empty_create(c, "这次没扫到热点",
                        "附近可能没有 2.4G 路由器，或不在覆盖范围内。按 OK 或长按 OK 返回结论。");
        ui_page_set_hint("OK/长按OK 返回");
        return;
    }

    // 频段如实标注：本机是单射频 2.4G，扫不到 5G，别让用户以为漏扫了。
    char tip[64];
    snprintf(tip, sizeof(tip), "按信号从强到弱排列；本机只扫到 %s",
             app_channel_band_text());
    ui_banner_create(c, tip, ui_c_accent());

    lv_obj_t *list = ui_list_create(c);
    for (int i = 0; i < s.inv_count; i++) {
        const app_channel_ap_t *ap = &s.report.aps[s.inv_idx[i]];
        const char *name = ap->ssid[0] ? ap->ssid : "隐藏网络";
        char val[24];
        snprintf(val, sizeof(val), "%s · %d dBm",
                 app_channel_sec_text(ap->sec), (int)ap->rssi);
        s.rows[i] = ui_row_create(list, name, val);
        ui_row_set_title_color(s.rows[i], rssi_color(ap->rssi));
    }

    if (s.focus < 0) s.focus = 0;
    if (s.focus >= s.inv_count) s.focus = s.inv_count - 1;
    inventory_render_focus();
    ui_page_set_hint("↑↓ 查看热点   OK/长按OK 返回结论");
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

// 发起一次扫描，并判断这次拒绝是不是"蓝牙正在拆栈"造成的：蓝牙退出是异步的（拆协议栈
// 要几百毫秒），用户刚离开找设备/遥控就进本页时，射频其实马上就空了。这种情况记下来，
// 由 tick 在射频真正空闲后自动补扫一次——否则用户明明已经退出蓝牙，却被要求"先退出"。
static void request_scan(void)
{
    bool ble_busy = app_ble_active();
    app_net_channel_scan_request();
    s.last_state = app_net_channel_scan_state();
    s.retry_pending = (s.last_state == APP_FETCH_FAILED && ble_busy);
}

void page_channel_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.view = CH_MAIN;
    s.page = ui_page_create(NULL);

    request_scan();
    refresh_report();
    build_main();
    // 被拒绝时（蓝牙占用中）原因已经写在页内横幅与空态里，不再叠一句"正在扫描"误导。
    if (s.last_state != APP_FETCH_FAILED) ui_hint_flash("正在扫描，请稍候…", 1500);
}

void page_channel_exit(void)
{
    if (!s.active && !s.page.scr) return;
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_channel_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    if (s.view == CH_INVENTORY) {
        // OK 或长按 OK 都返回结论屏：这是只读一览，没有更深一层可进。
        if ((ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) && btn == BSP_BTN_OK) {
            s.view = CH_MAIN;
            s.focus = 0;
            build_main();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) {
            if (s.inv_count > 0) {
                s.focus = (s.focus + s.inv_count - 1) % s.inv_count;
                inventory_render_focus();
            }
        } else if (btn == BSP_BTN_DOWN) {
            if (s.inv_count > 0) {
                s.focus = (s.focus + 1) % s.inv_count;
                inventory_render_focus();
            }
        }
        return;
    }

    if (s.view == CH_APS) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            s.view = CH_DETAIL;
            build_detail();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) {
            if (s.ap_count > 0) {
                s.focus = (s.focus + s.ap_count - 1) % s.ap_count;
                aps_render_focus();
            }
        } else if (btn == BSP_BTN_DOWN) {
            if (s.ap_count > 0) {
                s.focus = (s.focus + 1) % s.ap_count;
                aps_render_focus();
            }
        } else if (btn == BSP_BTN_OK) {
            s.view = CH_DETAIL;
            build_detail();
        }
        return;
    }

    if (s.view == CH_DETAIL) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            s.view = CH_MAIN;
            s.focus = 0;
            build_main();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) {
            s.focus = (s.focus + APP_CHANNEL_COUNT - 1) % APP_CHANNEL_COUNT;
            detail_render_focus();
        } else if (btn == BSP_BTN_DOWN) {
            s.focus = (s.focus + 1) % APP_CHANNEL_COUNT;
            detail_render_focus();
        } else if (btn == BSP_BTN_OK) {
            aps_open(app_channel_order_channel(&s.report, s.focus));
        }
        return;
    }

    // ---- 主视图 ----
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        page_channel_exit();
        return;
    }
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        request_scan();
        if (s.last_state == APP_FETCH_FAILED) {
            build_main();   // 立刻把"被蓝牙占用"的原因显示出来，不让用户干等
            ui_hint_flash("未能扫描：蓝牙正占用射频", 1800);
        } else {
            ui_hint_flash("正在重新扫描…", 1500);
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    // 结论屏两个条目：↑↓ 挪焦点，OK 进当前焦点对应的视图。
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (!s.have_report) {
            ui_hint_flash("还在扫描，请稍候", 1500);
            return;
        }
        s.focus = s.focus == 0 ? 1 : 0;
        main_render_focus();
        return;
    }
    if (btn == BSP_BTN_OK) {
        if (!s.have_report) {
            ui_hint_flash("还在扫描，请稍候", 1500);
            return;
        }
        if (s.focus == 1) {
            s.view = CH_INVENTORY;
            s.focus = 0;
            build_inventory();
        } else {
            s.view = CH_DETAIL;
            s.focus = 0;
            build_detail();
        }
    }
}

void page_channel_tick(void)
{
    if (!s.active || !s.page.scr) return;

    // 蓝牙拆栈要几百毫秒：若这次请求是被"蓝牙还没退干净"顶掉的，等射频真正空出来
    // 自动补扫一次，避免用户明明已退出蓝牙却被要求"先退出"。
    if (s.retry_pending && !app_ble_active()) {
        s.retry_pending = false;
        app_net_channel_scan_request();
        s.last_state = app_net_channel_scan_state();
        if (s.view == CH_MAIN) build_main();
        return;
    }

    app_fetch_state_t st = app_net_channel_scan_state();
    if (st == s.last_state) return;
    s.last_state = st;

    if (st == APP_FETCH_OK) refresh_report();
    // 只重画结论屏；明细/热点屏是静态快照，等用户返回时再看新结果。
    if (s.view == CH_MAIN) build_main();
}

bool page_channel_active(void)
{
    return s.active;
}
