// main/app_state.h —— 应用运行态与持久化数据的唯一入口。
//
// 页面、网络任务与后台逻辑都通过本模块读写数据，不各自打开 NVS、也不各自缓存副本。
// 这样"清除数据"只需要清一处，"重启后恢复"也只需要载入一处。
//
// 线程约定：本模块的函数可在任意任务调用；NVS 写入内部串行化。返回值不使用全局
// 可变静态缓冲，避免两个任务同时取数据时互相覆盖。
#pragma once

#include "logic/app_badge.h"
#include "logic/app_esports.h"
#include "logic/app_pomodoro.h"
#include "logic/app_reminder.h"
#include "logic/app_routine.h"
#include "logic/app_time.h"
#include "logic/app_totp.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_NET_OFF = 0,
    APP_NET_CONNECTING,
    APP_NET_ONLINE,
} app_net_state_t;

typedef enum {
    APP_THEME_FIXED_DARK = 0,
    APP_THEME_FIXED_LIGHT,
    APP_THEME_AUTO,
} app_theme_choice_t;

typedef enum {
    APP_TIMEOUT_NEVER = 0,   // 常亮
    APP_TIMEOUT_15S,
    APP_TIMEOUT_30S,
    APP_TIMEOUT_1M,
    APP_TIMEOUT_2M,
    APP_TIMEOUT_5M,
    APP_TIMEOUT_COUNT,
} app_timeout_t;

// 数据分类，用于分类清除与备份导出。
typedef enum {
    APP_DATA_BADGES = 0,
    APP_DATA_ROUTINE,
    APP_DATA_TOTP,
    APP_DATA_REMINDERS,
    APP_DATA_ESPORTS,
    APP_DATA_SETTINGS,
    APP_DATA_ALL,
} app_data_kind_t;

typedef struct {
    app_theme_choice_t theme;
    app_timeout_t      screen_timeout;
    uint8_t            backlight;        // 10..100
    bool               sound_muted;
    uint8_t            volume;           // 0..100
    bool               power_save;       // 省电选项：低电量时才允许自动缩短息屏
    int                utc_offset_minutes;
    bool               time_synced;      // 已通过 NTP 或手机校准
    char               time_source[16];  // "NTP" / "手机" / "手动" / "未设置"
    int                birth_year;       // 0 表示未设置（时间进度可跳过人生尺度）
    int                birth_month;
    int                birth_day;
    int                life_expectancy;  // 默认 80
    bool               onboarded;        // 首次引导是否已完成
    bool               boarding;         // 作息模板：true 住校 / false 走读
    bool               use_odd_week;     // 是否启用单双周作息
    int                home_focus;       // 主页模块焦点，重启后恢复
    int                last_reminder_utc; // 上次提醒检查的 Unix 秒，用于开机汇总错过的提醒
} app_settings_t;

// ---- 生命周期 ----
// 初始化 NVS 并载入全部数据。重复调用是幂等的。失败时返回错误，调用方应提示并继续
// 以默认值运行（离线工具箱不应因为 NVS 读失败而完全不可用）。
esp_err_t app_state_init(void);

// ---- 时间 ----
// 当前本地时间。时间未校准时给出的是设备上电后的累计时间，页面需依据
// app_state_settings()->time_synced 显示警示。
app_datetime_t app_state_now(void);
uint64_t       app_state_now_unix(void);
void           app_state_set_time(const app_datetime_t *dt, const char *source);

// ---- 电量与网络 ----
int  app_state_battery_soc(void);                 // 0..100；读取失败返回 -1
void app_state_battery_refresh(void);
void app_state_set_net(app_net_state_t state, const char *ssid);
app_net_state_t app_state_net(void);
const char *app_state_net_ssid(void);

// ---- 数据访问 ----
app_settings_t     *app_state_settings(void);
app_badge_list_t   *app_state_badges(void);
app_routine_t      *app_state_routine(void);
app_reminder_list_t *app_state_reminders(void);
app_pomodoro_t     *app_state_pomodoro(void);
app_esport_cache_t *app_state_esports(void);

// ---- 作息取表 ----
// 当前应使用的套别：0 单周 / 1 双周。未启用单双周时恒为 0。
int app_state_routine_slot(void);
// 指定星期在指定套别下的作息表；可直接修改后调用 app_state_save_routine 落盘。
app_routine_day_t *app_state_routine_day_slot(int weekday, int slot);
// 指定星期在当前套别下的作息表。界面日常读写都用它，单双周切换对界面透明。
app_routine_day_t *app_state_routine_day(int weekday);

// 动态口令账户表：返回实际条数，accounts 指针由调用方持有。
int  app_state_totp_count(void);
app_totp_account_t *app_state_totp_at(int index);
// 新增账户，返回索引；满返回 -1。
int  app_state_totp_add(const app_totp_account_t *account);
bool app_state_totp_remove(int index);

// 当前选中的工牌下标（0..count-1），无工牌返回 -1。
int  app_state_badge_selected(void);

// ---- 持久化 ----
void app_state_save_settings(void);
void app_state_save_badges(void);
void app_state_save_routine(void);
void app_state_save_totp(void);
void app_state_save_reminders(void);
void app_state_save_pomodoro(void);
void app_state_save_esports(void);
void app_state_save_all(void);

// 分类清除。清除前由界面层完成二次确认。
void app_state_clear(app_data_kind_t kind);
