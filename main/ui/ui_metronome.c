// main/ui/ui_metronome.c —— 工具页子页：节拍器。
//
// 节拍器要"准"，而本项目的全局节拍照是 1 秒一次（ui_app.c），拿它推拍只能得到 1Hz
// 的分辨率——完全不够用。所以本页自带一个常驻的 FreeRTOS 节拍任务，以 2ms 轮询
// esp_timer 的单调时钟，到点就发声并点亮当前拍。纯逻辑（速度、拍序、tap 定速）在
// logic/app_metronome 里，可在主机上断言；本页只负责发声、画面与按键。
//
// 线程约定（关键）：
//   * s.m 由节拍任务与按键回调共同读写，两者都必须在 bsp_lvgl_lock() 内访问，界面对象
//     也同样只在锁内触碰，任务与本页的按键处理因此天然串行。
//   * 发声是可能阻塞的操作，放在解锁之后再写 I2S，绝不占着 LVGL 锁；否则一次 25ms 的
//     写入就会让整屏卡一下。
//   * 离开页面时先置 s.active=false 再删屏：任务每次取锁后都重新判断 active，锁内不会
//     与"删屏"并发，因此不存在访问已删除控件的情况。任务常驻（与 net/app_ble 的异步
//     worker 同一做法），空闲时只按 20ms 睡，不做任何界面操作。
//
// 交互取舍：
//   * 三键里 OK 必须留给"开始/停止"，长按 OK 又是全局约定的"返回"，所以粗调（±10）
//     没有落脚点。取而代之：把"敲击定速"做成第二个视图（长按 ↑ 进入）——用户跟着节拍
//     敲几下就能拿到任意速度，比按住 ±10 更接近真机节拍器的用法。
//   * 到边界（40 / 240）时给一句提示，而不是让按键静默无效——"按了没反应"是最容易被
//     当成故障的交互。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_state.h"
#include "logic/app_metronome.h"

#include "bsp_audio.h"
#include "bsp_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include <stdio.h>

#define MT_CW (UI_W - 2 * UI_MARGIN_X)

// ---- 发声 ----
// 16kHz/16bit 单声道，与 ui_sound 一致：同格式复用同一个已打开的 codec，切歌页也不会
// 触发一次重新 open（重新 open 在无 PSRAM 的目标上代价明显）。
#define MT_RATE          16000
#define MT_CLICK_MS      25
#define MT_CLICK_SAMPLES (MT_RATE * MT_CLICK_MS / 1000)
#define MT_TONE_NORMAL   1000     // 弱拍：低沉短促
#define MT_TONE_ACCENT   1568     // 重拍：高一些，一耳朵能听出小节从哪里开始
#define MT_AMP_NORMAL    4600
#define MT_AMP_ACCENT    8200

typedef enum {
    MT_MAIN = 0,
    MT_TAP,
} mt_view_t;

static struct {
    volatile bool  active;        // 页面是否在屏；任务据此决定是否碰界面
    volatile bool  playing;       // 是否在走拍，决定任务的轮询周期
    ui_page_t      page;
    mt_view_t      view;

    app_metronome_t m;            // 只在 LVGL 锁内访问

    bool     audio_ok;
    bool     muted;

    lv_obj_t *hdr_right;
    lv_obj_t *bpm_lbl;
    lv_obj_t *unit_lbl;
    lv_obj_t *tap_lbl;
    lv_obj_t *dots[APP_METRO_BEATS_PER_BAR];
} s;

// 节拍任务只建一次、常驻（见文件头说明）。
static TaskHandle_t s_task;
static int16_t      s_click_normal[MT_CLICK_SAMPLES];
static int16_t      s_click_accent[MT_CLICK_SAMPLES];
static bool         s_click_ready;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static const char *tempo_text(int bpm)
{
    if (bpm < 60)  return "很慢";
    if (bpm < 80)  return "慢";
    if (bpm < 120) return "适中";
    if (bpm < 168) return "快";
    return "很快";
}

// ---------------------------------------------------------------------------
// 声音
// ---------------------------------------------------------------------------

