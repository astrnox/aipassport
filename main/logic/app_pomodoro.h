// main/logic/app_pomodoro.h —— 番茄钟状态机（与硬件无关）。
//
// 本层只做状态推进与统计，不涉及定时器、蜂鸣器或界面；调用方负责按秒喂 tick 并在
// 返回 true（发生阶段切换）时触发提示音。
#pragma once

#include <stdbool.h>

typedef enum {
    APP_POMO_IDLE = 0,   // 空闲
    APP_POMO_FOCUS,      // 专注中
    APP_POMO_BREAK,      // 休息中
    APP_POMO_PAUSED,     // 已暂停
} app_pomo_state_t;

typedef struct {
    int focus_minutes;              // 默认 25
    int break_minutes;              // 默认 5
    app_pomo_state_t state;
    app_pomo_state_t paused_from;   // 暂停前的状态
    int remaining_seconds;
    int completed_focus;            // 本次运行完成的专注段数
    int focus_minutes_today;        // 今日累计专注分钟
} app_pomodoro_t;

// 复位为默认值：25 / 5 分钟，空闲，统计清零。
void app_pomodoro_init(app_pomodoro_t *p);

// 设置时长，focus 夹到 1..120、break 夹到 1..60。仅空闲状态允许修改；
// 非空闲状态或数值无变化时返回 false。
bool app_pomodoro_set_durations(app_pomodoro_t *p, int focus_minutes, int break_minutes);

// 从空闲/暂停进入新的专注段。已在运行（专注/休息）时返回 false。
bool app_pomodoro_start(app_pomodoro_t *p);

// 开始 / 暂停 / 继续：空闲 -> 专注；专注或休息 -> 暂停；暂停 -> 恢复原阶段。
bool app_pomodoro_toggle(app_pomodoro_t *p);

// 回到空闲并保留今日统计。
void app_pomodoro_stop(app_pomodoro_t *p);

// 推进 seconds 秒。返回 true 表示本次 tick 内发生了阶段切换（用于触发提示音）。
bool app_pomodoro_tick(app_pomodoro_t *p, int seconds);

// 当前段进度 0..1000；空闲返回 0。
int app_pomodoro_progress_permille(const app_pomodoro_t *p);

// 空闲 / 专注中 / 休息中 / 已暂停；越界返回 "??"。
const char *app_pomodoro_state_name(app_pomo_state_t state);
