// main/ui/ui_settings.c —— 系统设置、热点配网、数据清除与首次引导。
//
// 设置页是"改一次就很少回来"的地方，所以按"功能相近、改动频率相近"的顺序平铺成一张
// 列表，不做多层菜单：网络、时间、显示、声音、数据各占一小段。UP/DOWN 移动，OK 直接
// 修改（开关类就地切换、枚举类循环、需要输入的开浮层），长按 OK 返回主页。
//
// 两条配网路径里，本文件只用设备热点网页配网：设备开热点、手机连上后打开
// http://192.168.4.1/ 填写 Wi-Fi、用手机时间校准、粘贴 otpauth 链接导入口令密钥。
// 该页面是设备本地服务，不需要互联网，所以纯离线用户也能用它配置与备份。
//
// 首次引导复用同一套控件：三步（时间 / 作息模板 / 口令密钥），每步都能跳过，
// 跳过之后主页与全部离线功能仍然完整可用。
#include "ui_pages.h"

#include "ui_app.h"
#include "ui_theme.h"
#include "ui_timeedit.h"

#include "app_state.h"
#include "logic/app_routine.h"
#include "logic/app_time.h"
#include "net/app_net.h"

#include "bsp_display.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define SET_CW  (UI_W - 2 * UI_MARGIN_X)
#define HINT_SETTINGS "↑↓ 选择  OK 修改  长按OK 返回"
#define HINT_HOME     "↑↓ 选择  OK 进入  长按↑ 面板  长按OK 熄屏"

enum {
    SET_NET = 0,
    SET_WIFI,
    SET_PROV,
    SET_SYNC,
    SET_TIME,
    SET_TZ,
    SET_TIMEOUT,
    SET_BRIGHT,
    SET_THEME,
    SET_SOUND,
    SET_VOLUME,
    SET_POWERSAVE,
    SET_ONBOARD,
    SET_DATA,
    SET_ROW_N,
};

static const char *const SET_TITLES[SET_ROW_N] = {
    "网络状态", "Wi-Fi 网络", "热点配网", "网络校时", "手动设置时间", "时区",
    "自动息屏", "屏幕亮度", "主题", "提示音", "提示音音量", "省电选项",
    "首次引导", "数据备份与清除",
};

static const char *const TIMEOUT_NAMES[APP_TIMEOUT_COUNT] = {
    "常亮", "15 秒", "30 秒", "1 分钟", "2 分钟", "5 分钟",
};

static const char *const THEME_NAMES[3] = { "深色", "浅色", "自动" };

// 时区候选：均为整半小时，覆盖常用地区，不做经纬度推算。
static const int TZ_TABLE[] = {
    -480, -420, -360, -300, -240, 0, 60, 120, 180, 240,
    300, 330, 360, 420, 480, 540, 600, 660, 720,
};
#define TZ_N ((int)(sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0])))

// 清除数据的类别与顺序（最后一项是恢复出厂设置）。
enum { CLR_BADGE = 0, CLR_ROUTINE, CLR_TOTP, CLR_REMIND, CLR_ESPORT, CLR_FACTORY, CLR_ROW_N };

static const char *const CLR_TITLES[CLR_ROW_N] = {
    "电子工牌", "作息表", "动态口令", "本地提醒", "赛事缓存", "恢复出厂设置",
};

static const app_data_kind_t CLR_KINDS[CLR_ROW_N] = {
    APP_DATA_BADGES, APP_DATA_ROUTINE, APP_DATA_TOTP,
    APP_DATA_REMINDERS, APP_DATA_ESPORTS, APP_DATA_SETTINGS,
};

static struct {
    ui_page_t page;
    lv_obj_t *banner;
    lv_obj_t *banner_lbl;
    ui_row_t rows[SET_ROW_N];
    int focus;
    char hint[64];

    // 热点配网浮层
    lv_obj_t *prov;
    lv_obj_t *prov_note;

    // 数据清除浮层
    lv_obj_t *data_panel;
    ui_row_t data_rows[CLR_ROW_N];
    int data_sel;
} s;

// 配网与首次引导可能同时需要"正在开启热点"这一状态，故放在文件级。
static volatile bool s_prov_busy;
static volatile bool s_prov_cancel;
static volatile int  s_prov_err;

