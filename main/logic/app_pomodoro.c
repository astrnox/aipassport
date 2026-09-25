// main/logic/app_pomodoro.c —— 番茄钟状态机实现。
#include "app_pomodoro.h"

#define POMO_DEFAULT_FOCUS       25
#define POMO_DEFAULT_BREAK       5
#define POMO_DEFAULT_LONG_BREAK  15
#define POMO_DEFAULT_CYCLES      4

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// 阶段总秒数。空闲或未知状态返回 0，进度与剩余时间都据此判断。
static int phase_seconds(const app_pomodoro_t *p, app_pomo_state_t phase)
{
    switch (phase) {
    case APP_POMO_FOCUS:      return p->focus_minutes * 60;
    case APP_POMO_BREAK:      return p->break_minutes * 60;
    case APP_POMO_LONG_BREAK: return p->long_break_minutes * 60;
    default:                  return 0;
    }
}

void app_pomodoro_init(app_pomodoro_t *p)
{
    if (!p) return;
    p->focus_minutes = POMO_DEFAULT_FOCUS;
    p->break_minutes = POMO_DEFAULT_BREAK;
    p->long_break_minutes = POMO_DEFAULT_LONG_BREAK;
    p->cycles_per_long_break = POMO_DEFAULT_CYCLES;
    p->auto_next = true;
    p->state = APP_POMO_IDLE;
    p->paused_from = APP_POMO_IDLE;
    p->remaining_seconds = 0;
    p->completed_focus = 0;
    p->focus_minutes_today = 0;
    p->stats_day = 0;
    p->today_sessions = 0;
    p->total_focus_sessions = 0;
    p->total_focus_minutes = 0;
    p->cycles_since_long_break = 0;
}

bool app_pomodoro_set_durations(app_pomodoro_t *p, int focus_minutes, int break_minutes)
{
    if (!p) return false;
    // 只在空闲时允许改时长，避免运行中把当前段长度改坏。
    if (p->state != APP_POMO_IDLE) return false;

    int focus = clamp_int(focus_minutes, APP_POMO_FOCUS_MIN_MIN, APP_POMO_FOCUS_MIN_MAX);
    int brk = clamp_int(break_minutes, APP_POMO_BREAK_MIN_MIN, APP_POMO_BREAK_MIN_MAX);
    if (focus == p->focus_minutes && brk == p->break_minutes) return false;

    p->focus_minutes = focus;
    p->break_minutes = brk;
    return true;
}

bool app_pomodoro_set_long_break(app_pomodoro_t *p, int long_break_minutes, int cycles)
{
    if (!p) return false;
    if (p->state != APP_POMO_IDLE) return false;

    int lng = clamp_int(long_break_minutes, APP_POMO_LONG_MIN_MIN, APP_POMO_LONG_MIN_MAX);
    int cyc = clamp_int(cycles, APP_POMO_CYCLES_MIN, APP_POMO_CYCLES_MAX);
    if (lng == p->long_break_minutes && cyc == p->cycles_per_long_break) return false;

    p->long_break_minutes = lng;
    p->cycles_per_long_break = cyc;
    return true;
}

bool app_pomodoro_set_auto_next(app_pomodoro_t *p, bool enabled)
{
    if (!p) return false;
    // 自动接续不决定当前段的长度，因此运行中也允许切换。
    if (p->auto_next == enabled) return false;
    p->auto_next = enabled;
    return true;
}

bool app_pomodoro_start(app_pomodoro_t *p)
{
    if (!p) return false;
    if (p->state == APP_POMO_FOCUS || p->state == APP_POMO_BREAK ||
        p->state == APP_POMO_LONG_BREAK) {
        return false;
    }
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
    case APP_POMO_LONG_BREAK:
        // 记住暂停前所处阶段，恢复时原样回到该阶段并保留剩余秒数。
        p->paused_from = p->state;
        p->state = APP_POMO_PAUSED;
        return true;
    case APP_POMO_PAUSED:
        p->state = (p->paused_from == APP_POMO_FOCUS || p->paused_from == APP_POMO_BREAK ||
                    p->paused_from == APP_POMO_LONG_BREAK)
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
    // 只回到空闲：今日与累计统计保留，completed_focus 作为"本次运行"的计数同样保留，
    // 只有重新 init 才归零。
}

