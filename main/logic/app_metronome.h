// main/logic/app_metronome.h —— 节拍器的纯逻辑：速度、拍序与 tap 定速。
//
// 这里不碰音频、不碰界面，只回答两个问题：现在该不该响一拍、以及用户连打几下的
// 速度是多少。真正的发声由 net/ui 之外的音频任务按 esp_timer 精确调度，本模块只提供
// "下一拍在什么时刻"的算术，因此可以在主机上把时间当成普通数字来验证。
//
// 时间基准统一用毫秒单调时钟（设备上是 esp_timer）。不是墙上时间：节拍器与"现在几点"
// 无关，只有单调时钟才能保证改系统时间不会让拍子乱掉。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 速度范围。40 太慢、240 太快都没人用，夹在这个区间内既够用又避免 0 除与离谱输入。
#define APP_METRO_BPM_MIN 40
#define APP_METRO_BPM_MAX 240
#define APP_METRO_BPM_DEFAULT 120

// 每小节拍数固定 4：绝大多数练习曲目是 4/4，做可配置会增加按键层级，收益不抵成本。
#define APP_METRO_BEATS_PER_BAR 4

// tap 定速的边界。间隔超过 2 秒视为"重新开始打拍"，而不是把停顿当成一个超长的拍；
// 间隔小于 100 ms（600 BPM 以上）多半是按键抖动或手指连按，不计入。
#define APP_METRO_TAP_TIMEOUT_MS 2000
#define APP_METRO_TAP_MIN_MS 100
// 打完最后一拍静置超过这个时间就退出 tap 状态，速度保留、继续用刚算出的值。
#define APP_METRO_TAP_IDLE_MS 3000
// 至少 3 拍才能算出两个间隔，少于此不出结果。
#define APP_METRO_TAP_MIN_TAPS 3

typedef struct {
    int      bpm;
    bool     running;
    int      beat_index;    // 下一个要响的拍在小节里的位置，0 是重拍
    uint64_t next_us;       // 下一拍的绝对微秒时刻
    uint64_t interval_us;   // 当前速度对应的拍间隔（微秒，带小数精度）

    // tap 定速
    bool     tapping;       // 是否处于"跟着敲"状态
    int      tap_count;     // 已记录的有效拍数
    uint64_t tap_sum_us;    // 相邻间隔之和，用于取平均
    uint64_t tap_last_us;   // 上一次敲击时刻
} app_metronome_t;

// 恢复默认：120 BPM、停止、不处于 tap 状态。
void app_metronome_init(app_metronome_t *m);

// 设定速度。超出范围会被夹取；非法值（<=0）忽略，保持原值。
void app_metronome_set_bpm(app_metronome_t *m, int bpm);
int  app_metronome_bpm(const app_metronome_t *m);

// 开始 / 停止。开始会把拍序归零，让下一拍立刻成为重拍。
void app_metronome_start(app_metronome_t *m, uint64_t now_ms);
void app_metronome_stop(app_metronome_t *m);
bool app_metronome_running(const app_metronome_t *m);

// 速度微调，便于按键直接调用。返回是否真的变了（到边界后不再是变化）。
bool app_metronome_nudge(app_metronome_t *m, int delta);

// 取当前这一拍并推进到下一拍。仅当 running 且 now_ms 已到（或超过）下一拍时返回 true，
// 同时通过 accent 告知这是不是小节重拍。
//
// 卡顿补偿：如果因为某个任务占用了时间导致错过整拍，直接跳到"现在之后的第一拍"，
// 不把错过的拍一次性补响——补响会变成一串急促的连击，比漏一拍更糟。
bool app_metronome_due(app_metronome_t *m, uint64_t now_ms, bool *accent);

// 进入 tap 定速状态：清空已记录的拍，等待用户敲击。
void app_metronome_tap_begin(app_metronome_t *m);
bool app_metronome_tapping(const app_metronome_t *m);

// 敲一下。返回敲击后的有效拍数（1 起）。拍数达到 APP_METRO_TAP_MIN_TAPS 后，每次敲击都会
// 把 bpm 更新为这些拍的平均速度，调用方用 app_metronome_bpm() 读结果。
// 间隔过大自动从这一下重新计拍；间隔过小（抖动）整下丢弃。
int app_metronome_tap(app_metronome_t *m, uint64_t now_ms);

// 长时间没敲就退出 tap 状态。每拍调用，返回是否刚好在这次调用退出。
// 尚未敲下第一下时不退出：此时没有"停手"可言，也避免 tap_last_us 初值 0 造成误判。
bool app_metronome_tap_poll(app_metronome_t *m, uint64_t now_ms);

// 当前拍在小节中的位置，供界面点亮节拍指示。
int app_metronome_beat_index(const app_metronome_t *m);