static int s_edit_values[5];
static int s_clear_kind;

static void refresh_values(void);
static void update_hint(void);
static void prov_open(void);
static void prov_close(void);

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static void set_hint(const char *text)
{
    if (!text) return;
    if (strcmp(s.hint, text) == 0) return;
    snprintf(s.hint, sizeof(s.hint), "%s", text);
    ui_page_set_hint(text);
}

static void tz_text(int minutes, char *out, size_t cap)
{
    int h = minutes / 60;
    int m = minutes % 60;
    if (m < 0) {
        h -= 1;
        m += 60;
    }
    if (m == 0) snprintf(out, cap, "UTC%+d", h);
    else snprintf(out, cap, "UTC%+d:%02d", h, m);
}

static void data_panel_close(void);

// 覆盖层底：全屏压暗 + 一张卡片，卡片是纵向 flex 容器。
static lv_obj_t *overlay_create(int card_h, lv_obj_t **card_out, const char *title)
{
    lv_obj_t *ov = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_size(ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);

    lv_obj_t *card = ui_card_create(ov, 12, (UI_H - card_h) / 2, UI_W - 24, card_h,
                                    ui_c_accent());
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);

    if (title) ui_label_create(card, title, ui_font_title, ui_c_text());
    if (card_out) *card_out = card;
    return ov;
}

// 浮层里的选项行统一比设置列表矮一点，保证一屏放得下三行加说明。
static ui_row_t overlay_row_create(lv_obj_t *parent, const char *title, const char *value)
{
    ui_row_t row = ui_row_create(parent, title, value);
    lv_obj_set_height(row.obj, 34);
    return row;
}

// ---------------------------------------------------------------------------
// 设置列表
// ---------------------------------------------------------------------------

static void settings_focus(int index)
{
    if (index < 0) index = 0;
    if (index >= SET_ROW_N) index = SET_ROW_N - 1;
    s.focus = index;
    for (int i = 0; i < SET_ROW_N; i++) {
        ui_row_set_selected(s.rows[i], i == index);
    }
    ui_scroll_into_view(s.rows[index].obj);
}

static void refresh_banner(void)
{
    if (!s.banner_lbl) return;

    app_settings_t *st = app_state_settings();
    char text[128] = { 0 };

    if (s_prov_busy) {
        snprintf(text, sizeof(text), "正在开启热点…");
    } else if (app_net_prov_active()) {
        snprintf(text, sizeof(text), "热点已开启：连上 %s 后打开 %s",
                 app_net_prov_ssid(), app_net_prov_url());
    } else if (!st->time_synced) {
        snprintf(text, sizeof(text), "时间未校准，口令与提醒可能不准；开启热点配网可用手机时间校准。");
    } else if (app_net_time_state() == APP_FETCH_FAILED) {
        const char *err = app_net_time_error();
        snprintf(text, sizeof(text), "上次校时失败：%s", err ? err : "未知原因");
    } else if (app_state_net() != APP_NET_ONLINE && !app_net_has_credentials()) {
        snprintf(text, sizeof(text), "离线模式：除赛事数据外全部功能可离线使用。");
    }

    if (text[0] == '\0') {
        lv_obj_add_flag(s.banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s.banner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s.banner_lbl, text);
}

static void refresh_values(void)
{
    app_settings_t *st = app_state_settings();
    char buf[48];

    int net = (int)app_state_net();
    ui_row_set_value(s.rows[SET_NET], net == APP_NET_ONLINE ? "在线"
                            : net == APP_NET_CONNECTING ? "连接中" : "离线");

    const char *ssid = app_net_saved_ssid();
    ui_row_set_value(s.rows[SET_WIFI], (ssid && ssid[0]) ? ssid : "未配置");
    ui_row_set_value(s.rows[SET_PROV], app_net_prov_active() ? "已开启" : "关闭");

    app_fetch_state_t ts = app_net_time_state();
    if (ts == APP_FETCH_RUNNING) {
        ui_row_set_value(s.rows[SET_SYNC], "同步中…");
    } else if (ts == APP_FETCH_FAILED) {
        ui_row_set_value(s.rows[SET_SYNC], "同步失败");
    } else if (st->time_synced) {
        ui_row_set_value(s.rows[SET_SYNC],
                         st->time_source[0] ? st->time_source : "已同步");
    } else {
        ui_row_set_value(s.rows[SET_SYNC], "未同步");
    }

    app_datetime_t now = app_state_now();
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             now.year, now.month, now.day, now.hour, now.minute);
    ui_row_set_value(s.rows[SET_TIME], buf);

    tz_text(st->utc_offset_minutes, buf, sizeof(buf));
    ui_row_set_value(s.rows[SET_TZ], buf);

    ui_row_set_value(s.rows[SET_TIMEOUT], TIMEOUT_NAMES[st->screen_timeout]);

    snprintf(buf, sizeof(buf), "%d%%", st->backlight);
    ui_row_set_value(s.rows[SET_BRIGHT], buf);

    ui_row_set_value(s.rows[SET_THEME], THEME_NAMES[st->theme]);
    ui_row_set_value(s.rows[SET_SOUND], st->sound_muted ? "静音" : "开");

    snprintf(buf, sizeof(buf), "%d%%", st->volume);
    ui_row_set_value(s.rows[SET_VOLUME], buf);

    ui_row_set_value(s.rows[SET_POWERSAVE], st->power_save ? "开" : "关");
    ui_row_set_value(s.rows[SET_ONBOARD], st->onboarded ? "已完成" : "重新运行");
    ui_row_set_value(s.rows[SET_DATA], "");

    refresh_banner();
}