// 生成一段带淡入淡出的方波。不加淡化时，码流在首尾瞬间从 0 跳到满幅，ES8311 会听到
// 明显的"啪"声，反而盖过拍点本身。
static void fill_click(int16_t *buf, int hz, int amp)
{
    int period = MT_RATE / (hz > 0 ? hz : 1);
    if (period < 4) period = 4;
    int fade = MT_RATE / 400;                 // 约 2.5ms
    if (fade > MT_CLICK_SAMPLES / 2) fade = MT_CLICK_SAMPLES / 2;

    int phase = 0;
    for (int i = 0; i < MT_CLICK_SAMPLES; i++) {
        int a = amp;
        if (i < fade) a = a * i / fade;
        else if (i >= MT_CLICK_SAMPLES - fade) a = a * (MT_CLICK_SAMPLES - i) / fade;
        buf[i] = (phase < period / 2) ? (int16_t)a : (int16_t)-a;
        if (++phase >= period) phase = 0;
    }
}

static void click_build(void)
{
    if (s_click_ready) return;
    fill_click(s_click_normal, MT_TONE_NORMAL, MT_AMP_NORMAL);
    fill_click(s_click_accent, MT_TONE_ACCENT, MT_AMP_ACCENT);
    s_click_ready = true;
}

static bool audio_prepare(void)
{
    if (bsp_audio_set_format(MT_RATE, 16, 1) != ESP_OK) return false;
    int vol = app_state_settings()->volume;
    if (vol < 10) vol = 10;
    if (vol > 100) vol = 100;
    bsp_audio_set_volume((uint8_t)vol);
    return true;
}

static void click_play(bool accent)
{
    // 静音是用户的明确选择，这里不绕开它；页面会用横幅说明"不会出声以及怎么打开"。
    if (!s.audio_ok || s.muted || !s_click_ready) return;
    const int16_t *buf = accent ? s_click_accent : s_click_normal;
    if (bsp_audio_write(buf, MT_CLICK_SAMPLES * sizeof(int16_t)) != ESP_OK) {
        ESP_LOGD("metronome", "发声失败，忽略");
    }
}

// ---------------------------------------------------------------------------
// 画面
// ---------------------------------------------------------------------------

static void render_dots(int firing)
{
    for (int i = 0; i < APP_METRO_BEATS_PER_BAR; i++) {
        if (!s.dots[i]) continue;
        uint32_t col = ui_c_border();
        if (i == firing) col = (i == 0) ? ui_c_live() : ui_c_accent();
        lv_obj_set_style_bg_color(s.dots[i], lv_color_hex(col), 0);
    }
}

static void render_bpm(void)
{
    int bpm = app_metronome_bpm(&s.m);
    if (s.bpm_lbl) lv_label_set_text_fmt(s.bpm_lbl, "%d", bpm);
    if (s.unit_lbl) lv_label_set_text_fmt(s.unit_lbl, "%s · BPM", tempo_text(bpm));
}

static void render_tap(void)
{
    if (!s.tap_lbl) return;
    if (s.m.tap_count <= 0) {
        lv_label_set_text(s.tap_lbl, "连敲 3 下以上，就能定出新速度");
        return;
    }
    if (s.m.tap_count < APP_METRO_TAP_MIN_TAPS) {
        lv_label_set_text_fmt(s.tap_lbl, "已敲 %d 下，再敲 %d 下",
                              s.m.tap_count, APP_METRO_TAP_MIN_TAPS - s.m.tap_count);
        return;
    }
    lv_label_set_text_fmt(s.tap_lbl, "已敲 %d 下，速度已更新", s.m.tap_count);
}

static void render_state(void)
{
    if (s.hdr_right) {
        lv_label_set_text(s.hdr_right, app_metronome_running(&s.m) ? "运行中" : "已停止");
    }
}

static void set_hint(void)
{
    if (s.view == MT_TAP) {
        ui_page_set_hint("OK 敲一下   ↑↓ 微调\n长按OK 返回主界面");
    } else {
        ui_page_set_hint("OK 开始/停止  ↑↓ 调速度\n长按↑ 敲击定速   长按OK 返回");
    }
}

