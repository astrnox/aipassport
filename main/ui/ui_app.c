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
#include "ui_pet.h"
#include "ui_sound.h"
#include "ui_theme.h"
#include "ui_timeedit.h"

#include "app_metrics.h"
#include "app_state.h"
#include "logic/app_esports.h"

#include "net/app_net.h"

#include "bsp_display.h"

#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "ui_app";

static const ui_module_t MODULES[] = {
    { "时间与日历", page_time_enter, page_time_exit, page_time_key, page_time_tick },
    { "专注与效率", page_focus_enter, page_focus_exit, page_focus_key, page_focus_tick },
    { "作息与倒计时", page_routine_enter, page_routine_exit, page_routine_key, page_routine_tick },
    { "身份", page_identity_enter, page_identity_exit, page_identity_key, page_identity_tick },
    { "工具", page_tools_enter, page_tools_exit, page_tools_key, page_tools_tick },
    { "英雄联盟赛事中心", page_esports_enter, page_esports_exit, page_esports_key, page_esports_tick },
    { "小说", page_novel_enter, page_novel_exit, page_novel_key, page_novel_tick },
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

// 低电量只在本次开机第一次跌破阈值时告诉桌宠一次，避免电量在阈值附近抖动时反复触发。
static bool s_low_batt_reported;

// 上次把墙钟落盘时所处的分钟。设备无 RTC 备份电池，运行期必须周期性把当前时间写回
// NVS，否则重启后时钟会停在上一次写盘的时刻（= 上次校时那一刻），关机时长全部丢失。
static int s_clock_saved_minute = -1;

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
    // 息屏期间跳过了状态栏的周期刷新（见 app_tick），这里补一次，避免唤醒后短暂
    // 显示息屏前的旧电量或旧 LIVE 角标。调用点均在 LVGL 锁内，可直接触碰控件。
    app_state_battery_refresh();
    ui_app_refresh_status();
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

// 设置页与快捷面板只把新值写进 NVS，真正"全局生效"落在这一处：主题调色板、屏幕
// 亮度、音量。开机与每次重建主页都会调用，因此改完设置重启或返回主页后都一致。
void ui_app_apply_settings(void)
{
    app_settings_t *st = app_state_settings();

    // 主题：固定明/暗直接用，自动档按当前本地小时判断。页面配色是构建时从调色板取的，
    // 所以调用方必须紧接着重建页面（见 ui_app_go_home 与快捷面板关闭处）。
    if (st->theme == APP_THEME_FIXED_LIGHT) {
        ui_theme_set(UI_THEME_LIGHT);
    } else if (st->theme == APP_THEME_AUTO) {
        ui_theme_apply_auto(true, app_state_now().hour);
    } else {
        ui_theme_set(UI_THEME_DARK);
    }

    // 亮度：越界值（损坏或旧版本的 blob）回落到全亮——屏幕是唯一界面载体，
    // 存成 0 会让人以为设备坏了。正常值域是 10..100。
    uint8_t bl = st->backlight;
    if (bl < 10 || bl > 100) bl = 100;
    bsp_display_backlight(bl);

    // 静音/音量平时由每次发声前应用，这里开机也落一次，保证第一条提示音就守设置。
    ui_sound_apply_volume();
}

static void teardown_current(void)
{
    // 弹层挂在当前页面的屏幕上，页面屏幕删除前必须先收掉，否则 s_alert 会留下悬空指针。
    ui_alert_close();
    if (s_current >= 0) {
        // 先记下采样标签（标题），再销毁页面——销毁之后 LVGL 池的回收才反映在这个点。
        char label[64];
        snprintf(label, sizeof(label), "离开:%s", MODULES[s_current].title);
        MODULES[s_current].exit();
        s_current = -1;
        app_metrics_mem(label);
    } else {
        page_home_exit();
    }
}

void ui_app_go_home(void)
{
    if (s_current < 0) return;
    teardown_current();
    // 重建主页之前先应用设置：设置页里改过的主题与亮度在这一刻才真正生效。
    ui_app_apply_settings();
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

    // 进入该页后的内存/栈水位采样（内存优化阶段 1）。
    char label[64];
    snprintf(label, sizeof(label), "进入:%s", MODULES[index].title);
    app_metrics_report(label);
}

void ui_app_show_quick_panel(void) { home_quick_open(); }
void ui_app_show_onboarding(void)  { onboarding_open(); }

// ---------------------------------------------------------------------------
// 全局节拍
// ---------------------------------------------------------------------------

static void advance_pomodoro(void)
{
    app_pomodoro_t *p = app_state_pomodoro();

    // 本地日期按 year*10000+month*100+day 每拍喂一次：跨零点时今日统计归零，永久累计不动。
    // 调用很廉价，无需额外缓存日期。
    app_datetime_t now = app_state_now();
    app_pomodoro_roll_day(p, (uint32_t)(now.year * 10000 + now.month * 100 + now.day));

    // 长休息也要推进，因此用 tick 的返回值判断阶段切换，而不是先看状态。
    // 桌宠跟着这些切换做反应：进入专注、专注结束、休息结束各有不同表现。进入专注这一步
    // 没有 tick 事件（用户按开始键时状态就跳过去了），只能靠 tick 前后的阶段对比捕捉；
    // 并且必须限定在 ev == NONE，否则"休息结束自动接续专注"会被同时当成开始与结束两件事。
    app_pomo_state_t before = app_pomodoro_phase(p);
    app_pomo_event_t ev = app_pomodoro_tick(p, 1);
    app_pomo_state_t after = app_pomodoro_phase(p);
    if (ev == APP_POMO_EVENT_NONE && before != APP_POMO_FOCUS && after == APP_POMO_FOCUS) {
        ui_pet_event(APP_PET_EV_FOCUS_ON);
    }
    if (ev != APP_POMO_EVENT_NONE) {
        // 阶段切换：写入一次持久化，让重启后能回到正确的段；同时给出提示音与桌宠反应。
        app_state_save_pomodoro();
        ui_sound_beep();
        ui_pet_event(ev == APP_POMO_EVENT_FOCUS_DONE ? APP_PET_EV_FOCUS_DONE
                                                     : APP_PET_EV_BREAK_DONE);
    }
}

// 提醒到点。按分钟去重，否则同一条提醒会在它那一分钟里每秒响一次。
//
// 提示形式：提示音 + 一层"知道了"弹层。熄屏时先唤醒屏幕，否则闹钟响了用户看不到。
// 若此刻正在引导或快捷面板上，只保留提示音——不叠第三层浮层，避免按键归属混乱。
//
// 免打扰（番茄钟专注段）期间不响也不弹，改为记下来，等这一段专注结束再一次性告知：
// 静音不等于把消息丢掉。
static bool dnd_active(void)
{
    return app_pomodoro_dnd_active(app_state_pomodoro());
}

// 免打扰期间被压下的提醒。只记条数与最近一条，够说明情况又不必开一个队列。
static int  s_dnd_held;
static char s_dnd_held_last[40];

static void dnd_hold_reminder(const app_reminder_t *r)
{
    if (s_dnd_held == 0) {
        snprintf(s_dnd_held_last, sizeof(s_dnd_held_last), "%02d:%02d %s",
                 r->hour, r->minute, r->label[0] ? r->label : "未命名");
    }
    s_dnd_held++;
}

// 专注结束后把压下的提醒说清楚。界面正忙（弹层/编辑中）就先不说，下一拍再试，
// 免得把用户正在做的操作顶掉、又让消息悄悄消失。
static void dnd_report_held(void)
{
    if (s_dnd_held <= 0) return;
    if (ui_timeedit_active() || ui_dialog_is_open() || ui_alert_is_open()) return;
    if (onboarding_active() || home_quick_active()) return;

    char body[96];
    snprintf(body, sizeof(body), "专注期间静音了 %d 条提醒，最近一条 %s",
             s_dnd_held, s_dnd_held_last);
    s_dnd_held = 0;
    ESP_LOGI(TAG, "%s", body);

    if (s_asleep) wake_now();
    ui_sound_beep();
    ui_alert_open(lv_screen_active(), "免打扰结束", body);
}

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

    if (dnd_active()) {
        dnd_hold_reminder(due);
        ESP_LOGI(TAG, "免打扰：暂缓提醒 %s", due->label[0] ? due->label : "未命名");
        return;
    }

    ESP_LOGI(TAG, "提醒到点: %s", due->label[0] ? due->label : "未命名");

    // 提醒是"必须让用户看到"的信息：先唤醒屏幕，再发声、弹层。
    // 静音或音量为 0 时 ui_sound_beep() 自身不会出声，但弹层照旧——提示音只是辅助，
    // 不能因为静音就把整条提醒吞掉。以前这里还会因为快捷面板/引导浮层打开而直接
    // return，连弹层都不给；现在只让开引导时不打断（用户就在屏前），其余情况都把
    // 会遮住提醒的临时浮层收掉后弹出。
    if (s_asleep) wake_now();
    ui_sound_beep();
    ui_pet_event(APP_PET_EV_REMINDER);
    if (onboarding_active()) return;
    if (home_quick_active()) home_quick_close();

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
    // 与 check_reminders 同一约定：息屏时不"默默"弹层——校时可能发生在息屏之后，
    // 先唤醒再弹，既让用户看到，也避免在背光熄灭时白白重绘一帧。
    if (s_asleep) wake_now();
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
    // 免打扰期间连"错过的提醒"也不弹：它同样是一种打断，等专注结束再说。
    if (dnd_active()) return;

    s_missed_reported = true;
    report_missed_reminders();
}