static void update_hint(void)
{
    if (s.prov) return;   // 浮层自己维护提示
    set_hint(HINT_SETTINGS);
}

// ---------------------------------------------------------------------------
// 手动设置时间
// ---------------------------------------------------------------------------

static void manual_time_done(bool saved, void *user)
{
    (void)user;
    if (saved) {
        app_datetime_t dt = {
            .year = s_edit_values[0], .month = s_edit_values[1], .day = s_edit_values[2],
            .hour = s_edit_values[3], .minute = s_edit_values[4], .second = 0,
        };
        if (app_time_valid(&dt)) {
            app_state_set_time(&dt, "手动");
            ui_hint_flash("时间已设置（手动）", 1500);
        } else {
            ui_hint_flash("日期无效，未保存", 1800);
        }
    }
    refresh_values();
    update_hint();
}

static void manual_time_open(void)
{
    app_datetime_t now = app_state_now();
    s_edit_values[0] = now.year;
    s_edit_values[1] = now.month;
    s_edit_values[2] = now.day;
    s_edit_values[3] = now.hour;
    s_edit_values[4] = now.minute;

    // 年份先给默认值，避免从零开始逐格加。
    if (s_edit_values[0] < 2000) s_edit_values[0] = 2026;

    static const ui_timeedit_field_t fields[5] = {
        { "年", 1970, 2099, 10, NULL },
        { "月", 1, 12, 1, NULL },
        { "日", 1, 31, 5, NULL },
        { "时", 0, 23, 1, NULL },
        { "分", 0, 59, 5, NULL },
    };
    ui_timeedit_open(s.page.scr, "手动设置时间", fields, s_edit_values, 5,
                     manual_time_done, NULL);
}

// ---------------------------------------------------------------------------
// 热点配网浮层
// ---------------------------------------------------------------------------

static void prov_start_task(void *arg)
{
    (void)arg;
    esp_err_t err = app_net_prov_start();
    if (s_prov_cancel) {
        app_net_prov_stop();
        err = ESP_ERR_INVALID_STATE;
        s_prov_cancel = false;
    }
    s_prov_err = (int)err;
    s_prov_busy = false;
    vTaskDelete(NULL);
}

static void prov_refresh(void)
{
    if (!s.prov || !s.prov_note) return;

    char text[160];
    if (s_prov_busy) {
        snprintf(text, sizeof(text), "正在开启热点，请稍候…");
    } else if (s_prov_err != 0) {
        snprintf(text, sizeof(text), "热点开启失败，请重试。");
    } else {
        const char *note = app_net_prov_note();
        snprintf(text, sizeof(text),
                 "热点：%s\n密码：%s\n网址：%s\n%s",
                 app_net_prov_ssid(), app_net_prov_pass(), app_net_prov_url(),
                 note ? note : "手机连上热点后打开网址填写 Wi-Fi、校准时间或导入口令密钥。");
    }
    lv_label_set_text(s.prov_note, text);
}