// 造出卡片里的"大号速度 + 说明 + 四拍圆点"。两张视图共用，只是说明行不同。
static void build_number_card(const char *sub_hint)
{
    lv_obj_t *card = ui_card_create(s.page.content, 0, 0, MT_CW, 146, ui_c_accent());

    s.bpm_lbl = ui_label_create(card, "120", ui_font_display, ui_c_text());
    lv_obj_set_width(s.bpm_lbl, MT_CW);
    lv_obj_set_style_text_align(s.bpm_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.bpm_lbl, 0, 6);

    s.unit_lbl = ui_label_create(card, "", ui_font_body, ui_c_dim());
    lv_obj_set_width(s.unit_lbl, MT_CW);
    lv_obj_set_style_text_align(s.unit_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.unit_lbl, 0, 60);

    // 四拍圆点固定尺寸、只换颜色：改变尺寸会让这一行左右抖动，越接近重拍抖得越明显。
    const int dot = 14;
    const int step = 30;
    int start = (MT_CW - (step * (APP_METRO_BEATS_PER_BAR - 1) + dot)) / 2;
    for (int i = 0; i < APP_METRO_BEATS_PER_BAR; i++) {
        lv_obj_t *d = lv_obj_create(card);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(ui_c_border()), 0);
        lv_obj_set_pos(d, start + i * step, 104);
        s.dots[i] = d;
    }

    if (sub_hint) {
        lv_obj_t *lbl = ui_label_create(card, sub_hint, ui_font_hint, ui_c_dim());
        lv_obj_set_width(lbl, MT_CW - 20);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lbl, 10, 124);
    }
}

static void build_main(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    for (int i = 0; i < APP_METRO_BEATS_PER_BAR; i++) s.dots[i] = NULL;
    s.bpm_lbl = s.unit_lbl = s.tap_lbl = NULL;

    ui_header_create(c, "节拍器", NULL, NULL, &s.hdr_right);
    if (s.muted) {
        ui_banner_create(c, "设备已静音，节拍不会出声。可在主页长按 ↑ 打开快捷面板关闭静音",
                         ui_c_warn());
    } else if (!s.audio_ok) {
        ui_banner_create(c, "音频不可用，只能看画面不能听声音", ui_c_warn());
    }

    build_number_card("每小节 4 拍，第 1 拍为重拍");
    render_state();
    render_bpm();
    render_dots(-1);
    set_hint();
}

static void build_tap(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    for (int i = 0; i < APP_METRO_BEATS_PER_BAR; i++) s.dots[i] = NULL;
    s.bpm_lbl = s.unit_lbl = s.tap_lbl = NULL;

    ui_header_create(c, "敲击定速", NULL, NULL, &s.hdr_right);
    if (s.muted || !s.audio_ok) {
        ui_banner_create(c, s.muted ? "设备已静音，听不到节拍，但仍可跟着心里数拍子敲"
                                    : "音频不可用，跟着心里数拍子敲即可",
                         ui_c_warn());
    } else {
        ui_banner_create(c, "跟着心里的拍子按 OK，连敲几下就定速", ui_c_accent());
    }

    build_number_card(NULL);
    // 用同一张卡片底部那一行显示敲击进度：省掉一张卡，画面更干净。
    s.tap_lbl = ui_label_create(c, NULL, ui_font_body, ui_c_dim());
    lv_obj_set_width(s.tap_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(s.tap_lbl, LV_TEXT_ALIGN_CENTER, 0);

    render_state();
    render_bpm();
    render_tap();
    set_hint();
}

// ---------------------------------------------------------------------------
// 节拍任务
// ---------------------------------------------------------------------------

static void mt_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s.active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint64_t t = now_ms();
        bool play = false;
        bool accent = false;

        if (bsp_lvgl_lock(20)) {
            if (s.active) {
                if (app_metronome_running(&s.m) &&
                    app_metronome_due(&s.m, t, &accent)) {
                    // app_metronome_due 已经把小节位置推进到下一拍，所以刚响的这一拍是
                    // 推进前的下标。
                    int firing = (app_metronome_beat_index(&s.m)
                                  + APP_METRO_BEATS_PER_BAR - 1) % APP_METRO_BEATS_PER_BAR;
                    render_dots(firing);
                    // 打拍子期间不该熄屏：看得见拍点才有"节拍器在工作"的确认。
                    ui_app_note_activity();
                    play = true;
                }
                if (app_metronome_tap_poll(&s.m, t)) render_tap();
            }
            bsp_lvgl_unlock();
        }

        // 发声放在锁外：I2S 写入可能阻塞十几毫秒，占着 LVGL 锁会拖慢整屏刷新。
        if (play && s.active) click_play(accent);

        vTaskDelay(pdMS_TO_TICKS(s.playing ? 2 : 10));
    }
}

