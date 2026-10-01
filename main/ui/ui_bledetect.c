// main/ui/ui_bledetect.c —— 工具页子页：BLE 检测（被动侦测，绝不发射）。
//
// 把一个"只听不发射"的蓝牙侦测页包成普通用户能看懂的界面。它复用"找设备"已经搭好的
// 观察者扫描（net/app_ble 的 finder 角色）来收广播，自己只做分类并显示结论——本页没有任何
// 广播、欺骗、断开连接或任何主动无线电动作，符合"被动/防御"定位。
//
// 与"找设备"页同样的非阻塞约定：开启/停止扫描走 app_ble 的异步请求，本页只读快照、发请求，
// 绝不碰 NimBLE。每个节拍 poll 一份 finder 快照，把每台设备喂给纯逻辑分类器
// （logic/app_bledetect），再把报告渲染成"判定 + 计数 + 小列表"。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "net/app_ble.h"
#include "logic/app_bledetect.h"
#include "logic/app_finder.h"

#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define BD_CW     (UI_W - 2 * UI_MARGIN_X)
#define BD_TICK_MS 800

static uint32_t mono_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static struct {
    bool      active;
    ui_page_t page;

    app_finder_t snap;        // 最近的 finder 快照，喂分类器用（约 1KB，放静态区）

    lv_obj_t *verdict_lbl;    // 主导判定（大字）
    lv_obj_t *counts_lbl;     // 计数行
    ui_row_t  rows[APP_BLEDETECT_EG_MAX];  // 样例地址列表行

    uint32_t  last_ms;
} s;

static void feed_classifier(void)
{
    uint64_t now = (uint64_t)mono_ms();
    for (int i = 0; i < s.snap.count; i++) {
        const app_finder_dev_t *d = &s.snap.devs[i];
        app_bledetect_ad_t ad;
        memset(&ad, 0, sizeof(ad));
        ad.addr       = d->addr;
        ad.rssi       = d->rssi;
        ad.now_ms     = now;
        ad.company_id = d->company_id;
        ad.mfg_type   = d->mfg_type;
        ad.mfg_data   = d->mfg_data;
        ad.mfg_data_len = d->mfg_data_len;
        ad.svc16      = d->svc16;
        ad.svc16_count = d->svc16_count;
        if (d->has_name) {
            ad.name = d->name;
            ad.name_len = (int)strlen(d->name);
        }
        app_bledetect_feed(&ad);
    }
}

static void build_page(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.verdict_lbl = NULL;
    s.counts_lbl = NULL;

    ui_header_create(c, "BLE 检测", NULL, NULL, NULL);

    // 被动声明：本页不发射任何广播，放在最显眼处，避免被误认为攻击工具。
    ui_banner_create(c, "被动监听，不发送任何广播", ui_c_ok());

    // 先收最新快照并喂分类器，再渲染报告——保证判定与当前屏幕同步。
    app_ble_finder_snapshot(&s.snap);
    feed_classifier();

    app_bledetect_report_t rep;
    app_bledetect_report(&rep);

    uint32_t vcol = (rep.verdict == APP_BLEDETECT_SPAM)   ? ui_c_warn()
                  : (rep.verdict == APP_BLEDETECT_AIRTAG) ? ui_c_accent()
                  : ui_c_ok();
    s.verdict_lbl = ui_label_create(c, app_bledetect_verdict_text(rep.verdict),
                                    ui_font_title, vcol);
    if (s.verdict_lbl) {
        lv_obj_set_width(s.verdict_lbl, BD_CW);
        lv_label_set_long_mode(s.verdict_lbl, LV_LABEL_LONG_WRAP);
    }

    char counts[128];
    snprintf(counts, sizeof(counts),
             "广播源 %d · 疑似轰炸 %d · AirTag %d · 其它 %d",
             rep.total_advertisers, rep.apple_continuity_spam,
             rep.airtag, rep.other);
    s.counts_lbl = ui_label_create(c, counts, ui_font_hint, ui_c_dim());
    if (s.counts_lbl) lv_obj_set_width(s.counts_lbl, BD_CW);

    // 小列表：展示几条样例地址，让"判定"落到具体设备上（地址会随机化，只代表这一次广播）。
    if (rep.example_count > 0) {
        lv_obj_t *list = ui_list_create(c);
        for (int i = 0; i < rep.example_count; i++) {
            const uint8_t *ad = rep.eg_addr[i];
            char addr[40];
            snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ad[0], ad[1], ad[2], ad[3], ad[4], ad[5]);
            s.rows[i] = ui_row_create(list, addr, app_bledetect_kind_text((app_bledetect_kind_t)rep.eg_kind[i]));
            uint32_t col = (rep.eg_kind[i] == APP_BLEDETECT_KIND_AIRTAG) ? ui_c_accent()
                        : (rep.eg_kind[i] == APP_BLEDETECT_KIND_SPAM)   ? ui_c_warn()
                        : ui_c_dim();
            if (s.rows[i].value) lv_obj_set_style_text_color(s.rows[i].value, lv_color_hex(col), 0);
        }
    } else {
        ui_empty_create(c, "附近没有侦测到广播",
                        "打开耳机盒盖、让手机进入蓝牙设置，或走到有人群的地方，这里会出现设备。");
    }

    // 提示与长按 OK 的"返回"严格对应；监听中可短按 OK 暂停（再按恢复）。
    if (app_ble_finder_last_error() != ESP_OK) {
        ui_page_set_hint("长按OK 返回工具页");
    } else if (app_ble_finder_running()) {
        ui_page_set_hint("OK 暂停监听   长按OK 返回工具页");
    } else {
        ui_page_set_hint("OK 开始监听   长按OK 返回工具页");
    }
}

static void refresh(void)
{
    build_page();
}

static void toggle_scan(void)
{
    if (app_ble_finder_running()) {
        app_ble_finder_request_stop();
        ui_hint_flash("已暂停监听", 1000);
    } else {
        app_ble_finder_request_start();
        ui_hint_flash("正在打开蓝牙…", 1200);
    }
    build_page();
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------
void page_bledetect_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.page = ui_page_create(NULL);

    // 从干净状态开始：不留上次进页的观察记录。
    app_bledetect_reset();
    // 复用 finder 的被动扫描（异步请求，避免拉起协议栈卡住 LVGL）。
    app_ble_finder_request_start();
    build_page();
    ui_hint_flash("正在打开蓝牙…", 1200);
}

void page_bledetect_exit(void)
{
    if (!s.active && !s.page.scr) return;
    // 先请求停扫描，再删屏：屏没了就不该还有蓝牙角色在跑。
    app_ble_finder_request_stop();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_bledetect_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 长按 OK：任何情况下都返回工具页（交给上层重建列表）。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        page_bledetect_exit();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    // 短按 OK：监听中暂停、未监听则开始（不做任何发射，只控制接收）。
    if (btn == BSP_BTN_OK) {
        if (app_ble_finder_last_error() != ESP_OK) {
            ui_hint_flash("蓝牙没能打开，请稍后重试", 1400);
        } else {
            toggle_scan();
        }
    }
}

void page_bledetect_tick(void)
{
    if (!s.active || !s.page.scr) return;
    uint32_t now = mono_ms();
    if (now - s.last_ms < BD_TICK_MS) return;
    s.last_ms = now;
    refresh();
}

bool page_bledetect_active(void)
{
    return s.active;
}