static void prov_open(void)
{
    if (s.prov) return;

    lv_obj_t *card = NULL;
    s.prov = overlay_create(200, &card, "热点配网");

    // 信息标签：多行文本，宽度受卡片约束，超出自动换行。
    s.prov_note = ui_label_create(card, "", ui_font_body, ui_c_text());
    lv_obj_set_width(s.prov_note, UI_W - 24 - 24);
    lv_label_set_long_mode(s.prov_note, LV_LABEL_LONG_WRAP);

    if (!app_net_prov_active() && !s_prov_busy) {
        s_prov_busy = true;
        s_prov_cancel = false;
        s_prov_err = 0;
        if (xTaskCreate(prov_start_task, "prov_start", 4096, NULL, 5, NULL) != pdPASS) {
            s_prov_busy = false;
            s_prov_err = (int)ESP_ERR_NO_MEM;
        }
    }

    prov_refresh();
    set_hint("手机连上热点后打开 192.168.4.1  长按OK 关闭");
    refresh_values();
}

static void prov_close(void)
{
    if (s_prov_busy) {
        // 启动任务还没结束：交给它收尾，避免与 esp_wifi 的状态竞争。
        s_prov_cancel = true;
    } else if (app_net_prov_active()) {
        app_net_prov_stop();
    }

    if (s.prov) {
        lv_obj_delete(s.prov);
        s.prov = NULL;
        s.prov_note = NULL;
    }
    s.hint[0] = '\0';
    refresh_values();
    update_hint();
}

// ---------------------------------------------------------------------------
// 数据清除浮层
// ---------------------------------------------------------------------------

static void data_panel_refresh(void)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "%d / %d", app_state_badges()->count, APP_BADGE_MAX);
    ui_row_set_value(s.data_rows[CLR_BADGE], buf);

    app_routine_t *r = app_state_routine();
    bool routine_used = false;
    for (int d = 0; d < APP_ROUTINE_DAYS; d++) {
        if (r->days[d].count > 0) routine_used = true;
    }
    ui_row_set_value(s.data_rows[CLR_ROUTINE], routine_used ? "已设置" : "空");

    snprintf(buf, sizeof(buf), "%d 个", app_state_totp_count());
    ui_row_set_value(s.data_rows[CLR_TOTP], buf);

    snprintf(buf, sizeof(buf), "%d 条", app_state_reminders()->count);
    ui_row_set_value(s.data_rows[CLR_REMIND], buf);

    app_esport_cache_t *cache = app_state_esports();
    if (!cache->valid || cache->match_count <= 0) {
        ui_row_set_value(s.data_rows[CLR_ESPORT], "无缓存");
    } else {
        snprintf(buf, sizeof(buf), "%d 场", cache->match_count);
        ui_row_set_value(s.data_rows[CLR_ESPORT], buf);
    }

    ui_row_set_value(s.data_rows[CLR_FACTORY], "清除全部");
}

static void data_panel_focus(int index)
{
    if (index < 0) index = 0;
    if (index >= CLR_ROW_N) index = CLR_ROW_N - 1;
    s.data_sel = index;
    for (int i = 0; i < CLR_ROW_N; i++) {
        ui_row_set_selected(s.data_rows[i], i == index);
    }
}

static void clear_confirmed(bool confirmed, void *user)
{
    (void)user;
    if (!confirmed) return;

    int kind = s_clear_kind;
    if (kind < 0 || kind >= CLR_ROW_N) return;

    app_state_clear(CLR_KINDS[kind]);
    if (kind == CLR_FACTORY) {
        // 恢复出厂后设置回到默认值，包括"引导未完成"，下次开机重新引导。
        ui_hint_flash("已恢复出厂设置，重启后重新引导", 2200);
    } else {
        ui_hint_flash("已清除", 1200);
    }

    data_panel_refresh();
    refresh_values();
}

