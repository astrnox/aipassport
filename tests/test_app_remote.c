// tests/test_app_remote.c —— app_remote 的主机侧单元测试。
//
// 遥控最怕"文档说 OK 是快门、实际发了回车"这类错位，所以这里对每条映射逐字节断言
// 报文的 report id、长度与按键码，并检查未映射的键确实返回 false（界面据此跳过）。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_remote.h"

// 键盘报文：修饰键与保留字节为 0，第一个按键槽放用途码，其余为 0。
static void assert_kb(const app_remote_report_t *r, uint8_t usage)
{
    assert(r->map_index == APP_REMOTE_MAP_KEYBOARD);
    assert(r->report_id == APP_REMOTE_RID_KEYBOARD);
    assert(r->length == APP_REMOTE_KB_LEN);
    assert(r->data[0] == 0);
    assert(r->data[1] == 0);
    assert(r->data[2] == usage);
    for (int i = 3; i < APP_REMOTE_KB_LEN; i++) assert(r->data[i] == 0);
}

// 消费类报文：16 位用途码小端。
static void assert_cons(const app_remote_report_t *r, uint16_t usage)
{
    assert(r->map_index == APP_REMOTE_MAP_CONSUMER);
    assert(r->report_id == APP_REMOTE_RID_CONSUMER);
    assert(r->length == APP_REMOTE_CONS_LEN);
    assert(r->data[0] == (uint8_t)(usage & 0xFF));
    assert(r->data[1] == (uint8_t)(usage >> 8));
}

// "松开"报文必须全 0，否则主机会把它当成持续按住。
static void assert_released(const app_remote_report_t *r, uint8_t map_index, uint8_t report_id,
                            uint8_t len)
{
    assert(r->map_index == map_index);
    assert(r->report_id == report_id);
    assert(r->length == len);
    for (int i = 0; i < 8; i++) assert(r->data[i] == 0);
}

static bool press(app_remote_mode_t mode, app_remote_btn_t btn, app_remote_press_t p,
                  app_remote_report_t *down, app_remote_report_t *up)
{
    memset(down, 0xAA, sizeof(*down));
    memset(up, 0xAA, sizeof(*up));
    return app_remote_press(mode, btn, p, down, up);
}

