// main/logic/app_pomodoro.c —— 番茄钟状态机实现。
#include "app_pomodoro.h"

// 时长上下限：专注 1..120 分钟，休息 1..60 分钟。
#define POMO_FOCUS_MIN   1
#define POMO_FOCUS_MAX   120
#define POMO_BREAK_MIN   1
#define POMO_BREAK_MAX   60

#define POMO_DEFAULT_FOCUS 25
#define POMO_DEFAULT_BREAK 5

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

void app_pomodoro_init(app_pomodoro_t *p)
{
    if (!p) return;
    p->focus_minutes = POMO_DEFAULT_FOCUS;
    p->break_minutes = POMO_DEFAULT_BREAK;
    p->state = APP_POMO_IDLE;
    p->paused_from = APP_POMO_IDLE;
    p->remaining_seconds = 0;
    p->completed_focus = 0;
    p->focus_minutes_today = 0;
}

bool app_pomodoro_set_durations(app_pomodoro_t *p, int focus_minutes, int break_minutes)
{
    if (!p) return false;
    // 只在空闲时允许改时长，避免运行中把当前段长度改坏。
    if (p->state != APP_POMO_IDLE) return false;

    int focus = clamp_int(focus_minutes, POMO_FOCUS_MIN, POMO_FOCUS_MAX);
    int brk = clamp_int(break_minutes, POMO_BREAK_MIN, POMO_BREAK_MAX);
    if (focus == p->focus_minutes && brk == p->break_minutes) return false;

    p->focus_minutes = focus;
    p->break_minutes = brk;
    return true;
}

bool app_pomodoro_start(app_pomodoro_t *p)
{
    if (!p) return false;
    if (p->state == APP_POMO_FOCUS || p->state == APP_POMO_BREAK) return false;
    p->state = APP_POMO_FOCUS;
    p->paused_from = APP_POMO_IDLE;
    p->remaining_seconds = p->focus_minutes * 60;
    return true;
}

bool app_pomodoro_toggle(app_pomodoro_t *p)
{
    if (!p) return false;
    switch (p->state) {
    case APP_POMO_IDLE:
        return app_pomodoro_start(p);
    case APP_POMO_FOCUS:
    case APP_POMO_BREAK:
        // 记住暂停前所处阶段，恢复时原样回到该阶段并保留剩余秒数。
        p->paused_from = p->state;
        p->state = APP_POMO_PAUSED;
        return true;
    case APP_POMO_PAUSED:
        p->state = (p->paused_from == APP_POMO_FOCUS || p->paused_from == APP_POMO_BREAK)
                       ? p->paused_from
                       : APP_POMO_FOCUS;
        return true;
    default:
        return false;
    }
}

void app_pomodoro_stop(app_pomodoro_t *p)
{
    if (!p) return;
    p->state = APP_POMO_IDLE;
    p->paused_from = APP_POMO_IDLE;
    p->remaining_seconds = 0;
    // 保留 focus_minutes_today（今日统计跨停止累计）；completed_focus 属于本次运行，清零。
    p->completed_focus = 0;
}

bool app_pomodoro_tick(app_pomodoro_t *p, int seconds)
{
    if (!p || seconds <= 0) return false;
    if (p->state != APP_POMO_FOCUS && p->state != APP_POMO_BREAK) return false;

    int remaining = p->remaining_seconds - seconds;
    if (remaining > 0) {
        p->remaining_seconds = remaining;
        return false;
    }

    // 越过或正好到达边界。surplus 为超出边界的秒数，需要带入下一阶段。
    int surplus = -remaining;
    if (p->state == APP_POMO_FOCUS) {
        p->focus_minutes_today += p->focus_minutes;
        p->completed_focus++;
        p->state = APP_POMO_BREAK;
        int length = p->break_minutes * 60;
        // 单次 tick 最多跨一个边界：剩余秒数超过下一阶段长度时直接夹到 0，不再循环。
        p->remaining_seconds = (surplus >= length) ? 0 : length - surplus;
    } else {
        p->state = APP_POMO_FOCUS;
        int length = p->focus_minutes * 60;
        p->remaining_seconds = (surplus >= length) ? 0 : length - surplus;
    }
    return true;
}

int app_pomodoro_progress_permille(const app_pomodoro_t *p)
{
    if (!p) return 0;
    // 暂停时进度沿用暂停前所处阶段。
    app_pomo_state_t phase = (p->state == APP_POMO_PAUSED) ? p->paused_from : p->state;
    if (phase != APP_POMO_FOCUS && phase != APP_POMO_BREAK) return 0;

    int total = (phase == APP_POMO_FOCUS ? p->focus_minutes : p->break_minutes) * 60;
    if (total <= 0) return 0;

    int remaining = p->remaining_seconds;
    if (remaining < 0) remaining = 0;
    if (remaining > total) remaining = total;
    int permille = (total - remaining) * 1000 / total;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    return permille;
}

const char *app_pomodoro_state_name(app_pomo_state_t state)
{
    switch (state) {
    case APP_POMO_IDLE:   return "空闲";
    case APP_POMO_FOCUS:  return "专注中";
    case APP_POMO_BREAK:  return "休息中";
    case APP_POMO_PAUSED: return "已暂停";
    default:              return "??";
    }
}