static void data_panel_activate(void)
{
    int kind = s.data_sel;
    if (kind < 0 || kind >= CLR_ROW_N) return;

    char body[96];
    if (kind == CLR_FACTORY) {
        snprintf(body, sizeof(body), "将清除工牌、作息、口令、提醒、赛事缓存与全部设置，且无法恢复。");
    } else {
        snprintf(body, sizeof(body), "将清除%s，且无法恢复。", CLR_TITLES[kind]);
    }

    s_clear_kind = kind;
    ui_dialog_open(s.page.scr, "确认清除？", body, "清除", clear_confirmed, NULL);
}

static void data_panel_open(void)
{
    if (s.data_panel) return;

    lv_obj_t *card = NULL;
    s.data_panel = overlay_create(280, &card, "数据备份与清除");

    lv_obj_t *tip = ui_label_create(card, "清除前会二次确认。工牌、作息、口令与恢复码可在手机配置页导出备份。",
                                    ui_font_hint, ui_c_dim());
    lv_obj_set_width(tip, UI_W - 24 - 20);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);

    for (int i = 0; i < CLR_ROW_N; i++) {
        s.data_rows[i] = overlay_row_create(card, CLR_TITLES[i], "");
    }
    data_panel_refresh();
    data_panel_focus(0);
    set_hint("↑↓ 选择  OK 清除  长按OK 返回");
}

static void data_panel_close(void)
{
    if (!s.data_panel) return;
    lv_obj_delete(s.data_panel);
    s.data_panel = NULL;
    s.hint[0] = '\0';
    refresh_values();
    update_hint();
}

// ---------------------------------------------------------------------------
// 设置动作分发
// ---------------------------------------------------------------------------

static void activate(void)
{
    app_settings_t *st = app_state_settings();

    switch (s.focus) {
    case SET_NET:
        if (!app_net_has_credentials()) {
            ui_hint_flash("未配置 Wi-Fi，先开启热点配网", 1800);
        } else if (!app_net_wifi_connected()) {
            if (app_net_wifi_start() == ESP_OK) ui_hint_flash("正在连接 Wi-Fi…", 1500);
            else ui_hint_flash("Wi-Fi 启动失败", 1500);
        } else {
            ui_hint_flash("已连接到 Wi-Fi", 1200);
        }
        break;

    case SET_WIFI:
        prov_open();
        break;

    case SET_PROV:
        if (app_net_prov_active() || s_prov_busy) prov_close();
        else prov_open();
        break;

    case SET_SYNC:
        if (!app_net_has_credentials()) {
            ui_hint_flash("先开启热点配网填写 Wi-Fi", 1800);
        } else {
            app_net_time_sync_request();
            ui_hint_flash("正在联网校时…", 1500);
        }
        break;

    case SET_TIME:
        manual_time_open();
        return;   // 浮层接管按键，回来时由回调刷新

    case SET_TZ: {
        int idx = 0;
        for (int i = 0; i < TZ_N; i++) {
            if (TZ_TABLE[i] == st->utc_offset_minutes) idx = i;
        }
        st->utc_offset_minutes = TZ_TABLE[(idx + 1) % TZ_N];
        app_state_save_settings();
        ui_hint_flash("时区已更新，时间显示随之调整", 1500);
        break;
    }

    case SET_TIMEOUT:
        st->screen_timeout = (app_timeout_t)((st->screen_timeout + 1) % APP_TIMEOUT_COUNT);
        app_state_save_settings();
        if (st->screen_timeout == APP_TIMEOUT_NEVER) {
            ui_hint_flash("常亮会明显增加耗电", 1800);
        }
        break;

    case SET_BRIGHT:
        st->backlight = (uint8_t)(st->backlight >= 100 ? 10 : st->backlight + 10);
        app_state_save_settings();
        // 立即生效：用户改亮度时马上看到结果，不用等退出。
        bsp_display_backlight(st->backlight);
        break;

    case SET_THEME:
        st->theme = (app_theme_choice_t)((st->theme + 1) % 3);
        app_state_save_settings();
        ui_hint_flash("主题已切换，返回主页后生效", 1800);
        break;

    case SET_SOUND:
        st->sound_muted = !st->sound_muted;
        app_state_save_settings();
        break;

    case SET_VOLUME:
        st->volume = (uint8_t)(st->volume >= 100 ? 20 : st->volume + 20);
        app_state_save_settings();
        break;

    case SET_POWERSAVE:
        st->power_save = !st->power_save;
        app_state_save_settings();
        break;

    case SET_ONBOARD:
        onboarding_open();
        break;

    case SET_DATA:
        data_panel_open();
        break;

    default:
        break;
    }

    refresh_values();
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_settings_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.page = ui_page_create(HINT_SETTINGS);

    // 顶部提示横幅：时间未校准、校时失败、热点开启、纯离线各有不同文案。
    s.banner = ui_card_create(s.page.content, 0, 0, SET_CW, 42, ui_c_warn());
    s.banner_lbl = ui_label_create(s.banner, "", ui_font_hint, ui_c_text());
    lv_obj_set_pos(s.banner_lbl, 10, 4);
    lv_obj_set_width(s.banner_lbl, SET_CW - 20);
    lv_label_set_long_mode(s.banner_lbl, LV_LABEL_LONG_WRAP);

    lv_obj_t *head = ui_label_create(s.page.content, "设置", ui_font_hint, ui_c_dim());
    (void)head;

    lv_obj_t *list = ui_list_create(s.page.content);
    for (int i = 0; i < SET_ROW_N; i++) {
        s.rows[i] = ui_row_create(list, SET_TITLES[i], "");
    }

    refresh_values();
    s.hint[0] = '\0';
    settings_focus(0);
    update_hint();
}

