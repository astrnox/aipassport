// main/ui/ui_wifilab.c —— 工具页子页：Wi-Fi 实验（Wi-Fi lab / CTF）。
//
// 把 net/app_net 的"Wi-Fi 实验"角色包成普通人能用的页面。它只是把 logic/app_wifilab 算好的
// 原始 802.11 帧通过射频发出去，本身不决定"对谁发"——危险来自"对谁发"。所以本页在发出
// 任何一字节之前，必须先过一道授权/法律告知，用户按 OK 明确确认后才进入实验室；退出页面
// 授权即作废，下次进来重新确认（与 ui_blelab.c 的授权门槛一致）。
//
// 与"信道体检""BLE 实验"同样的非阻塞约定：开启/停止都走 app_net 的异步请求，绝不在按键
// 回调里直接操作射频（那会卡住 LVGL）。本页只读状态、发请求、展示结果，不碰 esp_wifi。
//
// 注意：本页只面向"自有设备与已授权环境"的 CTF / 实验室用途。对他人或你无权操作的网络
// 发射，可能违反当地无线电管理法规与平台使用条款，后果由操作者自行承担。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "net/app_net.h"
#include "logic/app_wifilab.h"

#include "esp_timer.h"
#include "lvgl.h"

#include <string.h>

#define WL_CW   (UI_W - 2 * UI_MARGIN_X)
#define WL_TICK_MS 500

// 四种模式的中文名（与 app_wifilab_mode_t 顺序一致）。
static const char *const MODE_NAMES[APP_WIFILAB_MODE_COUNT] = {
    [APP_WIFILAB_DEAUTH]       = "解除认证洪泛",
    [APP_WIFILAB_EAPOL_LOGOFF] = "EAPOL 下线洪泛",
    [APP_WIFILAB_SAE_FLOOD]    = "SAE 握手洪泛",
    [APP_WIFILAB_BEACON_SPAM]  = "信标帧洪泛",
};

// 信标子模式的中文名。
static const char *const BEACON_NAMES[APP_WIFILAB_BEACON_COUNT] = {
    [APP_WIFILAB_BEACON_RANDOM]    = "随机 SSID",
    [APP_WIFILAB_BEACON_RICKROLL]  = "Rickroll 恶搞",
    [APP_WIFILAB_BEACON_AP_LIST]   = "扫描到的 AP",
};

typedef enum {
    WL_CONSENT = 0,   // 授权/法律告知页，未确认前禁止发射
    WL_LAB,           // 实验室：选模式 + 选目标/信标类型 + 开始/停止
    WL_TARGET,        // 选目标：展示最近一次扫描到的 AP 列表
} wl_view_t;

// 选中的目标 AP（DEAUTH/EAPOL/SAE 用；BEACON 的 AP_LIST 子模式直接复用扫描结果）。
typedef struct {
    uint8_t bssid[6];
    int     channel;
    char    ssid[33];
    bool    valid;
} wl_target_t;

static struct {
    bool        active;
    ui_page_t   page;
    wl_view_t   view;

    bool        consent;            // 本次进页是否已授权
    int         focus;              // 实验室/目标列表的选中下标
    int         mode_focus;         // 选中的模式下标 [0, APP_WIFILAB_MODE_COUNT)
    app_wifilab_beacon_t beacon_sub;// BEACON_SPAM 的子模式
    wl_target_t target;             // 当前选中的目标 AP

    bool        scanning;           // 正在等一次扫描结果（目标视图里）
    ui_row_t    rows[8];            // 实验室最多 6 行 + 目标列表复用
    lv_obj_t   *status_lbl;

    uint32_t    last_ms;
    int         lab_sig;            // 上次构建实验室页时的状态指纹，用于避免每拍重建
} s;

// 当前选中模式（mode_focus 越界时退回 0）。
static app_wifilab_mode_t cur_mode(void)
{
    if (s.mode_focus < 0 || s.mode_focus >= APP_WIFILAB_MODE_COUNT) return APP_WIFILAB_DEAUTH;
    return (app_wifilab_mode_t)s.mode_focus;
}

