// tests/test_app_pomodoro.c —— app_pomodoro 的主机侧单元测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_pomodoro.h"

int main(void)
{
    app_pomodoro_t p;

    // ---- 默认值 ----
    app_pomodoro_init(&p);
    assert(p.focus_minutes == 25);
    assert(p.break_minutes == 5);
    assert(p.state == APP_POMO_IDLE);
    assert(p.paused_from == APP_POMO_IDLE);
    assert(p.remaining_seconds == 0);
    assert(p.completed_focus == 0);
    assert(p.focus_minutes_today == 0);
    assert(app_pomodoro_progress_permille(&p) == 0);

    // ---- 时长设置与夹取 ----
    assert(!app_pomodoro_set_durations(&p, 25, 5));   // 无变化
    assert(app_pomodoro_set_durations(&p, 0, 0));     // 夹到下限
    assert(p.focus_minutes == 1);
    assert(p.break_minutes == 1);
    assert(app_pomodoro_set_durations(&p, 999, 999)); // 夹到上限
    assert(p.focus_minutes == 120);
    assert(p.break_minutes == 60);

    // ---- 开始 / 暂停 / 继续 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_start(&p));
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 25 * 60);
    assert(!app_pomodoro_start(&p));                   // 已在运行
    assert(!app_pomodoro_set_durations(&p, 10, 10));   // 运行中禁止改时长

    assert(app_pomodoro_toggle(&p));
    assert(p.state == APP_POMO_PAUSED);
    assert(p.paused_from == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 25 * 60);            // 暂停保留剩余
    assert(app_pomodoro_toggle(&p));                   // 继续
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 25 * 60);

    // ---- 推进与暂停不推进 ----
    assert(!app_pomodoro_tick(&p, 30));
    assert(p.remaining_seconds == 25 * 60 - 30);
    app_pomodoro_toggle(&p);
    assert(p.state == APP_POMO_PAUSED);
    assert(!app_pomodoro_tick(&p, 100));               // 暂停时不推进
    assert(p.remaining_seconds == 25 * 60 - 30);
    assert(!app_pomodoro_tick(&p, 0));                 // 非正数忽略
    assert(!app_pomodoro_tick(&p, -5));

    // ---- stop 回空闲并保留今日统计 ----
    p.focus_minutes_today = 12;
    p.completed_focus = 3;
    app_pomodoro_stop(&p);
    assert(p.state == APP_POMO_IDLE);
    assert(p.remaining_seconds == 0);
    assert(p.focus_minutes_today == 12);
    assert(p.completed_focus == 0);

    // ---- 完整专注段 -> 休息段 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 2));      // 60s / 120s
    assert(app_pomodoro_start(&p));
    assert(p.remaining_seconds == 60);
    assert(app_pomodoro_progress_permille(&p) == 0);
    assert(!app_pomodoro_tick(&p, 30));
    assert(p.remaining_seconds == 30);
    assert(app_pomodoro_progress_permille(&p) == 500);
    assert(app_pomodoro_tick(&p, 30));                 // 到达边界
    assert(p.state == APP_POMO_BREAK);
    assert(p.remaining_seconds == 120);
    assert(p.completed_focus == 1);
    assert(p.focus_minutes_today == 1);
    assert(app_pomodoro_progress_permille(&p) == 0);

    // ---- 休息段结束回到专注段 ----
    assert(app_pomodoro_tick(&p, 120));
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 60);
    assert(p.completed_focus == 1);                    // 休息结束不加专注计数

    // ---- 边界外的富余秒数要带入下一阶段 ----
    app_pomodoro_init(&p);
    assert(app_pomodoro_set_durations(&p, 1, 1));      // 60s / 60s
    assert(app_pomodoro_start(&p));
    assert(app_pomodoro_tick(&p, 70));                 // 越过专注边界 10 秒
    assert(p.state == APP_POMO_BREAK);
    assert(p.remaining_seconds == 50);                 // 60 - 10
    assert(p.completed_focus == 1);
    assert(p.focus_minutes_today == 1);
    // 富余秒数超过下一阶段长度时夹到 0，不无限循环
    assert(app_pomodoro_tick(&p, 500));
    assert(p.state == APP_POMO_FOCUS);
    assert(p.remaining_seconds == 0);

    // ---- 进度边界 ----
    p.state = APP_POMO_FOCUS;
    p.remaining_seconds = 0;
    assert(app_pomodoro_progress_permille(&p) == 1000);
    app_pomodoro_stop(&p);
    assert(app_pomodoro_progress_permille(&p) == 0);

    // ---- 状态名 ----
    assert(strcmp(app_pomodoro_state_name(APP_POMO_IDLE), "空闲") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_FOCUS), "专注中") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_BREAK), "休息中") == 0);
    assert(strcmp(app_pomodoro_state_name(APP_POMO_PAUSED), "已暂停") == 0);
    assert(strcmp(app_pomodoro_state_name((app_pomo_state_t)99), "??") == 0);

    puts("test_app_pomodoro: PASS");
    return 0;
}
