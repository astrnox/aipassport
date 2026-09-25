// main/ui/ui_app.c —— 应用控制器：导航、息屏、全局节拍与状态栏。
//
// 三个按键的全局约定在这里落地：
//   主页   UP/DOWN 移动模块焦点，OK 进入，长按 UP 快捷面板，长按 OK 熄屏
//   模块页 由页面自定义；长按 OK 在页面根视图返回主页
//   熄屏   任意键仅唤醒并回到熄屏前页面
// 番茄钟、提醒调度与作息节点切换都在本文件的 1 秒节拍里推进，因此它们不依赖
// 当前停留在哪个页面，熄屏时也继续运行。
#include "ui_app.h"

#include "ui_pages.h"
#include "ui_sound.h"
#include "ui_theme.h"
#include "ui_timeedit.h"

#include "app_state.h"
#include "logic/app_esports.h"

#include "bsp_display.h"

#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "ui_app";

static const ui_module_t MODULES[] = {
    { "时间与日历", page_time_enter, page_time_exit, page_time_key, page_time_tick },
    { "专注与效率", page_focus_enter, page_focus_exit, page_focus_key, page_focus_tick },
    { "作息与倒计时", page_routine_enter, page_routine_exit, page_routine_key, page_routine_tick },
    { "身份与工具", page_identity_enter, page_identity_exit, page_identity_key, page_identity_tick },
    { "英雄联盟赛事中心", page_esports_enter, page_esports_exit, page_esports_key, page_esports_tick },
    { "系统设置", page_settings_enter, page_settings_exit, page_settings_key, page_settings_tick },
};
#define MODULE_COUNT ((int)(sizeof(MODULES) / sizeof(MODULES[0])))

static int s_current = -1;        // -1 = 主页
static bool s_asleep;
static bool s_started;
static int s_idle_seconds;
static int s_tick_count;
static lv_timer_t *s_tick;

// 提醒与作息节点切换的去重状态，避免同一分钟重复提示。
static int s_reminder_minute = -1;
static int s_routine_node = -1;
static bool s_routine_seen;

// 错过提醒只在本次开机、且时间重新校准后汇总一次。
static bool s_missed_reported;

int ui_app_module_count(void) { return MODULE_COUNT; }

const char *ui_app_module_title(int index)
{
    if (index < 0 || index >= MODULE_COUNT) return "";
    return MODULES[index].title;
}

// 低电量阈值与省电上限。低电量只在用户已开启省电选项时才缩短息屏，且只改本函数的
// 返回值，不动用户选定的档位——电量回升或关掉省电就自动回到用户设置。
#define APP_BATTERY_LOW_PCT        20
#define APP_POWERSAVE_LIMIT_S      30
#define APP_POWERSAVE_NEVER_LIMIT_S 60

static int timeout_seconds(void)
{
    app_settings_t *st = app_state_settings();
    int limit;
    switch (st->screen_timeout) {
    case APP_TIMEOUT_15S: limit = 15;  break;
    case APP_TIMEOUT_30S: limit = 30;  break;
    case APP_TIMEOUT_1M:  limit = 60;  break;
    case APP_TIMEOUT_2M:  limit = 120; break;
    case APP_TIMEOUT_5M:  limit = 300; break;
    case APP_TIMEOUT_NEVER:
    default:              limit = 0;   break;
    }

    if (st->power_save) {
        int soc = app_state_battery_soc();
        if (soc >= 0 && soc < APP_BATTERY_LOW_PCT) {
            // 常亮本身没有上限，低电量时给它一个上限；其余档位只压低、不抬高。
            int cap = (limit == 0) ? APP_POWERSAVE_NEVER_LIMIT_S : APP_POWERSAVE_LIMIT_S;
            if (limit == 0 || limit > cap) limit = cap;
        }
    }
    return limit;
}

void ui_app_note_activity(void)
{
    s_idle_seconds = 0;
}

bool ui_app_is_asleep(void) { return s_asleep; }

static void sleep_now(void)
{
    s_asleep = true;
    bsp_display_backlight(0);
    ESP_LOGI(TAG, "息屏");
}

static void wake_now(void)
{
    s_asleep = false;
    s_idle_seconds = 0;
    bsp_display_backlight(app_state_settings()->backlight);
    ESP_LOGI(TAG, "唤醒");
}

void ui_app_refresh_status(void)
{
    app_esport_cache_t *cache = app_state_esports();
    int now_utc = (int)app_state_now_unix();

    const char *badge = NULL;
    bool live = false;
    if (cache->valid && cache->match_count > 0) {
        int pick = app_esport_home_pick(cache->matches, cache->match_count, now_utc);
        if (pick >= 0) {
            const app_esport_match_t *m = &cache->matches[pick];
            if (m->state == APP_MATCH_LIVE) {
                badge = "LIVE";
                live = true;
            } else if (app_esport_priority(m, now_utc) == 1) {
                badge = "即将";
            }
        }
    }
    ui_status_bar_set_badge(badge, live);

    app_net_state_t net = app_state_net();
    if (net == APP_NET_ONLINE) {
        ui_status_bar_set_net("在线", true);
    } else if (net == APP_NET_CONNECTING) {
        ui_status_bar_set_net("连接中", false);
    } else {
        ui_status_bar_set_net(NULL, false);
    }
    ui_status_bar_refresh();
}

