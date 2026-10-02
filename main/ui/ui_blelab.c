// main/ui/ui_blelab.c —— 工具页子页：BLE 实验（BLE advertising lab）。
//
// 把一个"仅广播"的蓝牙角色（net/app_ble 的 advertiser）包成普通人能用的页面。它只是把
// logic/app_blelab 算好的原始广播字节通过协议栈发出去，本身不建立连接、不窃取任何东西——
// 危险来自"对谁发"。所以本页在发出任何一字节之前，必须先过一道授权/法律告知，用户按 OK
// 明确确认后才进入实验室；退出页面授权即作废，下次进来重新确认。
//
// 与"找设备"页同样的非阻塞约定：开启/停止都走 app_ble 的异步请求，绝不在按键回调里直接
// 拉起或拆掉协议栈（那会卡住 LVGL）。本页只读状态、发请求，不碰 NimBLE。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "net/app_ble.h"
#include "logic/app_blelab.h"

#include "esp_timer.h"
#include "lvgl.h"

#include <string.h>

#define BL_CW   (UI_W - 2 * UI_MARGIN_X)
#define BL_TICK_MS 500

// 四个模式的中文名（与 logic/app_blelab 的 app_blelab_mode_t 顺序一致）。
static const char *const MODE_NAMES[APP_BLELAB_MODE_COUNT] = {
    [APP_BLELAB_MODE_APPLE_AUDIO] = "苹果音频弹窗",
    [APP_BLELAB_MODE_APPLE_SETUP] = "苹果设置弹窗",
    [APP_BLELAB_MODE_SWIFT_PAIR]  = "Swift Pair",
    [APP_BLELAB_MODE_IBEACON]     = "iBeacon",
};

typedef enum {
    BL_CONSENT = 0,   // 授权/法律告知页，未确认前禁止广播
    BL_LAB,           // 实验室：选模式 + 开始/停止
} bl_view_t;

static struct {
    bool     active;
    ui_page_t page;
    bl_view_t view;

    bool     consent;            // 本次进页是否已授权
    int      focus;              // 选中的模式下标 [0, APP_BLELAB_MODE_COUNT)

    ui_row_t rows[APP_BLELAB_MODE_COUNT];
    lv_obj_t *hdr_right;
    lv_obj_t *status_lbl;

    uint32_t last_ms;
    int      lab_sig;            // 上次构建实验室页时的状态指纹，用于避免每拍重建
} s;

// 当前选中模式（focus 越界时退回 0）。
static app_blelab_mode_t cur_mode(void)
{
    if (s.focus < 0 || s.focus >= APP_BLELAB_MODE_COUNT) return APP_BLELAB_MODE_APPLE_AUDIO;
    return (app_blelab_mode_t)s.focus;
}

// 内容上下滚动。三键设备没有触摸和手势，长按 ↑↓ 是唯一的滚动途径。
static void scroll_content(int dy)
{
    if (s.page.content) lv_obj_scroll_by(s.page.content, 0, dy, LV_ANIM_OFF);
}

// 实验室页的状态指纹：只有它变了才重建。以前每 500ms 无脑重建，每次都会把滚动位置
// 和高亮拉回选中行，用户看到的正是"按键滚不动、页面自己弹回去"。
static int lab_sig(void)
{
    bool failed = app_ble_adv_last_error() != ESP_OK;
    bool running = app_ble_adv_running() && !failed;
    return (failed ? 1 : 0) | (running ? 2 : 0) | ((int)app_ble_adv_mode() << 2)
           | (s.focus << 6);
}

