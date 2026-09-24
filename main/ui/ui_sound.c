// main/ui/ui_sound.c —— 提示音实现（双音短提示）。
//
// 采样率固定 16 kHz、16 bit 单声道，与 BSP 音频层默认能力一致。每段音前后做淡入淡出，
// 避免 ES8311 在起止瞬间的爆音。播放期间不再叠加新请求，防止连续触发时任务堆积。
#include "ui_sound.h"

#include "app_state.h"

#include "bsp_audio.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#define SND_RATE     16000
#define SND_MS       90
#define SND_GAP_MS   35
#define SND_TONE_A   1568   // G6
#define SND_TONE_B   2093   // C7
#define SND_AMP      4200

static volatile bool s_busy;

static void fill_tone(int16_t *buf, int samples, int hz)
{
    if (samples <= 0) return;
    int period = SND_RATE / (hz > 0 ? hz : 1);
    if (period < 4) period = 4;
    int fade = SND_RATE / 250;          // 约 4 ms
    if (fade > samples / 2) fade = samples / 2;

    int phase = 0;
    for (int i = 0; i < samples; i++) {
        int amp = SND_AMP;
        if (fade > 0 && i < fade) amp = amp * i / fade;
        else if (fade > 0 && i >= samples - fade) amp = amp * (samples - i) / fade;
        buf[i] = (phase < period / 2) ? (int16_t)amp : (int16_t)-amp;
        if (++phase >= period) phase = 0;
    }
}

static void beep_task(void *arg)
{
    (void)arg;

    const int tone_samples = SND_RATE * SND_MS / 1000;
    const int gap_samples = SND_RATE * SND_GAP_MS / 1000;
    const int total = tone_samples * 2 + gap_samples;

    int16_t *buf = malloc((size_t)total * sizeof(int16_t));
    if (!buf) {
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    fill_tone(buf, tone_samples, SND_TONE_A);
    memset(buf + tone_samples, 0, (size_t)gap_samples * sizeof(int16_t));
    fill_tone(buf + tone_samples + gap_samples, tone_samples, SND_TONE_B);

    // 音频未初始化时 set_format 返回 INVALID_STATE，此时静默跳过，不影响其他功能。
    if (bsp_audio_set_format(SND_RATE, 16, 1) == ESP_OK) {
        app_settings_t *st = app_state_settings();
        int vol = st->volume;
        if (vol < 10) vol = 10;
        if (vol > 100) vol = 100;
        bsp_audio_set_volume((uint8_t)vol);
        if (bsp_audio_write(buf, (size_t)total * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGD("ui_sound", "提示音写入失败，忽略");
        }
    }

    free(buf);
    s_busy = false;
    vTaskDelete(NULL);
}

void ui_sound_beep(void)
{
    app_settings_t *st = app_state_settings();
    if (st->sound_muted) return;
    if (s_busy) return;
    s_busy = true;

    // 提示音很短（约 0.2 秒），栈也只放局部变量，故给一个较小的栈即可。
    if (xTaskCreate(beep_task, "ui_beep", 3072, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
    }
}