void ui_app_refresh_home(void)
{
    if (s_current < 0) page_home_tick();
    ui_app_refresh_status();
}

// ---------------------------------------------------------------------------
// 导航
// ---------------------------------------------------------------------------

static void teardown_current(void)
{
    // 弹层挂在当前页面的屏幕上，页面屏幕删除前必须先收掉，否则 s_alert 会留下悬空指针。
    ui_alert_close();
    if (s_current >= 0) {
        MODULES[s_current].exit();
        s_current = -1;
    } else {
        page_home_exit();
    }
}

void ui_app_go_home(void)
{
    if (s_current < 0) return;
    teardown_current();
    page_home_enter();
    ui_app_refresh_status();
}

void ui_app_open_module(int index)
{
    if (index < 0 || index >= MODULE_COUNT) return;
    teardown_current();
    s_current = index;
    MODULES[index].enter();
    ui_app_note_activity();
    ui_app_refresh_status();
}

void ui_app_show_quick_panel(void) { home_quick_open(); }
void ui_app_show_onboarding(void)  { onboarding_open(); }

// ---------------------------------------------------------------------------
// 全局节拍
// ---------------------------------------------------------------------------

static void advance_pomodoro(void)
{
    app_pomodoro_t *p = app_state_pomodoro();
    if (p->state == APP_POMO_FOCUS || p->state == APP_POMO_BREAK) {
        if (app_pomodoro_tick(p, 1)) {
            // 阶段切换：写入一次持久化，让重启后能回到正确的段；同时给出提示音。
            app_state_save_pomodoro();
            ui_sound_beep();
        }
    }
}

// 提醒到点。按分钟去重，否则同一条提醒会在它那一分钟里每秒响一次。
//
// 提示形式：提示音 + 一层"知道了"弹层。熄屏时先唤醒屏幕，否则闹钟响了用户看不到。
// 若此刻正在引导或快捷面板上，只保留提示音——不叠第三层浮层，避免按键归属混乱。
static void check_reminders(void)
{
    app_reminder_list_t *list = app_state_reminders();
    if (list->count <= 0) return;

    app_datetime_t now = app_state_now();
    int weekday = app_time_weekday(now.year, now.month, now.day);

    const app_reminder_t *due = NULL;
    for (int i = 0; i < list->count; i++) {
        if (app_reminder_due(&list->items[i], &now, weekday)) {
            due = &list->items[i];
            break;
        }
    }
    if (!due) return;

    int minute = (int)(app_state_now_unix() / 60);
    if (s_reminder_minute == minute) return;
    s_reminder_minute = minute;

    ESP_LOGI(TAG, "提醒到点: %s", due->label[0] ? due->label : "未命名");
    ui_sound_beep();

    if (s_asleep) wake_now();
    if (onboarding_active() || home_quick_active() || ui_alert_is_open()) return;

    char body[64];
    if (due->label[0]) {
        snprintf(body, sizeof(body), "%02d:%02d  %s", due->hour, due->minute, due->label);
    } else {
        snprintf(body, sizeof(body), "%02d:%02d  该做这件事了", due->hour, due->minute);
    }
    ui_alert_open(lv_screen_active(), "提醒", body);
}

// 把"最后一次确认提醒"的时间写入持久存储。运行期按间隔节流，只在提醒真正响过或
// 跨越节流窗口时落盘，避免每秒写一次 NVS。
#define REMINDER_MARK_INTERVAL_S 300

static void mark_reminders_checked(bool force)
{
    app_settings_t *st = app_state_settings();
    int now = (int)app_state_now_unix();
    if (now <= 0) return;
    if (!force && st->last_reminder_utc > 0 &&
        now - st->last_reminder_utc < REMINDER_MARK_INTERVAL_S) {
        return;
    }
    st->last_reminder_utc = now;
    app_state_save_settings();
}

