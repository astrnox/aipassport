// tests/test_app_pomodoro.c —— app_pomodoro 的主机侧单元测试。
//
// 覆盖：默认值、夹取、运行期拒绝改时长、短休息与长休息切换、关闭自动接续回到空闲、
// 三层统计的累加与停止保留、跨天滚动、暂停长休息的进度，以及 tick 对无效输入的忽略。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_pomodoro.h"

// 完整走完一段专注 + 一段休息，用于快速推进到下一个周期。
static void run_focus_and_break(app_pomodoro_t *p)
{
    assert(app_pomodoro_tick(p, p->focus_minutes * 60) == APP_POMO_EVENT_FOCUS_DONE);
    assert(app_pomodoro_tick(p, p->remaining_seconds) == APP_POMO_EVENT_BREAK_DONE);
}

int main(void)
{
    app_pomodoro_t p;

    // ---- 默认值 ----
    app_pomodoro_init(&p);
    assert(p.focus_minutes == 25);
    assert(p.break_minutes == 5);
    assert(p.long_break_minutes == 15);
    assert(p.cycles_per_long_break == 4);
    assert(p.auto_next == true);
    assert(p.state == APP_POMO_IDLE);
    assert(p.paused_from == APP_POMO_IDLE);
    assert(p.remaining_seconds == 0);
    assert(p.completed_focus == 0);
    assert(p.focus_minutes_today == 0);
    assert(p.stats_day == 0);
    assert(p.today_sessions == 0);
    assert(p.total_focus_sessions == 0);
    assert(p.total_focus_minutes == 0);
    assert(p.cycles_since_long_break == 0);
    assert(app_pomodoro_progress_permille(&p) == 0);
    assert(app_pomodoro_phase(&p) == APP_POMO_IDLE);
    assert(app_pomodoro_phase_seconds(&p) == 0);

    // ---- 时长设置与夹取 ----
    assert(!app_pomodoro_set_durations(&p, 25, 5));   // 无变化
    assert(app_pomodoro_set_durations(&p, 0, 0));     // 夹到下限
    assert(p.focus_minutes == 1);
    assert(p.break_minutes == 1);
    assert(app_pomodoro_set_durations(&p, 999, 999)); // 夹到上限
    assert(p.focus_minutes == 120);
    assert(p.break_minutes == 60);
    assert(!app_pomodoro_set_durations(&p, 999, 999)); // 夹取后仍无变化

    // ---- 长休息与循环次数设置与夹取 ----
    app_pomodoro_init(&p);
    assert(!app_pomodoro_set_long_break(&p, 15, 4));  // 无变化
    assert(app_pomodoro_set_long_break(&p, 0, 0));    // 夹到下限
    assert(p.long_break_minutes == 1);
    assert(p.cycles_per_long_break == 2);
    assert(app_pomodoro_set_long_break(&p, 999, 999)); // 夹到上限
    assert(p.long_break_minutes == 60);
    assert(p.cycles_per_long_break == 8);

    // ---- 自动接续开关 ----
    app_pomodoro_init(&p);
    assert(!app_pomodoro_set_auto_next(&p, true));    // 无变化
    assert(app_pomodoro_set_auto_next(&p, false));
    assert(p.auto_next == false);
    assert(app_pomodoro_set_auto_next(&p, true));

    // ---- 开始 / 暂停 / 继续 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_start(&p));
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 25 * 60);
    assert(!app_pomodoro_start(&p));                   // 已在运行
    assert(!app_pomodoro_set_durations(&p, 10, 10));   // 运行中禁止改时长
    assert(!app_pomodoro_set_long_break(&p, 10, 3));   // 运行中禁止改循环
    assert(app_pomodoro_set_auto_next(&p, false));     // 自动接续运行中可改
    assert(app_pomodoro_set_auto_next(&p, true));

    assert(app_pomodoro_toggle(&p));
    assert(p.state == APP_POMO_PAUSED);
    assert(p.paused_from == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 25 * 60);            // 暂停保留剩余
    assert(app_pomodoro_toggle(&p));                   // 继续
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 25 * 60);

    // ---- 推进与暂停不推进 ----
    assert(app_pomodoro_tick(&p, 30) == APP_POMO_EVENT_NONE);
    assert(p.remaining_seconds == 25 * 60 - 30);
    app_pomodoro_toggle(&p);
    assert(p.state == APP_POMO_PAUSED);
    assert(app_pomodoro_tick(&p, 100) == APP_POMO_EVENT_NONE);  // 暂停时不推进
    assert(p.remaining_seconds == 25 * 60 - 30);
    assert(app_pomodoro_tick(&p, 0) == APP_POMO_EVENT_NONE);    // 非正数忽略
    assert(app_pomodoro_tick(&p, -5) == APP_POMO_EVENT_NONE);

    // ---- 空闲状态 tick 无事件 ----
    app_pomodoro_stop(&p);
    assert(app_pomodoro_tick(&p, 10) == APP_POMO_EVENT_NONE);

    // ---- 完整专注段 -> 短休息段（默认 4 段一长休） ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 2));      // 60s / 120s
    assert(app_pomodoro_start(&p));
    assert(p.remaining_seconds == 60);
    assert(app_pomodoro_progress_permille(&p) == 0);
    assert(app_pomodoro_tick(&p, 30) == APP_POMO_EVENT_NONE);
    assert(p.remaining_seconds == 30);
    assert(app_pomodoro_progress_permille(&p) == 500);
    assert(app_pomodoro_tick(&p, 30) == APP_POMO_EVENT_FOCUS_DONE);  // 到达边界
    assert(p.state == APP_POMO_BREAK);
    assert(p.remaining_seconds == 120);
    assert(p.completed_focus == 1);
    assert(p.focus_minutes_today == 1);
    assert(p.cycles_since_long_break == 1);
    assert(app_pomodoro_progress_permille(&p) == 0);

    // ---- 休息段结束回到专注段 ----
    assert(app_pomodoro_tick(&p, 120) == APP_POMO_EVENT_BREAK_DONE);
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 60);
    assert(p.completed_focus == 1);                    // 休息结束不加专注计数

    // ---- 统计：今日、累计与本次运行同时累加 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 2, 1));
    assert(app_pomodoro_start(&p));
    assert(app_pomodoro_tick(&p, 120) == APP_POMO_EVENT_FOCUS_DONE);
    assert(p.completed_focus == 1);
    assert(p.today_sessions == 1);
    assert(p.focus_minutes_today == 2);
    assert(p.total_focus_sessions == 1);
    assert(p.total_focus_minutes == 2);

    // ---- stop 回空闲并保留全部统计 ----
    app_pomodoro_stop(&p);
    assert(p.state == APP_POMO_IDLE);
    assert(p.remaining_seconds == 0);
    assert(p.paused_from == APP_POMO_IDLE);
    assert(p.completed_focus == 1);
    assert(p.today_sessions == 1);
    assert(p.focus_minutes_today == 2);
    assert(p.total_focus_sessions == 1);
    assert(p.total_focus_minutes == 2);

    // ---- 每 N 段专注后进入长休息，并重新计数 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 1));
    assert(app_pomodoro_set_long_break(&p, 3, 2));    // 2 段一长休

    assert(app_pomodoro_start(&p));
    run_focus_and_break(&p);                           // 第 1 段 -> 短休息
    assert(p.cycles_since_long_break == 1);
    assert(p.state == APP_POMO_FOCUS);

    assert(app_pomodoro_tick(&p, 60) == APP_POMO_EVENT_FOCUS_DONE);  // 第 2 段
    assert(p.state == APP_POMO_LONG_BREAK);
    assert(p.remaining_seconds == 3 * 60);
    assert(p.cycles_since_long_break == 0);            // 触发长休后重新计数
    assert(app_pomodoro_phase(&p) == APP_POMO_LONG_BREAK);
    assert(app_pomodoro_phase_seconds(&p) == 3 * 60);

    // 长休息结束回到专注段
    assert(app_pomodoro_tick(&p, 180) == APP_POMO_EVENT_BREAK_DONE);
    assert(p.state == APP_POMO_FOCUS);
    assert(p.completed_focus == 2);
    assert(p.total_focus_sessions == 2);
    assert(p.focus_minutes_today == 2);

    // ---- 关闭自动接续：阶段结束停回空闲，统计照常累加 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 1));
    assert(app_pomodoro_set_auto_next(&p, false));
    assert(app_pomodoro_start(&p));
    assert(app_pomodoro_tick(&p, 60) == APP_POMO_EVENT_FOCUS_DONE);
    assert(p.state == APP_POMO_IDLE);
    assert(p.remaining_seconds == 0);
    assert(p.paused_from == APP_POMO_IDLE);
    assert(p.today_sessions == 1);
    assert(p.total_focus_sessions == 1);

    // 休息结束同样停回空闲
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 1));
    assert(app_pomodoro_start(&p));
    assert(app_pomodoro_tick(&p, 60) == APP_POMO_EVENT_FOCUS_DONE);
    assert(p.state == APP_POMO_BREAK);
    assert(app_pomodoro_set_auto_next(&p, false));     // 运行中关掉自动接续
    assert(app_pomodoro_tick(&p, 60) == APP_POMO_EVENT_BREAK_DONE);
    assert(p.state == APP_POMO_IDLE);
    assert(p.remaining_seconds == 0);

    // ---- 边界外的富余秒数要带入下一阶段 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 1));      // 60s / 60s
    assert(app_pomodoro_start(&p));
    assert(app_pomodoro_tick(&p, 70) == APP_POMO_EVENT_FOCUS_DONE);  // 越过专注边界 10 秒
    assert(p.state == APP_POMO_BREAK);
    assert(p.remaining_seconds == 50);                 // 60 - 10
    assert(p.completed_focus == 1);
    assert(p.focus_minutes_today == 1);
    // 富余秒数超过下一阶段长度时夹到 0，不无限循环也不留负值
    assert(app_pomodoro_tick(&p, 500) == APP_POMO_EVENT_BREAK_DONE);
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 0);
    assert(!(p.remaining_seconds < 0));

    // ---- 跨天滚动 ----
    app_pomodoro_init(&p);
    p.focus_minutes_today = 50;
    p.today_sessions = 2;
    p.total_focus_sessions = 10;
    p.total_focus_minutes = 500;
    app_pomodoro_roll_day(&p, 0);                      // 0 = 无操作
    assert(p.stats_day == 0);
    assert(p.focus_minutes_today == 50);
    app_pomodoro_roll_day(&p, 20260925);               // 首次记录只补日期
    assert(p.stats_day == 20260925);
    assert(p.focus_minutes_today == 50);
    assert(p.today_sessions == 2);
    assert(p.total_focus_sessions == 10);
    app_pomodoro_roll_day(&p, 20260925);               // 同一天不动
    assert(p.focus_minutes_today == 50);
    app_pomodoro_roll_day(&p, 20260926);               // 换天只清今日
    assert(p.stats_day == 20260926);
    assert(p.focus_minutes_today == 0);
    assert(p.today_sessions == 0);
    assert(p.total_focus_sessions == 10);              // 累计不受影响
    assert(p.total_focus_minutes == 500);

    // ---- 暂停长休息的进度按长休息总时长计算 ----
    app_pomodoro_init(&p);
    p.long_break_minutes = 10;
    p.state = APP_POMO_PAUSED;
    p.paused_from = APP_POMO_LONG_BREAK;
    p.remaining_seconds = 300;
    assert(app_pomodoro_phase(&p) == APP_POMO_LONG_BREAK);
    assert(app_pomodoro_phase_seconds(&p) == 600);
    assert(app_pomodoro_progress_permille(&p) == 500);

    // ---- 进度边界 ----
    app_pomodoro_init(&p);
    p.state = APP_POMO_FOCUS;
    p.remaining_seconds = 0;
    assert(app_pomodoro_progress_permille(&p) == 1000);
    app_pomodoro_stop(&p);
    assert(app_pomodoro_progress_permille(&p) == 0);

    // ---- 状态名 ----
    assert(strcmp(app_pomodoro_state_name(APP_POMO_IDLE), "空闲") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_FOCUS), "专注中") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_BREAK), "短休息") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_LONG_BREAK), "长休息") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_PAUSED), "已暂停") == 0);
    assert(strcmp(app_pomodoro_state_name((app_pomo_state_t)99), "??") == 0);

    puts("test_app_pomodoro: PASS");
    return 0;
}
