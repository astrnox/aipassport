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
#include "ui_theme.h"

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

int ui_app_module_count(void) { return MODULE_COUNT; }

const char *ui_app_module_title(int index)
{
    if (index < 0 || index >= MODULE_COUNT) return "";
    return MODULES[index].title;
}

static int timeout_seconds(void)
{
    switch (app_state_settings()->screen_timeout) {
    case APP_TIMEOUT_15S: return 15;
    case APP_TIMEOUT_30S: return 30;
    case APP_TIMEOUT_1M:  return 60;
    case APP_TIMEOUT_2M:  return 120;
    case APP_TIMEOUT_5M:  return 300;
    case APP_TIMEOUT_NEVER:
    default:              return 0;
    }
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
            // 阶段切换：写入一次持久化，让重启后能回到正确的段。
            app_state_save_pomodoro();
        }
    }
}

static void check_reminders(void)
{
    app_reminder_list_t *list = app_state_reminders();
    if (list->count <= 0) return;
    app_datetime_t now = app_state_now();
    int weekday = app_time_weekday(now.year, now.month, now.day);
    for (int i = 0; i < list->count; i++) {
        if (app_reminder_due(&list->items[i], &now, weekday)) {
            // 到点提示：当前以提示条与可选提示音呈现，由页面 tick 读取。
            ESP_LOGI(TAG, "提醒到点: %s", list->items[i].label);
        }
    }
}

static void app_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!bsp_lvgl_lock(50)) return;

    s_tick_count++;
    advance_pomodoro();
    check_reminders();

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
