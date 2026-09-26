// tests/test_app_metronome.c —— app_metronome 的主机侧单元测试。
//
// 覆盖：默认值、速度夹取与非法输入、微调、开始/停止、按时刻取拍、重拍位置、卡顿补偿、
// tap 定速的平均与四舍五入、停顿重开、抖动丢弃、静置退出。
#include <assert.h>
#include <stdio.h>

#include "logic/app_metronome.h"

int main(void)
{
    app_metronome_t m;

    // ---- 默认值 ----
    app_metronome_init(&m);
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_DEFAULT);
    assert(app_metronome_running(&m) == false);
    assert(app_metronome_tapping(&m) == false);
    assert(app_metronome_beat_index(&m) == 0);
    assert(m.interval_us == 500000);   // 120 BPM = 每拍 0.5 秒

    // ---- 速度设置：夹取与非法输入 ----
    app_metronome_set_bpm(&m, 10);
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_MIN);
    app_metronome_set_bpm(&m, 999);
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_MAX);
    app_metronome_set_bpm(&m, 0);          // 非法：保持原值（240）
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_MAX);
    app_metronome_set_bpm(&m, -5);
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_MAX);

    // ---- 微调 ----
    app_metronome_set_bpm(&m, 120);
    assert(app_metronome_nudge(&m, 5) == true);
    assert(app_metronome_bpm(&m) == 125);
    assert(app_metronome_nudge(&m, 0) == false);
    app_metronome_set_bpm(&m, APP_METRO_BPM_MAX);
    assert(app_metronome_nudge(&m, 10) == false);   // 已到上限
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_MAX);

    // ---- 取拍：未开始时永不触发 ----
    app_metronome_init(&m);
    bool accent = false;
    assert(app_metronome_due(&m, 100000, &accent) == false);

    // ---- 取拍：开始即重拍，之后按间隔推进 ----
    app_metronome_start(&m, 1000);          // 1000ms 处下一拍，120 BPM
    assert(app_metronome_running(&m) == true);
    assert(m.next_us == 1000000ULL);
    assert(app_metronome_due(&m, 999, &accent) == false);   // 还差 1ms
    assert(app_metronome_due(&m, 1000, &accent) == true);
    assert(accent == true);                  // 小节重拍
    assert(m.next_us == 1500000ULL);
    assert(app_metronome_beat_index(&m) == 1);
    assert(app_metronome_due(&m, 1000, &accent) == false);  // 同一时刻只响一次
    assert(app_metronome_due(&m, 1500, &accent) == true);
    assert(accent == false);
    assert(app_metronome_due(&m, 2000, &accent) == true);
    assert(accent == false);
    assert(app_metronome_due(&m, 2500, &accent) == true);
    assert(accent == false);
    assert(app_metronome_due(&m, 3000, &accent) == true);
    assert(accent == true);                  // 第 5 拍回到小节头
    assert(app_metronome_beat_index(&m) == 1);

    // ---- 卡顿补偿：错过整拍只响一拍，不补成一串连击 ----
    app_metronome_init(&m);
    app_metronome_start(&m, 0);
    assert(app_metronome_due(&m, 0, &accent) == true);
    assert(m.next_us == 500000ULL);
    // 拖到 3000ms 才来：只应返回一次 true，并把下一拍推到 3000ms 之后。
    assert(app_metronome_due(&m, 3000, &accent) == true);
    assert(m.next_us > 3000000ULL);
    assert(m.next_us == 3500000ULL);
    assert(app_metronome_due(&m, 3000, &accent) == false);

    // ---- 停止 ----
    app_metronome_stop(&m);
    assert(app_metronome_running(&m) == false);
    assert(app_metronome_due(&m, 9999, &accent) == false);

    // ---- tap 定速：三拍 400ms -> 150 BPM ----
    app_metronome_init(&m);
    assert(app_metronome_tap(&m, 0) == 1);
    assert(app_metronome_tapping(&m) == true);
    assert(app_metronome_tap(&m, 400) == 2);
    assert(app_metronome_bpm(&m) == APP_METRO_BPM_DEFAULT);   // 不足 3 拍不改速度
    assert(app_metronome_tap(&m, 800) == 3);
    assert(app_metronome_bpm(&m) == 150);

    // ---- tap 定速：四舍五入到整数 BPM ----
    app_metronome_init(&m);
    app_metronome_tap(&m, 0);
    app_metronome_tap(&m, 500);
    app_metronome_tap(&m, 1000);
    assert(app_metronome_bpm(&m) == 120);   // 500000us -> 120.0

    // ---- tap 定速：抖动间隔整下丢弃 ----
    app_metronome_init(&m);
    assert(app_metronome_tap(&m, 0) == 1);
    assert(app_metronome_tap(&m, 50) == 1);     // 50ms < 100ms，丢弃
    assert(app_metronome_tap(&m, 500) == 2);    // 相对 0ms 计 500ms
    assert(app_metronome_tap(&m, 1000) == 3);
    assert(app_metronome_bpm(&m) == 120);

    // ---- tap 定速：停顿超过 2 秒从这一下重新计拍 ----
    app_metronome_init(&m);
    app_metronome_tap(&m, 0);
    app_metronome_tap(&m, 3000);                // 3s > 2s，重开
    assert(m.tap_count == 1);
    assert(app_metronome_tap(&m, 3400) == 2);
    assert(app_metronome_tap(&m, 3800) == 3);
    assert(app_metronome_bpm(&m) == 150);

    // ---- tap 静置退出 ----
    app_metronome_init(&m);
    app_metronome_tap_begin(&m);
    assert(app_metronome_tap_poll(&m, 5000) == false);   // 还没敲过，不算停手
    app_metronome_tap(&m, 1000);
    assert(app_metronome_tap_poll(&m, 2000) == false);   // 只过了 1s
    assert(app_metronome_tap_poll(&m, 4500) == true);    // 过了 3.5s
    assert(app_metronome_tapping(&m) == false);
    assert(app_metronome_tap_poll(&m, 6000) == false);   // 已退出，不重复报

    printf("test_app_metronome: PASS\n");
    return 0;
}