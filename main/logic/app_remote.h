// main/logic/app_remote.h —— "万能遥控"的按键映射纯逻辑。
//
// 设备以 BLE HID 外设的身份出现：对外是一个蓝牙键盘 + 一个消费类控制设备（媒体键）。
// 主机（手机 / 电脑 / 电视盒子）收到的是标准 HID 报文，所以不需要对方装任何 App，
// 也不需要互联网——配对一次之后，三键就是遥控器。
//
// 本模块只回答"某个模式的某个键，该发什么报文"，不碰蓝牙、不碰界面，因此每种模式的
// 映射都能在主机上逐条断言，避免"文档说 OK 是快门、代码其实发了回车"这类错位。
//
// 关于空调 / 电视红外遥控：那需要红外发射管，本机硬件没有该引脚，因此做不到，本模块
// 只覆盖走蓝牙 HID 的场景（蓝牙电视、手机、电脑、平板、部分机顶盒）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// HID 报文长度：键盘 8 字节（修饰键 + 保留 + 6 个按键），消费类 2 字节（16 位用途码）。
#define APP_REMOTE_KB_LEN   8
#define APP_REMOTE_CONS_LEN 2

// HID 报告 ID 与对应的 map 下标。键盘与消费类分成两份 report map：主机端会把它们
// 识别成"键盘"和"媒体控制器"两个设备，兼容性比塞进同一份 map 更好。
#define APP_REMOTE_RID_KEYBOARD 1
#define APP_REMOTE_RID_CONSUMER 2
#define APP_REMOTE_MAP_KEYBOARD 0
#define APP_REMOTE_MAP_CONSUMER 1

typedef enum {
    APP_REMOTE_MODE_READER = 0,   // 电子书翻页
    APP_REMOTE_MODE_VOLUME,       // 音量控制
    APP_REMOTE_MODE_SLIDES,       // PPT 演示
    APP_REMOTE_MODE_MEDIA,        // 媒体播放与自拍快门
    APP_REMOTE_MODE_COUNT,
} app_remote_mode_t;

typedef enum {
    APP_REMOTE_BTN_UP = 0,
    APP_REMOTE_BTN_DOWN,
    APP_REMOTE_BTN_OK,
} app_remote_btn_t;

typedef enum {
    APP_REMOTE_CLICK = 0,
    APP_REMOTE_LONG,
} app_remote_press_t;

// 一条待发送的 HID 报文。data 已按 HID 规范排好（消费类为小端 16 位用途码）。
typedef struct {
    uint8_t map_index;
    uint8_t report_id;
    uint8_t length;
    uint8_t data[APP_REMOTE_KB_LEN];
} app_remote_report_t;

// 查一次按键映射。press 为 CLICK 或 LONG；无映射返回 false。
// 命中时 down 为"按下"报文、up 为"松开"报文（全 0），调用方先发 down、短延时后发 up，
// 主机才会把它当成一次完整敲击而不是长按。
bool app_remote_press(app_remote_mode_t mode, app_remote_btn_t btn, app_remote_press_t press,
                      app_remote_report_t *down, app_remote_report_t *up);

// 模式名（界面标题）："电子书翻页" / "音量控制" / "PPT 演示" / "媒体与拍照"。
const char *app_remote_mode_name(app_remote_mode_t mode);

// 该模式下三键各自做什么，用于界面上的按键说明。无映射的键返回 NULL（界面应跳过）。
const char *app_remote_action_text(app_remote_mode_t mode, app_remote_btn_t btn,
                                   app_remote_press_t press);

// 下一个模式（循环）。便于长按 DOWN 依次切换。
app_remote_mode_t app_remote_mode_next(app_remote_mode_t mode);