// 内容上下滚动。三键设备没有触摸和手势，长按 ↑↓ 是唯一的滚动途径。
static void scroll_content(int dy)
{
    if (s.page.content) lv_obj_scroll_by(s.page.content, 0, dy, LV_ANIM_OFF);
}

// 实验室页状态指纹：只包含"会改变页面结构/文字"的状态。已发帧数每拍都在变，
// 故意不计入——它只更新状态行文本。若把它算进来，页面会每 500ms 重建一次，
// 滚动位置与高亮被反复重置，用户就会觉得"按键滚不动、页面自己弹回去"。
static int lab_sig(void)
{
    bool failed = app_net_wifilab_last_error() != ESP_OK;
    bool running = app_net_wifilab_running() && !failed;
    return (failed ? 1 : 0) | (running ? 2 : 0)
           | (s.mode_focus << 2) | (s.beacon_sub << 5)
           | ((s.target.valid ? 1 : 0) << 8) | ((s.target.channel & 0x1F) << 9)
           | (s.focus << 14);
}

// 只更新"状态：发射中 / 已发 N 帧"这一行，不重建页面。
static void update_status(void)
{
    if (!s.status_lbl) return;
    char stat[96];
    if (app_net_wifilab_last_error() != ESP_OK) {
        snprintf(stat, sizeof(stat), "状态：未开启");
    } else if (app_net_wifilab_running()) {
        // "已发"只统计驱动接受的帧。有帧被拒时同时显示失败数与首个错误码：设备上拿不到
        // 串口日志时，这行字就是唯一能说清"为什么没有效果"的地方。
        unsigned sent = (unsigned)app_net_wifilab_packets_sent();
        unsigned failed = (unsigned)app_net_wifilab_packets_failed();
        const char *txerr = app_net_wifilab_tx_error_text();
        if (failed == 0) {
            snprintf(stat, sizeof(stat), "状态：发射中  已发 %u 帧", sent);
        } else if (txerr) {
            snprintf(stat, sizeof(stat), "状态：发射中  已发 %u 失败 %u（%s）",
                     sent, failed, txerr);
        } else {
            snprintf(stat, sizeof(stat), "状态：发射中  已发 %u 失败 %u", sent, failed);
        }
    } else {
        snprintf(stat, sizeof(stat), "状态：已停止");
    }
    lv_label_set_text(s.status_lbl, stat);
}

// 目标是否需要（DEAUTH/EAPOL/SAE 需要选一个 AP；BEACON 不需要）。
static bool mode_needs_target(app_wifilab_mode_t m)
{
    return m != APP_WIFILAB_BEACON_SPAM;
}

// 选目标页最多展示的 AP 行数（剩余一行给"重新扫描"）。
#define WL_MAX_APS 7

// 选目标页的实际行数（含"重新扫描"行），与 build_target 一致，供按键导航取模。
static int wl_target_row_count(void)
{
    const app_channel_report_t *r = app_net_channel_report();
    int aps = (r && r->stored > 0) ? (r->stored < WL_MAX_APS ? r->stored : WL_MAX_APS) : 0;
    // 无数据时仍展示"重新扫描" + 一个"暂无结果"提示行。
    return (aps > 0 ? aps : 0) + 1 + (aps == 0 ? 1 : 0);
}

// 前向声明：实验室页的 refresh() 会用到。
static void build_target(void);