int main(void)
{
    app_remote_report_t down, up;

    // ---- 模式名与循环 ----
    assert(strcmp(app_remote_mode_name(APP_REMOTE_MODE_READER), "电子书翻页") == 0);
    assert(strcmp(app_remote_mode_name(APP_REMOTE_MODE_VOLUME), "音量控制") == 0);
    assert(strcmp(app_remote_mode_name(APP_REMOTE_MODE_SLIDES), "PPT 演示") == 0);
    assert(strcmp(app_remote_mode_name(APP_REMOTE_MODE_MEDIA), "万能遥控") == 0);
    assert(app_remote_mode_next(APP_REMOTE_MODE_READER) == APP_REMOTE_MODE_VOLUME);
    assert(app_remote_mode_next(APP_REMOTE_MODE_VOLUME) == APP_REMOTE_MODE_SLIDES);
    assert(app_remote_mode_next(APP_REMOTE_MODE_SLIDES) == APP_REMOTE_MODE_MEDIA);
    assert(app_remote_mode_next(APP_REMOTE_MODE_MEDIA) == APP_REMOTE_MODE_READER);  // 循环
    assert(app_remote_mode_next((app_remote_mode_t)99) == APP_REMOTE_MODE_READER);  // 越界收敛

    // ---- 电子书翻页 ----
    assert(press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_UP, APP_REMOTE_CLICK, &down, &up));
    assert_kb(&down, 0x4B);   // PageUp
    assert_released(&up, APP_REMOTE_MAP_KEYBOARD, APP_REMOTE_RID_KEYBOARD, APP_REMOTE_KB_LEN);

    assert(press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, &down, &up));
    assert_kb(&down, 0x4E);   // PageDown

    assert(press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_OK, APP_REMOTE_CLICK, &down, &up));
    assert_kb(&down, 0x28);   // Enter

    assert(press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_OK, APP_REMOTE_LONG, &down, &up));
    assert_kb(&down, 0x29);   // Esc

    // 电子书模式没有长按 UP/DOWN 的映射
    assert(press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_UP, APP_REMOTE_LONG, &down, &up) == false);
    assert(press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG, &down, &up) == false);

    // ---- 音量控制 ----
    assert(press(APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_UP, APP_REMOTE_CLICK, &down, &up));
    assert_cons(&down, 0x00E9);   // Volume Up
    assert_released(&up, APP_REMOTE_MAP_CONSUMER, APP_REMOTE_RID_CONSUMER, APP_REMOTE_CONS_LEN);

    assert(press(APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, &down, &up));
    assert_cons(&down, 0x00EA);   // Volume Down

    assert(press(APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_OK, APP_REMOTE_CLICK, &down, &up));
    assert_cons(&down, 0x00E2);   // Mute

    assert(press(APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_OK, APP_REMOTE_LONG, &down, &up));
    assert_cons(&down, 0x00CD);   // Play/Pause
    assert(app_remote_press(APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_UP, APP_REMOTE_LONG,
                            &down, &up) == false);

    // ---- PPT 演示 ----
    assert(press(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_UP, APP_REMOTE_CLICK, &down, &up));
    assert_kb(&down, 0x4B);
    assert(press(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, &down, &up));
    assert_kb(&down, 0x4E);
    assert(press(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_OK, APP_REMOTE_CLICK, &down, &up));
    assert_kb(&down, 0x3E);   // F5 开始放映
    assert(press(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_OK, APP_REMOTE_LONG, &down, &up));
    assert_kb(&down, 0x29);   // Esc 退出放映
    assert(press(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_UP, APP_REMOTE_LONG, &down, &up));
    assert_kb(&down, 0x05);   // B 黑屏
    assert(press(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG, &down, &up));
    assert_kb(&down, 0x1A);   // W 白屏

    // ---- 万能遥控（媒体 + 快门） ----
    assert(press(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_OK, APP_REMOTE_CLICK, &down, &up));
    assert_cons(&down, 0x00CD);   // Play/Pause
    assert(press(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_UP, APP_REMOTE_CLICK, &down, &up));
    assert_cons(&down, 0x00E9);   // 音量+ / 快门
    assert(press(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, &down, &up));
    assert_cons(&down, 0x00EA);
    assert(press(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_UP, APP_REMOTE_LONG, &down, &up));
    assert_cons(&down, 0x00B5);   // 下一曲
    assert(press(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG, &down, &up));
    assert_cons(&down, 0x00B6);   // 上一曲
    assert(press(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_OK, APP_REMOTE_LONG, &down, &up));
    assert_kb(&down, 0x28);       // Enter 快门

    // ---- 非法参数 ----
    assert(app_remote_press((app_remote_mode_t)99, APP_REMOTE_BTN_OK, APP_REMOTE_CLICK,
                            &down, &up) == false);
    assert(app_remote_press(APP_REMOTE_MODE_READER, (app_remote_btn_t)99, APP_REMOTE_CLICK,
                            &down, &up) == false);
    assert(app_remote_press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_OK, (app_remote_press_t)99,
                            &down, &up) == false);
    assert(app_remote_press(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_OK, APP_REMOTE_CLICK,
                            NULL, &up) == false);

    // ---- 界面文案 ----
    assert(strcmp(app_remote_action_text(APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_OK,
                                         APP_REMOTE_CLICK), "开始放映") == 0);
    assert(strcmp(app_remote_action_text(APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_OK,
                                         APP_REMOTE_LONG), "快门 / 确认") == 0);
    assert(app_remote_action_text(APP_REMOTE_MODE_READER, APP_REMOTE_BTN_UP,
                                  APP_REMOTE_LONG) == NULL);
    assert(app_remote_action_text((app_remote_mode_t)99, APP_REMOTE_BTN_OK,
                                  APP_REMOTE_CLICK) == NULL);

    printf("test_app_remote: PASS\n");
    return 0;
}