// 作息节点切换提示。节拍在任何页面都跑，所以停在主页或熄屏时也能听到节点开始。
static void check_routine_node(void)
{
    app_datetime_t now = app_state_now();
    int wd = app_time_weekday(now.year, now.month, now.day);
    const app_routine_day_t *day = app_state_routine_day(wd);

    if (!day || day->count <= 0) {
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
    // 进入空档不提示，只有真正开始一个新节点才响；免打扰时不打断专注。
    if (index >= 0 && !dnd_active()) ui_sound_beep();
}

// 低电量提醒：桌宠发抖一下，提示该充电了。与息屏策略里的低电量阈值共用同一个常量，
// 免得"省电提前息屏"和"桌宠反应"对低电量的定义不一致。
static void check_low_battery(void)
{
    if (s_low_batt_reported) return;
    int soc = app_state_battery_soc();
    if (soc < 0 || soc >= APP_BATTERY_LOW_PCT) return;
    s_low_batt_reported = true;
    ui_pet_event(APP_PET_EV_LOW_BATTERY);
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
    // 免打扰结束后补报期间压下的提醒。放在这里而不是阶段切换的分支里，是因为弹层
    // 冲突时它需要等界面空下来再报。
    if (!dnd_active()) dnd_report_held();

    // 每分钟把墙钟落盘一次：设备没有 RTC 备份电池，重启后只能从上次落盘时刻接着走，
    // 落盘间隔直接决定重启后的最大时间误差。只在时间已知时写，避免把占位基准写进 NVS。
    if (app_state_time_known()) {
        int clock_minute = (int)(app_state_now_unix() / 60);
        if (clock_minute != s_clock_saved_minute) {
            s_clock_saved_minute = clock_minute;
            app_state_save_clock();
        }
    }

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

    // 息屏时不刷新状态栏：背光已灭，重绘看不见，却要让 CPU 跑一趟 LVGL 失效与
    // SPI 输出。省电阶段 1 的核心就是去掉这段无用功；唤醒时在 wake_now 补刷一次，
    // 所以醒着时状态栏不会停在旧值。
    if (!s_asleep && s_tick_count % 15 == 0) {
        app_state_battery_refresh();
        ui_app_refresh_status();
    }

    // 每 60 秒一次汇总采样：内存快照 + 输入任务与 LVGL 任务的栈高水位。周期性而非
    // 仅页面切换时采样，是为了覆盖"长时间停留/反复操作"后的缓慢泄漏与栈峰值。
    if (s_tick_count % 60 == 0) app_metrics_report("周期");

    bsp_lvgl_unlock();
}

void ui_app_start(void)
{
    if (s_started) return;
    s_started = true;

    // 开机先按设置落主题与亮度，再建主页：主页是第一个可见页面，必须一开始就是
    // 用户设置的样子（以前亮度固定 100%，重启后设置就丢了）。
    ui_app_apply_settings();

    page_home_enter();
    ui_app_refresh_status();

    if (!app_state_settings()->onboarded) {
        onboarding_open();
    }

    s_tick = lv_timer_create(app_tick, 1000, NULL);

    // 重启后墙钟只到"上次落盘时刻"，关机时长不计入，因此只要有 Wi-Fi 凭证就开机自动
    // 校时一次，让时间在联网后自我纠正。校时在 app_net 的 worker 里执行，不阻塞界面，
    // 且结束即释放射频，不构成常驻联网。失败时设置页会照常显示"上次校时失败"。
    if (app_net_has_credentials()) app_net_time_sync_request();

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

    // 通知弹层（提醒到点等）要能被任何浮层之上的按键关掉：它可能是在快捷面板或
    // 某个对话框打开时弹出来的，所以优先于它们处理。以前它排在最后，一旦在快捷
    // 面板上弹了提醒，按键会被面板吃掉、提醒关不掉。
    if (ui_alert_is_open()) {
        ui_alert_handle(btn, ev);
        bsp_lvgl_unlock();
        return;
    }
    if (onboarding_active()) {
        onboarding_key(btn, ev);
        bsp_lvgl_unlock();
        return;
    }
    if (home_quick_active()) {
        app_theme_choice_t theme_before = app_state_settings()->theme;
        home_quick_key(btn, ev);
        // 快捷面板挂在主页上，切主题只写了设置值；面板关掉后必须重建主页才会出现新
        // 配色——面板不是页面，主页不会自己重建，"返回后生效"就永远等不到。
        if (!home_quick_active() && app_state_settings()->theme != theme_before) {
            ui_app_apply_settings();
            page_home_exit();
            page_home_enter();
            ui_app_refresh_status();
        }
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