// ---------------------------------------------------------------------------
// 授权/法律告知页
// ---------------------------------------------------------------------------
static void build_consent(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.status_lbl = NULL;

    ui_header_create(c, "Wi-Fi 实验", NULL, NULL, NULL);

    ui_banner_create(c, "仅限自有设备与授权环境用于学习研究", ui_c_warn());

    const char *body =
        "本功能会向周围发射特制的 Wi-Fi 报文（解除认证 / EAPOL 下线 / SAE 握手 / 信标洪泛等）。"
        "这些报文可能让附近设备断线或误以为出现大量同名热点。\n\n"
        "对他人或你无权操作的 Wi-Fi 网络发射，可能违反当地无线电管理法规与网络使用条款，"
        "并可能被用于攻击——这是违法行为。继续即表示你确认：仅在自己拥有或已获书面授权的网络、"
        "于合法授权的 CTF / 实验环境内做学习研究，并自行承担由此产生的一切后果。";
    lv_obj_t *lbl = ui_label_create(c, body, ui_font_body, ui_c_text());
    if (lbl) {
        lv_obj_set_width(lbl, WL_CW);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    }

    // 授权说明比一屏长，且此视图没有可选行：短按 ↑↓ 用来上下阅读。
    ui_page_set_hint("↑↓ 上下阅读  OK 授权  长按OK 返回工具页");
    if (s.page.content) lv_obj_scroll_to_y(s.page.content, 0, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// 实验室页
// ---------------------------------------------------------------------------
static void build_lab(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.status_lbl = NULL;

    ui_header_create(c, "Wi-Fi 实验", NULL, NULL, NULL);

    bool failed = app_net_wifilab_last_error() != ESP_OK;
    if (failed) {
        const char *why = app_net_wifilab_error_text();
        ui_banner_create(c, why ? why : "没能开始，请稍后重试", ui_c_warn());
    }

    lv_obj_t *list = ui_list_create(c);
    // 模式列表（4 行）。
    for (int i = 0; i < APP_WIFILAB_MODE_COUNT; i++) {
        const char *val = "";
        if (!failed && app_net_wifilab_running() && (app_wifilab_mode_t)i == cur_mode()) {
            val = "发射中";
        }
        s.rows[i] = ui_row_create(list, MODE_NAMES[i], val);
    }

    int n = APP_WIFILAB_MODE_COUNT;
    if (mode_needs_target(cur_mode())) {
        // 目标行：点 OK 进入扫描选 AP。
        char buf[80];
        if (s.target.valid) {
            snprintf(buf, sizeof(buf), "目标: %s  信道 %d",
                     s.target.ssid[0] ? s.target.ssid : "(隐藏)", s.target.channel);
        } else {
            snprintf(buf, sizeof(buf), "目标: 未选择（点 OK 扫描 AP）");
        }
        s.rows[n++] = ui_row_create(list, buf, NULL);
    } else {
        // 信标类型行（仅 BEACON_SPAM）：点 OK 循环切换。
        char buf[80];
        snprintf(buf, sizeof(buf), "信标类型: %s", BEACON_NAMES[s.beacon_sub]);
        s.rows[n++] = ui_row_create(list, buf, NULL);
    }

    // 开始/停止行。
    const char *act = app_net_wifilab_running() ? "停止发射" : "开始发射";
    s.rows[n++] = ui_row_create(list, act, NULL);

    // 状态行：把"在发什么、发了多少"讲清楚，避免用户误以为已经停下来。已发帧数每拍
    // 变化，由 update_status() 原地更新，不触发整页重建。
    s.status_lbl = ui_label_create(c, "", ui_font_hint, ui_c_dim());
    if (s.status_lbl) lv_obj_set_width(s.status_lbl, WL_CW);
    update_status();

    // 应用选中高亮并滚动到选中行。
    for (int i = 0; i < n; i++) {
        if (s.rows[i].obj) ui_row_set_selected(s.rows[i], i == s.focus);
    }
    if (s.focus >= 0 && s.focus < n && s.rows[s.focus].obj) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }

    // 长按 ↑↓ 在整页里滚动；失败时短按 OK 重新尝试启动。
    if (failed) {
        ui_page_set_hint("OK 重试  长按↑↓ 滚动  长按OK 返回");
    } else if (app_net_wifilab_running()) {
        ui_page_set_hint("↑↓ 选择  OK 停发/切换  长按↑↓ 滚动  长按OK 返回");
    } else {
        ui_page_set_hint("↑↓ 选择  OK 开始/扫描  长按↑↓ 滚动  长按OK 返回");
    }
    s.lab_sig = lab_sig();
}

static void render_focus(void)
{
    // 行数随"是否需要目标"变化，这里按当前模式重算焦点上限。
    int max = APP_WIFILAB_MODE_COUNT + 1 + 1;   // 模式 + 上下文行 + 开始/停止
    for (int i = 0; i < max; i++) {
        if (s.rows[i].obj) ui_row_set_selected(s.rows[i], i == s.focus);
    }
    if (s.focus >= 0 && s.focus < max && s.rows[s.focus].obj) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }
    s.lab_sig = lab_sig();
}