// 开机汇总错过的提醒。设备关机期间提醒不响，因此校时后把时间重新对准，再把
// (last_reminder_utc, now] 之间本应触发的提醒一次性列出来告知用户。
static void report_missed_reminders(void)
{
    app_settings_t *st = app_state_settings();
    int now = (int)app_state_now_unix();
    if (now <= 0) return;

    char times[APP_REMINDER_MAX][6];
    int missed = app_reminder_missed(app_state_reminders(), st->last_reminder_utc, now,
                                     st->utc_offset_minutes, times, APP_REMINDER_MAX);
    mark_reminders_checked(true);
    if (missed <= 0) return;

    // 当前这一分钟已并入汇总，避免弹层关掉后同一分钟再响一次。
    s_reminder_minute = now / 60;

    char list_text[128];
    int used = 0;
    for (int i = 0; i < missed && used < (int)sizeof(list_text) - 8; i++) {
        used += snprintf(list_text + used, sizeof(list_text) - (size_t)used,
                         "%s%s", i ? " " : "", times[i]);
    }

    char body[192];
    snprintf(body, sizeof(body), "关机期间错过 %d 条提醒：%s", missed, list_text);
    ESP_LOGI(TAG, "错过提醒 %d 条: %s", missed, list_text);
    ui_sound_beep();
    if (ui_alert_is_open()) ui_alert_close();
    ui_alert_open(lv_screen_active(), "错过的提醒", body);
}

// 时间校准是"现在几点"生效的时机，错过提醒的汇总必须等到这一刻才有意义。
static void tick_missed_reminders(void)
{
    if (s_missed_reported) return;
    if (!app_state_settings()->time_synced) return;
    // 用户正在编辑或确认其它内容时不打断，下一拍再汇总。
    if (ui_timeedit_active() || ui_dialog_is_open() || ui_alert_is_open()) return;
    if (onboarding_active() || home_quick_active()) return;

    s_missed_reported = true;
    report_missed_reminders();
}

// 作息节点切换提示。节拍在任何页面都跑，所以停在主页或熄屏时也能听到节点开始。
static void check_routine_node(void)
{
    app_datetime_t now = app_state_now();
    int wd = app_time_weekday(now.year, now.month, now.day);
    const app_routine_day_t *day = &app_state_routine()->days[wd];

    if (day->count <= 0) {
        s_routine_node = -1;
        s_routine_seen = false;
        return;
    }

    app_routine_status_t st;
    app_routine_status(day, now.hour * 60 + now.minute, now.second, &st);
    int index = (st.pos == APP_ROUTINE_IN_NODE) ? st.current_index : -1;

    if (!s_routine_seen) {
        // 首次采样只记录不补响，否则开机进页面就会无缘无故响一声。
        s_routine_seen = true;
        s_routine_node = index;
        return;
    }
    if (index == s_routine_node) return;

    s_routine_node = index;
    // 进入空档不提示，只有真正开始一个新节点才响。
    if (index >= 0) ui_sound_beep();
}

static void app_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!bsp_lvgl_lock(50)) return;

    s_tick_count++;
    advance_pomodoro();
    check_reminders();
    check_routine_node();
    tick_missed_reminders();

    if (!s_asleep) {
        int limit = timeout_seconds();
        if (limit > 0 && ++s_idle_seconds >= limit) {
            sleep_now();
        }
    }

    if (!s_asleep) {
        // 引导浮层盖在当前页之上，且可能在主页或设置页打开，所以单独走它自己的节拍。
        if (onboarding_active()) onboarding_tick();
        else if (s_current < 0) page_home_tick();
        else MODULES[s_current].tick();
    }

    if (s_tick_count % 15 == 0) {
        app_state_battery_refresh();
        ui_app_refresh_status();
    }

    bsp_lvgl_unlock();
}

void ui_app_start(void)
{
    if (s_started) return;
    s_started = true;

    page_home_enter();
    ui_app_refresh_status();

    if (!app_state_settings()->onboarded) {
        onboarding_open();
    }

    s_tick = lv_timer_create(app_tick, 1000, NULL);
    ESP_LOGI(TAG, "界面控制器就绪");
}

// ---------------------------------------------------------------------------
// 按键分发
// ---------------------------------------------------------------------------

void ui_app_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!bsp_lvgl_lock(200)) return;

    // 熄屏后第一次按键只唤醒，不触发常规动作。
    if (s_asleep) {
        wake_now();
        bsp_lvgl_unlock();
        return;
    }
    ui_app_note_activity();

    if (onboarding_active()) {
        onboarding_key(btn, ev);
        bsp_lvgl_unlock();
        return;
    }
    if (home_quick_active()) {
        home_quick_key(btn, ev);
        bsp_lvgl_unlock();
        return;
    }
    if (ui_alert_is_open()) {
        ui_alert_handle(btn, ev);
        bsp_lvgl_unlock();
        return;
    }
    if (ui_dialog_is_open()) {
        ui_dialog_handle(btn, ev);
        bsp_lvgl_unlock();
        return;
    }

    if (s_current < 0) {
        // 主页：长按 OK 熄屏、长按 UP 快捷面板，其余交给主页。
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            sleep_now();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            home_quick_open();
        } else {
            page_home_key(btn, ev);
        }
    } else {
        MODULES[s_current].key(btn, ev);
    }

    bsp_lvgl_unlock();
}