void page_settings_exit(void)
{
    ui_timeedit_close();
    ui_dialog_close();
    if (s.prov) {
        // 离开设置页就关热点：既省电，也不让配置入口长期暴露。
        prov_close();
    } else if (s_prov_busy) {
        s_prov_cancel = true;
    } else if (app_net_prov_active()) {
        app_net_prov_stop();
    }
    if (s.data_panel) {
        lv_obj_delete(s.data_panel);
        s.data_panel = NULL;
    }
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ui_timeedit_active()) {
        ui_timeedit_handle(btn, ev);
        return;
    }

    if (s.prov) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) prov_close();
        return;
    }

    if (s.data_panel) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            data_panel_close();
        } else if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) data_panel_focus(s.data_sel - 1);
            else if (btn == BSP_BTN_DOWN) data_panel_focus(s.data_sel + 1);
            else if (btn == BSP_BTN_OK) data_panel_activate();
        }
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) settings_focus(s.focus - 1);
    else if (btn == BSP_BTN_DOWN) settings_focus(s.focus + 1);
    else if (btn == BSP_BTN_OK) activate();
}

void page_settings_tick(void)
{
    if (!s.page.scr) return;

    // 引导浮层盖在本页之上，提示条与列表都由引导自己维护。
    if (onboarding_active()) return;

    // 配网启动任务、校时任务都在别的任务里推进，这里只把结果搬到界面上。
    if (s.prov) {
        prov_refresh();
        return;
    }
    if (s.data_panel) return;

    if (ui_timeedit_active()) return;

    refresh_values();
    update_hint();
}

// ---------------------------------------------------------------------------
// 首次引导（三步，每步可跳过）
// ---------------------------------------------------------------------------

typedef struct {
    lv_obj_t *ov;
    lv_obj_t *card;
    lv_obj_t *note;
    ui_row_t rows[3];
    int row_count;
    int sel;
    int step;
    bool prov_started;   // 上一次渲染时热点是否已开启，用于只重画一次
} onboard_t;

static onboard_t s_ob;

static void onboarding_render(void);

// 每一步的说明。第一步解释为什么需要时间，第二步说明模板可改，第三步说明口令离线可用。
static const char *const OB_STEP_NOTE[3] = {
    "时间用于动态口令、作息倒计时与提醒。未校准也能用，但口令可能与服务器不一致。",
    "作息决定主页的课程倒计时。先选一个最接近的模板，之后可在作息页逐项调整。",
    "动态口令完全离线生成。只有需要导入已有密钥时才用热点配网。",
};

static void onboarding_finish(void)
{
    if (s_prov_busy) {
        s_prov_cancel = true;
    } else if (app_net_prov_active()) {
        app_net_prov_stop();
    }

    app_settings_t *st = app_state_settings();
    st->onboarded = true;
    app_state_save_settings();

    if (s_ob.ov) {
        lv_obj_delete(s_ob.ov);
        s_ob.ov = NULL;
        s_ob.card = NULL;
    }
    memset(&s_ob, 0, sizeof(s_ob));

    // 引导可能从设置页打开（重新运行），也可能开机时直接打开；提示条要还给对应的页面。
    s.hint[0] = '\0';
    ui_page_set_hint(s.page.scr ? HINT_SETTINGS : HINT_HOME);
    ui_app_refresh_home();
}