static void refresh(void)
{
    if (s.view == WL_LAB) {
        if (lab_sig() != s.lab_sig) {
            build_lab();
        } else {
            // 结构不变：只刷新帧数，保住用户的滚动位置。
            update_status();
        }
    }
    // 目标视图不在这里重建：扫描完成由 page_wifilab_tick() 触发，避免每拍把
    // 列表滚动位置拉回选中行。
}

// 切换开始/停止。
static void toggle_run(void)
{
    if (app_net_wifilab_running()) {
        app_net_wifilab_request_stop();
        ui_hint_flash("正在停止…", 1000);
    } else {
        // 需要目标却还没选：先提示去扫描，不启动。
        if (mode_needs_target(cur_mode()) && !s.target.valid) {
            ui_hint_flash("请先选一个目标 AP", 1200);
            return;
        }
        app_net_wifilab_request_start(cur_mode(), s.beacon_sub,
                                      s.target.valid ? s.target.bssid : NULL,
                                      s.target.valid ? s.target.channel : 1,
                                      s.target.valid ? s.target.ssid : NULL,
                                      s.target.valid ? strlen(s.target.ssid) : 0,
                                      0);
        ui_hint_flash("正在启动 Wi-Fi…", 1200);
    }
    build_lab();
}

// ---------------------------------------------------------------------------
// 选目标页（展示最近一次扫描结果）
// ---------------------------------------------------------------------------
static void build_target(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.status_lbl = NULL;

    ui_header_create(c, "选择目标 AP", NULL, NULL, NULL);

    const app_channel_report_t *r = app_net_channel_report();
    lv_obj_t *list = ui_list_create(c);

    // 第 0 行：重新扫描。
    s.rows[0] = ui_row_create(list, "重新扫描附近 AP", NULL);

    int n = 1;
    if (!r || r->stored == 0) {
        // 无数据：提示先扫描。
        s.rows[n++] = ui_row_create(list, "（暂无结果，先扫描）", NULL);
    } else {
        int aps = (r->stored < WL_MAX_APS) ? r->stored : WL_MAX_APS;
        for (int i = 0; i < aps && n < (int)(sizeof(s.rows) / sizeof(s.rows[0])); i++) {
            char buf[80];
            const app_channel_ap_t *ap = &r->aps[i];
            snprintf(buf, sizeof(buf), "%s  信道 %d",
                     ap->ssid[0] ? ap->ssid : "(隐藏)", ap->channel);
            s.rows[n++] = ui_row_create(list, buf, NULL);
        }
    }

    for (int i = 0; i < n; i++) {
        if (s.rows[i].obj) ui_row_set_selected(s.rows[i], i == s.focus);
    }
    if (s.focus >= 0 && s.focus < n && s.rows[s.focus].obj) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }

    ui_page_set_hint("↑↓ 选择  OK 选 AP/扫描  长按↑↓ 滚动  长按OK 返回");
}

// 触发一次信道体检扫描（复用 app_net 的扫描能力，结果缓存供本页与 AP_LIST 复用）。
static void request_scan(void)
{
    s.scanning = true;
    app_net_channel_scan_request();
    ui_hint_flash("正在扫描…", 1500);
}

static void on_target_ok(void)
{
    if (s.focus == 0) {
        // 重新扫描。
        request_scan();
        return;
    }
    const app_channel_report_t *r = app_net_channel_report();
    int ap_idx = s.focus - 1;   // 第 0 行是"重新扫描"
    if (!r || ap_idx < 0 || ap_idx >= r->stored) return;
    const app_channel_ap_t *ap = &r->aps[ap_idx];
    memcpy(s.target.bssid, ap->bssid, 6);
    s.target.channel = ap->channel;
    int sl = 0;
    while ((size_t)sl < sizeof(ap->ssid) && ap->ssid[sl] != '\0') sl++;
    memcpy(s.target.ssid, ap->ssid, (size_t)sl);
    s.target.ssid[sl] = '\0';
    s.target.valid = true;
    // 回到实验室，并把焦点移到"开始发射"行。
    s.view = WL_LAB;
    s.focus = APP_WIFILAB_MODE_COUNT + 1;   // 开始/停止 行
    build_lab();
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------
void page_wifilab_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.view = WL_CONSENT;
    s.consent = false;
    s.mode_focus = 0;
    s.beacon_sub = APP_WIFILAB_BEACON_RANDOM;
    s.focus = 0;
    s.page = ui_page_create(NULL);
    build_consent();
}