// ---------------------------------------------------------------------------
// 授权/法律告知页
// ---------------------------------------------------------------------------
static void build_consent(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.rows, 0, sizeof(s.rows));
    s.hdr_right = NULL;
    s.status_lbl = NULL;

    ui_header_create(c, "BLE 实验", NULL, NULL, NULL);

    ui_banner_create(c, "仅限自有设备与授权环境用于学习研究", ui_c_warn());

    const char *body =
        "本功能会向周围广播特制的蓝牙报文（苹果弹窗 / Swift Pair / iBeacon 等）。"
        "这些报文可能触发附近设备的弹窗或提示。\n\n"
        "对他人或你无权操作的设备广播，可能违反当地无线电管理法规与平台使用条款。"
        "继续即表示你确认：仅在自己拥有或已获书面授权的设备上、于合法授权的环境内做学习研究，"
        "并自行承担由此产生的一切后果。";
    lv_obj_t *lbl = ui_label_create(c, body, ui_font_body, ui_c_text());
    if (lbl) {
        lv_obj_set_width(lbl, BL_CW);
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
    s.hdr_right = NULL;
    s.status_lbl = NULL;

    ui_header_create(c, "BLE 实验", NULL, NULL, &s.hdr_right);

    bool failed = app_ble_adv_last_error() != ESP_OK;
    if (failed) {
        const char *why = app_ble_adv_error_text();
        ui_banner_create(c, why ? why : "蓝牙没能打开，请稍后重试", ui_c_warn());
    }

    // 模式列表：选中行高亮，运行中的模式在右侧标"广播中"（启动失败时不算在播）。
    bool running = app_ble_adv_running() && !failed;
    lv_obj_t *list = ui_list_create(c);
    for (int i = 0; i < APP_BLELAB_MODE_COUNT; i++) {
        const char *val = "";
        if (running && (app_blelab_mode_t)i == app_ble_adv_mode()) {
            val = "广播中";
        }
        s.rows[i] = ui_row_create(list, MODE_NAMES[i], val);
    }

    // 状态行：把"在播什么"讲清楚，避免用户误以为已经停下来。
    const char *stat;
    if (failed) {
        stat = "状态：未开启";
    } else if (running) {
        stat = "状态：广播中";
    } else {
        stat = "状态：已停止";
    }
    s.status_lbl = ui_label_create(c, stat, ui_font_hint, ui_c_dim());
    if (s.status_lbl) lv_obj_set_width(s.status_lbl, BL_CW);

    // 应用选中高亮，并滚动到选中行。
    for (int i = 0; i < APP_BLELAB_MODE_COUNT; i++) {
        ui_row_set_selected(s.rows[i], i == s.focus);
    }
    if (s.focus >= 0 && s.focus < APP_BLELAB_MODE_COUNT && s.rows[s.focus].obj) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }

    // 提示与实际按键严格对应：长按 ↑↓ 上下滚动；失败时短按 OK 重新尝试启动。
    if (failed) {
        ui_page_set_hint("OK 重试  长按↑↓ 滚动  长按OK 返回工具页");
    } else if (app_ble_adv_running()) {
        ui_page_set_hint("↑↓ 选模式  OK 停止广播  长按↑↓ 滚动  长按OK 返回工具页");
    } else {
        ui_page_set_hint("↑↓ 选模式  OK 开始广播  长按↑↓ 滚动  长按OK 返回工具页");
    }
    s.lab_sig = lab_sig();
}

static void render_focus(void)
{
    for (int i = 0; i < APP_BLELAB_MODE_COUNT; i++) {
        if (s.rows[i].obj) ui_row_set_selected(s.rows[i], i == s.focus);
    }
    if (s.focus >= 0 && s.focus < APP_BLELAB_MODE_COUNT && s.rows[s.focus].obj) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }
    s.lab_sig = lab_sig();
}

static void refresh(void)
{
    if (s.view != BL_LAB) return;
    // 只在状态真正变化时重建；帧数/运行状态没变就不动页面，滚动位置因此得以保留。
    if (lab_sig() == s.lab_sig) return;
    build_lab();
}

static void toggle_run(void)
{
    if (app_ble_adv_running()) {
        app_ble_adv_request_stop();
        ui_hint_flash("正在停止…", 1000);
    } else {
        app_ble_adv_set_mode(cur_mode());
        app_ble_adv_request_start();
        ui_hint_flash("正在启动蓝牙…", 1200);
    }
    build_lab();
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------
void page_blelab_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.view = BL_CONSENT;
    s.consent = false;
    s.focus = 0;
    s.page = ui_page_create(NULL);
    build_consent();
}

void page_blelab_exit(void)
{
    if (!s.active && !s.page.scr) return;
    // 先请求停广播，再删屏：屏没了就不该还有蓝牙角色在跑。
    app_ble_adv_request_stop();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_blelab_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 长按 OK：任何视图下都返回工具页（交给上层重建列表）。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        page_blelab_exit();
        return;
    }

    if (s.view == BL_CONSENT) {
        // 本视图没有可选行：短按 ↑↓ 用来上下阅读这段比一屏长的授权说明，
        // 仅 OK 确认授权；未确认前不允许做任何广播。
        if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            scroll_content(btn == BSP_BTN_UP ? 40 : -40);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            s.consent = true;
            s.view = BL_LAB;
            build_lab();
        }
        return;
    }

    // ---- 实验室视图 ----
    // 长按 ↑↓ 上下滚动内容：三键设备没有触摸/手势，这是唯一的滚动途径。放在短按
    // 分支之前，避免被下面的 `ev != CLICK` 直接丢掉（此前 scroll_content 没有被调用，
    // 提示里写的"长按↑↓ 滚动"实际无效）。
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        scroll_content(btn == BSP_BTN_UP ? 40 : -40);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        s.focus = (s.focus + (btn == BSP_BTN_UP ? APP_BLELAB_MODE_COUNT - 1 : 1))
                  % APP_BLELAB_MODE_COUNT;
        render_focus();
    } else if (btn == BSP_BTN_OK) {
        toggle_run();
    }
}

void page_blelab_tick(void)
{
    if (!s.active || !s.page.scr) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s.last_ms < BL_TICK_MS) return;
    s.last_ms = now;
    refresh();
}

bool page_blelab_active(void)
{
    return s.active;
}