app_pomo_event_t app_pomodoro_tick(app_pomodoro_t *p, int seconds)
{
    if (!p || seconds <= 0) return APP_POMO_EVENT_NONE;
    if (p->state != APP_POMO_FOCUS && p->state != APP_POMO_BREAK &&
        p->state != APP_POMO_LONG_BREAK) {
        return APP_POMO_EVENT_NONE;
    }

    int remaining = p->remaining_seconds - seconds;
    if (remaining > 0) {
        p->remaining_seconds = remaining;
        return APP_POMO_EVENT_NONE;
    }

    // 越过或正好到达边界。surplus 为超出的秒数，需要带入下一阶段。
    int surplus = -remaining;
    bool was_focus = (p->state == APP_POMO_FOCUS);
    app_pomo_state_t next;

    if (was_focus) {
        // 一段专注完成：先记满今日、本次运行与永久三层统计，再推进长休息周期。
        p->completed_focus++;
        p->today_sessions++;
        p->focus_minutes_today += p->focus_minutes;
        p->total_focus_sessions++;
        p->total_focus_minutes += p->focus_minutes;
        p->cycles_since_long_break++;
        if (p->cycles_since_long_break >= p->cycles_per_long_break) {
            p->cycles_since_long_break = 0;
            next = APP_POMO_LONG_BREAK;
        } else {
            next = APP_POMO_BREAK;
        }
    } else {
        next = APP_POMO_FOCUS;
    }

    // 关闭自动接续时阶段结束就停在空闲；上面的统计已经记好，不受影响。
    if (!p->auto_next) {
        p->state = APP_POMO_IDLE;
        p->paused_from = APP_POMO_IDLE;
        p->remaining_seconds = 0;
        return was_focus ? APP_POMO_EVENT_FOCUS_DONE : APP_POMO_EVENT_BREAK_DONE;
    }

    p->state = next;
    p->paused_from = APP_POMO_IDLE;
    int length = phase_seconds(p, next);
    // 单次 tick 最多跨一个边界：剩余秒数超过下一阶段长度时夹到 0，不留负值也不连跳。
    p->remaining_seconds = (surplus >= length) ? 0 : length - surplus;
    return was_focus ? APP_POMO_EVENT_FOCUS_DONE : APP_POMO_EVENT_BREAK_DONE;
}

void app_pomodoro_roll_day(app_pomodoro_t *p, uint32_t date)
{
    if (!p || date == 0) return;
    // 首次记录（升级或清除数据后）只补上日期，不动已有的今日数据，避免误清。
    if (p->stats_day == 0) {
        p->stats_day = date;
        return;
    }
    if (p->stats_day == date) return;
    // 换天只清今日，永久累计与本次运行计数都不受影响。
    p->stats_day = date;
    p->focus_minutes_today = 0;
    p->today_sessions = 0;
}

app_pomo_state_t app_pomodoro_phase(const app_pomodoro_t *p)
{
    if (!p) return APP_POMO_IDLE;
    // 暂停时进度与剩余时间都属于暂停前的那个阶段。
    if (p->state == APP_POMO_PAUSED) return p->paused_from;
    return p->state;
}

int app_pomodoro_phase_seconds(const app_pomodoro_t *p)
{
    if (!p) return 0;
    return phase_seconds(p, app_pomodoro_phase(p));
}

int app_pomodoro_progress_permille(const app_pomodoro_t *p)
{
    if (!p) return 0;
    int total = app_pomodoro_phase_seconds(p);
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
    case APP_POMO_IDLE:       return "空闲";
    case APP_POMO_FOCUS:      return "专注中";
    case APP_POMO_BREAK:      return "短休息";
    case APP_POMO_LONG_BREAK: return "长休息";
    case APP_POMO_PAUSED:     return "已暂停";
    default:                  return "??";
    }
}