void page_wifilab_exit(void)
{
    if (!s.active && !s.page.scr) return;
    // 先请求停发射，再删屏：屏没了就不该还有 Wi-Fi 实验角色在跑。
    app_net_wifilab_request_stop();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_wifilab_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 长按 OK：任何视图下都返回工具页（交给上层重建列表）。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        page_wifilab_exit();
        return;
    }

    if (s.view == WL_CONSENT) {
        // 本视图没有可选行：短按 ↑↓ 用来上下阅读这段比一屏长的授权说明，
        // 仅 OK 确认授权；未确认前不允许做任何发射。
        if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            scroll_content(btn == BSP_BTN_UP ? 40 : -40);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            s.consent = true;
            s.view = WL_LAB;
            s.focus = 0;
            build_lab();
        }
        return;
    }

    // 长按 ↑↓ 在任意视图里上下滚动内容：三键设备没有触摸/手势，这是唯一的滚动途径。
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        scroll_content(btn == BSP_BTN_UP ? 40 : -40);
        return;
    }

    if (s.view == WL_TARGET) {
        if (ev != BSP_BTN_CLICK) return;
        int n = wl_target_row_count();
        if (btn == BSP_BTN_UP) {
            s.focus = (s.focus + n - 1) % n;
            // 重建以刷新高亮（行数固定时也可只刷高亮，这里直接重建更稳妥）。
            build_target();
        } else if (btn == BSP_BTN_DOWN) {
            s.focus = (s.focus + 1) % n;
            build_target();
        } else if (btn == BSP_BTN_OK) {
            on_target_ok();
        }
        return;
    }

    // ---- 实验室视图 ----
    int max = APP_WIFILAB_MODE_COUNT + 1 + 1;   // 模式 + 上下文行 + 开始/停止
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s.focus = (s.focus + max - 1) % max;
        render_focus();
    } else if (btn == BSP_BTN_DOWN) {
        s.focus = (s.focus + 1) % max;
        render_focus();
    } else if (btn == BSP_BTN_OK) {
        if (s.focus < APP_WIFILAB_MODE_COUNT) {
            // 选中模式；若切到 BEACON 且当前在目标行之后，把焦点收敛到模式区避免错位。
            s.mode_focus = s.focus;
            if (mode_needs_target(cur_mode())) {
                // 保持焦点在上下文行（紧接着模式之后）
                s.focus = APP_WIFILAB_MODE_COUNT;
            } else {
                s.focus = APP_WIFILAB_MODE_COUNT;   // 切到信标类型行
            }
            build_lab();
        } else if (s.focus == APP_WIFILAB_MODE_COUNT) {
            // 上下文行：需要目标 → 进入选目标；否则循环信标类型。
            if (mode_needs_target(cur_mode())) {
                s.view = WL_TARGET;
                s.focus = 0;
                request_scan();
                build_target();
            } else {
                s.beacon_sub = (app_wifilab_beacon_t)((s.beacon_sub + 1) % APP_WIFILAB_BEACON_COUNT);
                build_lab();
            }
        } else {
            // 开始/停止行。
            toggle_run();
        }
    }
}

void page_wifilab_tick(void)
{
    if (!s.active || !s.page.scr) return;

    // 选目标视图里：扫描进行中则等结果，完成后刷新列表。
    if (s.view == WL_TARGET && s.scanning) {
        if (app_net_channel_scan_state() != APP_FETCH_RUNNING) {
            s.scanning = false;
            build_target();
        }
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s.last_ms < WL_TICK_MS) return;
    s.last_ms = now;
    refresh();
}

bool page_wifilab_active(void)
{
    return s.active;
}
