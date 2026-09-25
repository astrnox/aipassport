// main/logic/app_pomodoro.h —— 番茄钟状态机（与硬件无关）。
//
// 本层只做状态推进与统计，不涉及定时器、蜂鸣器或界面；调用方负责按秒喂 tick，并根据
// tick 返回的阶段切换事件决定是否提示音、自动接续或停在空闲。
//
// 统计分三层，便于界面诚实地分开显示：
//   本次运行   completed_focus            本次开机以来完成的专注段数，重启归零
//   今日       focus_minutes_today/today_sessions  随本地日期滚动，跨天清零
//   历史累计   total_focus_sessions/minutes        跨开机持久，永不因跨天或停止归零
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_POMO_IDLE = 0,      // 空闲
    APP_POMO_FOCUS,         // 专注中
    APP_POMO_BREAK,         // 短休息
    APP_POMO_LONG_BREAK,    // 长休息
    APP_POMO_PAUSED,        // 已暂停
} app_pomo_state_t;

// 时长与循环次数上下限。界面与逻辑共用同一组数字，避免两处各写一份而逐渐走偏。
#define APP_POMO_FOCUS_MIN_MIN     1
#define APP_POMO_FOCUS_MIN_MAX     120
#define APP_POMO_BREAK_MIN_MIN     1
#define APP_POMO_BREAK_MIN_MAX     60
#define APP_POMO_LONG_MIN_MIN      1
#define APP_POMO_LONG_MIN_MAX      60
#define APP_POMO_CYCLES_MIN        2
#define APP_POMO_CYCLES_MAX        8

// tick 的返回值：描述"本次推进期间发生了什么阶段切换"。
typedef enum {
    APP_POMO_EVENT_NONE = 0,
    APP_POMO_EVENT_FOCUS_DONE,   // 专注结束，已切到短休息或长休息
    APP_POMO_EVENT_BREAK_DONE,   // 休息结束，已切到下一段专注
} app_pomo_event_t;

typedef struct {
    int focus_minutes;              // 默认 25
    int break_minutes;              // 默认 5
    int long_break_minutes;         // 默认 15
    int cycles_per_long_break;      // 默认 4，夹到 [2,8]
    bool auto_next;                 // 默认 true：阶段结束自动接续下一段
    app_pomo_state_t state;
    app_pomo_state_t paused_from;   // 暂停前的状态
    int remaining_seconds;
    int completed_focus;            // 本次运行（本次开机以来）完成的专注段数
    int focus_minutes_today;        // 今日累计专注分钟
    uint32_t stats_day;             // focus_minutes_today 所属本地日期，格式 YYYYMMDD；0 = 未记录
    int today_sessions;             // 今日完成的专注段数
    int total_focus_sessions;       // 历史累计完成的专注段数（跨开机持久）
    int total_focus_minutes;        // 历史累计专注分钟（跨开机持久）
    int cycles_since_long_break;    // 距上次长休息已完成了几段专注
} app_pomodoro_t;

// 复位为默认值：25 / 5 / 15 分钟、4 段一长休、自动接续，空闲，统计清零。
void app_pomodoro_init(app_pomodoro_t *p);

// 设置专注与短休息时长，分别夹到 1..120、1..60。仅空闲状态允许修改；
// 非空闲状态或数值（夹取后）无变化时返回 false。
bool app_pomodoro_set_durations(app_pomodoro_t *p, int focus_minutes, int break_minutes);

// 设置长休息时长与"几段专注后长休"，分别夹到 1..60、2..8。同样仅空闲状态允许修改。
bool app_pomodoro_set_long_break(app_pomodoro_t *p, int long_break_minutes, int cycles);

// 开关自动接续。它不影响当前段的长度，因此任何状态下都允许修改；无变化返回 false。
bool app_pomodoro_set_auto_next(app_pomodoro_t *p, bool enabled);

// 从空闲/暂停进入新的专注段。已在运行（专注/短休/长休）时返回 false。
bool app_pomodoro_start(app_pomodoro_t *p);

// 开始 / 暂停 / 继续：空闲 -> 专注；专注或休息 -> 暂停；暂停 -> 恢复原阶段。
bool app_pomodoro_toggle(app_pomodoro_t *p);

// 回到空闲并保留统计（今日、累计与本次运行计数都不清）。
void app_pomodoro_stop(app_pomodoro_t *p);

// 推进 seconds 秒。返回本次推进期间发生的阶段切换；未跨阶段返回 APP_POMO_EVENT_NONE。
app_pomo_event_t app_pomodoro_tick(app_pomodoro_t *p, int seconds);

// 本地日期滚动（date = year*10000 + month*100 + day）。首次记录只补日期不动数据；
// 换天后清空今日统计，历史累计始终保持。
void app_pomodoro_roll_day(app_pomodoro_t *p, uint32_t date);

// 当前段进度 0..1000；空闲返回 0。暂停时按暂停前所处阶段计算。
int app_pomodoro_progress_permille(const app_pomodoro_t *p);

// 当前阶段：暂停时取暂停前的阶段；空闲返回 APP_POMO_IDLE。
app_pomo_state_t app_pomodoro_phase(const app_pomodoro_t *p);

// 当前阶段的总秒数；空闲返回 0。暂停时按暂停前所处阶段计算。
int app_pomodoro_phase_seconds(const app_pomodoro_t *p);

// 空闲 / 专注中 / 短休息 / 长休息 / 已暂停；越界返回 "??"。
const char *app_pomodoro_state_name(app_pomo_state_t state);