static void onboarding_advance(void)
{
    if (s_ob.step >= 2) {
        onboarding_finish();
        return;
    }
    s_ob.step++;
    s_ob.sel = 0;
    onboarding_render();
}

static void onboarding_load_template(bool boarding)
{
    app_state_settings()->boarding = boarding;
    app_routine_load_template(app_state_routine(), boarding);
    app_state_save_routine();
    app_state_save_settings();
    // 先切步再提示：切步会整卡重画并重设提示条，顺序反了这次反馈会被立刻覆盖。
    onboarding_advance();
    ui_hint_flash(boarding ? "已套用住校模板" : "已套用走读模板", 1500);
}

static void onboarding_manual_done(bool saved, void *user)
{
    (void)user;
    if (saved) {
        app_datetime_t dt = {
            .year = s_edit_values[0], .month = s_edit_values[1], .day = s_edit_values[2],
            .hour = s_edit_values[3], .minute = s_edit_values[4], .second = 0,
        };
        if (app_time_valid(&dt)) {
            app_state_set_time(&dt, "手动");
            onboarding_advance();
            ui_hint_flash("时间已设置为手动值", 1500);
            return;
        }
        ui_hint_flash("日期无效，未保存", 1800);
    }
    onboarding_render();
}

static void onboarding_manual_time(void)
{
    app_datetime_t now = app_state_now();
    s_edit_values[0] = now.year >= 2000 ? now.year : 2026;
    s_edit_values[1] = now.month >= 1 ? now.month : 1;
    s_edit_values[2] = now.day >= 1 ? now.day : 1;
    s_edit_values[3] = now.hour;
    s_edit_values[4] = now.minute;

    static const ui_timeedit_field_t fields[5] = {
        { "年", 1970, 2099, 10, NULL },
        { "月", 1, 12, 1, NULL },
        { "日", 1, 31, 5, NULL },
        { "时", 0, 23, 1, NULL },
        { "分", 0, 59, 5, NULL },
    };
    ui_timeedit_open(s_ob.ov, "设置设备时间", fields, s_edit_values, 5,
                     onboarding_manual_done, NULL);
}

static void onboarding_focus(int index)
{
    if (s_ob.row_count <= 0) return;
    if (index < 0) index = 0;
    if (index >= s_ob.row_count) index = s_ob.row_count - 1;
    s_ob.sel = index;
    for (int i = 0; i < s_ob.row_count; i++) {
        ui_row_set_selected(s_ob.rows[i], i == index);
    }
}

static void onboarding_render(void)
{
    if (!s_ob.ov) return;

    // 每步都整卡重画：步骤之间控件数不同，复用旧卡反而要逐个改写类型。
    if (s_ob.card) {
        lv_obj_delete(s_ob.card);
        s_ob.card = NULL;
        s_ob.note = NULL;
    }
    s_ob.row_count = 0;

    char title[48];
    snprintf(title, sizeof(title), "首次设置  %d / 3", s_ob.step + 1);

    // 卡片只占内容区，状态栏与底部提示条保持可见，用户始终知道现在几点、能按什么。
    lv_obj_t *card = ui_card_create(s_ob.ov, 12, 8, UI_W - 24,
                                    UI_H - UI_STATUS_H - UI_HINT_H - 16, ui_c_accent());
    s_ob.card = card;
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);

    ui_label_create(card, title, ui_font_title, ui_c_text());

    s_ob.note = ui_label_create(card, OB_STEP_NOTE[s_ob.step], ui_font_hint, ui_c_dim());
    lv_obj_set_width(s_ob.note, UI_W - 24 - 20);
    lv_label_set_long_mode(s_ob.note, LV_LABEL_LONG_WRAP);

    if (s_ob.step == 0) {
        s_ob.rows[0] = overlay_row_create(card, "手动设置时间", "");
        s_ob.rows[1] = overlay_row_create(card, "跳过，稍后校准", "");
        s_ob.row_count = 2;
    } else if (s_ob.step == 1) {
        s_ob.rows[0] = overlay_row_create(card, "住校模板（含晚自习）", "");
        s_ob.rows[1] = overlay_row_create(card, "走读模板", "");
        s_ob.row_count = 2;
    } else {
        if (s_prov_busy) {
            s_ob.rows[0] = overlay_row_create(card, "正在开启热点…", "");
        } else if (app_net_prov_active()) {
            s_ob.rows[0] = overlay_row_create(card, "完成后继续", "");
            // 说明位置改放连接信息：这一步用户要照着输入的只有这几个字符串。
            lv_label_set_text_fmt(s_ob.note, "热点：%s\n密码：%s\n网址：%s",
                                  app_net_prov_ssid(), app_net_prov_pass(),
                                  app_net_prov_url());
        } else {
            s_ob.rows[0] = overlay_row_create(card, "开启热点配网导入", "");
        }
        s_ob.rows[1] = overlay_row_create(card, "跳过，仅用离线功能", "");
        s_ob.row_count = 2;
    }

    onboarding_focus(s_ob.sel);
    ui_page_set_hint("↑↓ 选择   OK 确定   长按OK 跳过");
}

