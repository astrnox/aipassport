// main/logic/app_metronome.c —— 节拍器纯逻辑实现（详见 app_metronome.h）。
#include "app_metronome.h"

// 一拍多少微秒。60 秒 = 60,000,000 微秒，整数除法的舍入误差在 120 BPM 下不到 1us，
// 一分钟累计偏差远小于人耳可辨的 1ms，不需要定点小数。
static uint64_t interval_from_bpm(int bpm)
{
    if (bpm < APP_METRO_BPM_MIN) bpm = APP_METRO_BPM_MIN;
    if (bpm > APP_METRO_BPM_MAX) bpm = APP_METRO_BPM_MAX;
    return 60000000ULL / (uint64_t)bpm;
}

void app_metronome_init(app_metronome_t *m)
{
    if (!m) return;
    m->bpm = APP_METRO_BPM_DEFAULT;
    m->running = false;
    m->beat_index = 0;
    m->next_us = 0;
    m->interval_us = interval_from_bpm(APP_METRO_BPM_DEFAULT);
    m->tapping = false;
    m->tap_count = 0;
    m->tap_sum_us = 0;
    m->tap_last_us = 0;
}

void app_metronome_set_bpm(app_metronome_t *m, int bpm)
{
    if (!m) return;
    if (bpm <= 0) return;   // 非法输入保持原值：0 或负数没有"夹到下限"的语义，会误改用户设置
    if (bpm < APP_METRO_BPM_MIN) bpm = APP_METRO_BPM_MIN;
    if (bpm > APP_METRO_BPM_MAX) bpm = APP_METRO_BPM_MAX;
    m->bpm = bpm;
    m->interval_us = interval_from_bpm(bpm);
}

int app_metronome_bpm(const app_metronome_t *m)
{
    return m ? m->bpm : APP_METRO_BPM_DEFAULT;
}

bool app_metronome_nudge(app_metronome_t *m, int delta)
{
    if (!m || delta == 0) return false;
    int before = m->bpm;
    app_metronome_set_bpm(m, m->bpm + delta);
    return m->bpm != before;
}

void app_metronome_start(app_metronome_t *m, uint64_t now_ms)
{
    if (!m) return;
    m->running = true;
    // 拍序归零并让下一拍就落在"现在"：按下开始立刻听到重拍，用户不用等半拍才知道它启动了。
    m->beat_index = 0;
    m->next_us = now_ms * 1000ULL;
}

void app_metronome_stop(app_metronome_t *m)
{
    if (!m) return;
    m->running = false;
}

bool app_metronome_running(const app_metronome_t *m)
{
    return m ? m->running : false;
}

bool app_metronome_due(app_metronome_t *m, uint64_t now_ms, bool *accent)
{
    if (!m || !m->running) return false;

    uint64_t now_us = now_ms * 1000ULL;
    if (now_us < m->next_us) return false;

    if (accent) *accent = (m->beat_index == 0);

    m->next_us += m->interval_us;
    m->beat_index = (m->beat_index + 1) % APP_METRO_BEATS_PER_BAR;

    // 卡顿补偿：被别的高优先级任务拖住导致错过整拍时，直接把时刻推到"现在之后"，
    // 但拍序要按跳过的拍数一起推进，否则小节重拍会错位。补响被跳过的拍会让用户听到
    // 一串急促连击，比漏一拍更难听。
    while (m->next_us <= now_us) {
        m->next_us += m->interval_us;
        m->beat_index = (m->beat_index + 1) % APP_METRO_BEATS_PER_BAR;
    }
    return true;
}

void app_metronome_tap_begin(app_metronome_t *m)
{
    if (!m) return;
    m->tapping = true;
    m->tap_count = 0;
    m->tap_sum_us = 0;
    m->tap_last_us = 0;
}

bool app_metronome_tapping(const app_metronome_t *m)
{
    return m ? m->tapping : false;
}

int app_metronome_tap(app_metronome_t *m, uint64_t now_ms)
{
    if (!m) return 0;
    if (!m->tapping) app_metronome_tap_begin(m);

    uint64_t now_us = now_ms * 1000ULL;

    if (m->tap_count <= 0) {
        // 第一下只记时刻，算不出间隔。
        m->tap_count = 1;
        m->tap_last_us = now_us;
        return m->tap_count;
    }

    uint64_t dt = now_us - m->tap_last_us;

    if (dt > (uint64_t)APP_METRO_TAP_TIMEOUT_MS * 1000ULL) {
        // 停手太久：把这一下当成新的一轮第一拍，而不是把一个几秒的停顿算成超慢速。
        m->tap_count = 1;
        m->tap_sum_us = 0;
        m->tap_last_us = now_us;
        return m->tap_count;
    }

    if (dt < (uint64_t)APP_METRO_TAP_MIN_MS * 1000ULL) {
        // 抖动或连按：整下丢弃，且不更新基准时刻，免得把后续间隔也算歪。
        return m->tap_count;
    }

    m->tap_sum_us += dt;
    m->tap_count++;
    m->tap_last_us = now_us;

    if (m->tap_count >= APP_METRO_TAP_MIN_TAPS) {
        uint64_t avg = m->tap_sum_us / (uint64_t)(m->tap_count - 1);
        if (avg > 0) {
            // 四舍五入到整数 BPM：先乘后除，避免整数除法丢掉的半个 BPM 累积成误差。
            int bpm = (int)((60000000ULL + avg / 2) / avg);
            app_metronome_set_bpm(m, bpm);
        }
    }
    return m->tap_count;
}

bool app_metronome_tap_poll(app_metronome_t *m, uint64_t now_ms)
{
    if (!m || !m->tapping) return false;
    // 还没敲过第一下就谈不上"停手"：此时 tap_last_us 还是 0，用它判断会立刻退出。
    if (m->tap_count <= 0) return false;

    uint64_t now_us = now_ms * 1000ULL;
    if (now_us - m->tap_last_us <= (uint64_t)APP_METRO_TAP_IDLE_MS * 1000ULL) return false;

    m->tapping = false;
    return true;
}

int app_metronome_beat_index(const app_metronome_t *m)
{
    return m ? m->beat_index : 0;
}