static void ensure_task(void)
{
    if (s_task) return;
    if (xTaskCreate(mt_task, "metronome", 4096, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE("metronome", "节拍任务创建失败");
    }
}

// ---------------------------------------------------------------------------
// 按键与操作
// ---------------------------------------------------------------------------

static void nudge(int delta)
{
    if (!app_metronome_nudge(&s.m, delta)) {
        ui_hint_flash(delta > 0 ? "已到最高 240" : "已到最低 40", 1200);
        return;
    }
    render_bpm();
}

static void toggle_run(void)
{
    if (app_metronome_running(&s.m)) {
        app_metronome_stop(&s.m);
        s.playing = false;
        render_dots(-1);
        render_state();
        ui_hint_flash("已停止", 900);
        return;
    }

    app_metronome_start(&s.m, now_ms());
    s.playing = true;
    render_state();
    if (s.muted || !s.audio_ok) ui_hint_flash("已开始（无声音）", 1500);
    else ui_hint_flash("开始", 800);
}

void page_metronome_enter(void)
{
    s.page.scr = NULL;
    s.page.content = NULL;
    s.page.hint = NULL;
    s.view = MT_MAIN;
    s.hdr_right = s.bpm_lbl = s.unit_lbl = s.tap_lbl = NULL;
    for (int i = 0; i < APP_METRO_BEATS_PER_BAR; i++) s.dots[i] = NULL;
    s.playing = false;

    app_metronome_init(&s.m);
    click_build();
    s.audio_ok = audio_prepare();
    s.muted = app_state_settings()->sound_muted;

    ensure_task();
    s.page = ui_page_create(NULL);
    build_main();
    s.active = true;

    if (s.muted) ui_hint_flash("已静音：不会出声", 1800);
    else if (!s.audio_ok) ui_hint_flash("音频不可用：不会出声", 1800);
}

void page_metronome_exit(void)
{
    if (!s.active && !s.page.scr) return;
    // 先停拍、再停界面访问、最后删屏：任务每次取锁后都会重新判断 active，因此删屏之后
    // 它不会再写任何已释放的控件。
    app_metronome_stop(&s.m);
    s.playing = false;
    s.active = false;
    if (s.page.scr) lv_obj_delete(s.page.scr);
    s.page.scr = NULL;
    s.page.content = NULL;
    s.page.hint = NULL;
    s.hdr_right = s.bpm_lbl = s.unit_lbl = s.tap_lbl = NULL;
    for (int i = 0; i < APP_METRO_BEATS_PER_BAR; i++) s.dots[i] = NULL;
}

void page_metronome_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr) return;

    // 长按 OK：子页内逐级返回（敲击定速 → 主界面 → 工具页），与遥控页同一套层级约定。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s.view == MT_TAP) {
            s.view = MT_MAIN;
            build_main();
        } else {
            page_metronome_exit();
        }
        return;
    }

    if (s.view == MT_TAP) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_OK) {
            app_metronome_tap(&s.m, now_ms());
            render_bpm();
            render_tap();
        } else {
            nudge(btn == BSP_BTN_UP ? -1 : 1);
        }
        return;
    }

    // ---- 主界面 ----
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
        s.view = MT_TAP;
        app_metronome_tap_begin(&s.m);
        build_tap();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) toggle_run();
    else nudge(btn == BSP_BTN_UP ? -1 : 1);
}

void page_metronome_tick(void)
{
    // 节拍与画面都由常驻任务推进，这里不需要做事：1 秒的全局节拍对节拍器太粗。
}

bool page_metronome_active(void)
{
    return s.active;
}