// OK：执行当前行。每步的首行是"现在做"，次行是"跳过"，长按 OK 也能跳过。
static void onboarding_activate(void)
{
    if (s_ob.step == 0) {
        if (s_ob.sel == 0) onboarding_manual_time();
        else onboarding_advance();
        return;
    }

    if (s_ob.step == 1) {
        onboarding_load_template(s_ob.sel == 0);
        return;
    }

    if (s_ob.sel == 1) {
        onboarding_finish();
        return;
    }

    if (app_net_prov_active()) {
        onboarding_finish();
    } else if (s_prov_busy) {
        ui_hint_flash("热点正在开启，请稍候…", 1500);
    } else {
        // 先置忙再建任务，避免任务跑完把忙标志清掉后才被这里重新置上。
        s_prov_busy = true;
        s_prov_cancel = false;
        s_prov_err = 0;
        if (xTaskCreate(prov_start_task, "ob_prov", 4096, NULL, 5, NULL) != pdPASS) {
            s_prov_busy = false;
            ui_hint_flash("无法开启热点，请稍后重试", 1800);
        }
        onboarding_render();
    }
}

void onboarding_open(void)
{
    if (s_ob.ov) return;

    memset(&s_ob, 0, sizeof(s_ob));
    s_ob.step = 0;
    s_ob.sel = 0;
    s_ob.prov_started = app_net_prov_active();

    // 浮层只压暗内容区：状态栏继续显示时间，提示条继续显示按键约定。
    s_ob.ov = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(s_ob.ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ob.ov, 0, UI_STATUS_H);
    lv_obj_set_size(s_ob.ov, UI_W, UI_H - UI_STATUS_H - UI_HINT_H);
    lv_obj_set_style_bg_color(s_ob.ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ob.ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_ob.ov, 0, 0);
    lv_obj_set_style_radius(s_ob.ov, 0, 0);
    lv_obj_set_style_pad_all(s_ob.ov, 0, 0);

    onboarding_render();
}

void onboarding_tick(void)
{
    if (!s_ob.ov || ui_timeedit_active()) return;
    if (s_ob.step != 2) return;

    // 热点是异步拉起的：状态变化后重画一次，把地址与密码显示出来。
    bool active = app_net_prov_active();
    if (active != s_ob.prov_started) {
        s_ob.prov_started = active;
        onboarding_render();
    }
}

void onboarding_close(void)
{
    if (!s_ob.ov) return;
    lv_obj_delete(s_ob.ov);
    memset(&s_ob, 0, sizeof(s_ob));
}

bool onboarding_active(void)
{
    return s_ob.ov != NULL;
}

void onboarding_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_ob.ov) return;

    if (ui_timeedit_active()) {
        ui_timeedit_handle(btn, ev);
        return;
    }

    // 长按 OK：跳当前步骤，引导只出现一次，用户不该被卡住。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        onboarding_advance();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) onboarding_focus(s_ob.sel - 1);
    else if (btn == BSP_BTN_DOWN) onboarding_focus(s_ob.sel + 1);
    else if (btn == BSP_BTN_OK) onboarding_activate();
}
