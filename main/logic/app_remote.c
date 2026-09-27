// main/logic/app_remote.c —— "万能遥控"按键映射表。
//
// 每种模式就是一张"按了什么键、发什么报文"的登记表。界面、蓝牙发送都只是查这张表，
// 因此改键位只改一处，测试也只针对这张表。
#include "app_remote.h"

#include <stddef.h>
#include <string.h>

// ---- HID 用途码（Usage ID） ----
// 键盘：USB HID Usage Tables, Keyboard/Keypad page (0x07)。
#define KB_PAGE_UP    0x4B
#define KB_PAGE_DOWN  0x4E
#define KB_ENTER      0x28
#define KB_ESC        0x29
#define KB_F5         0x3E
#define KB_B          0x05

// 消费类：Consumer page (0x0C)，16 位用途码。
#define CONS_PLAY_PAUSE  0x00CD
#define CONS_VOL_UP      0x00E9
#define CONS_VOL_DOWN    0x00EA
#define CONS_MUTE        0x00E2
#define CONS_NEXT_TRACK  0x00B5
#define CONS_PREV_TRACK  0x00B6

typedef struct {
    app_remote_mode_t  mode;
    app_remote_btn_t   btn;
    app_remote_press_t press;
    uint8_t            map_index;   // APP_REMOTE_MAP_KEYBOARD / CONSUMER
    uint16_t           usage;       // 键盘为 1 字节用途码，消费类为 16 位
    const char        *text;        // 界面上的按键说明
} remote_binding_t;

// 按键登记表。顺序不影响查表结果，但按"模式 -> 键"排列便于对照阅读。
//
// 一条硬约束：**长按 OK 永远不在这里登记**。界面上长按 OK 恒为"返回上一级"，按键回调
// 在查表之前就把它消费掉了，所以登记了也永远发不出去——那只会让界面显示一个按了没反应
// 的假功能。所有"次级动作"因此落在长按 ↑ / ↓ 上；每个模式最多 5 个可达动作
// （↑↓OK 短按 + ↑↓ 长按）。
static const remote_binding_t BINDINGS[] = {
    // ---- 电子书翻页 ----
    { APP_REMOTE_MODE_READER, APP_REMOTE_BTN_UP,   APP_REMOTE_CLICK, APP_REMOTE_MAP_KEYBOARD, KB_PAGE_UP,   "上一页" },
    { APP_REMOTE_MODE_READER, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, APP_REMOTE_MAP_KEYBOARD, KB_PAGE_DOWN, "下一页" },
    { APP_REMOTE_MODE_READER, APP_REMOTE_BTN_OK,   APP_REMOTE_CLICK, APP_REMOTE_MAP_KEYBOARD, KB_ENTER,     "确认 / 打开" },
    { APP_REMOTE_MODE_READER, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG,  APP_REMOTE_MAP_KEYBOARD, KB_ESC,       "返回书架" },

    // ---- 音量控制 ----
    { APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_UP,   APP_REMOTE_CLICK, APP_REMOTE_MAP_CONSUMER, CONS_VOL_UP,     "音量+" },
    { APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, APP_REMOTE_MAP_CONSUMER, CONS_VOL_DOWN,   "音量−" },
    { APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_OK,   APP_REMOTE_CLICK, APP_REMOTE_MAP_CONSUMER, CONS_PLAY_PAUSE, "播放 / 暂停" },
    { APP_REMOTE_MODE_VOLUME, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG,  APP_REMOTE_MAP_CONSUMER, CONS_MUTE,       "静音切换" },

    // ---- PPT 演示 ----
    { APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_UP,   APP_REMOTE_CLICK, APP_REMOTE_MAP_KEYBOARD, KB_PAGE_UP,   "上一页" },
    { APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, APP_REMOTE_MAP_KEYBOARD, KB_PAGE_DOWN, "下一页" },
    { APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_OK,   APP_REMOTE_CLICK, APP_REMOTE_MAP_KEYBOARD, KB_F5,        "开始放映" },
    { APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_UP,   APP_REMOTE_LONG,  APP_REMOTE_MAP_KEYBOARD, KB_B,         "黑屏" },
    { APP_REMOTE_MODE_SLIDES, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG,  APP_REMOTE_MAP_KEYBOARD, KB_ESC,       "退出放映" },

    // ---- 万能遥控（媒体播放 + 拍照快门） ----
    { APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_OK,   APP_REMOTE_CLICK, APP_REMOTE_MAP_CONSUMER, CONS_PLAY_PAUSE, "播放 / 暂停" },
    { APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_UP,   APP_REMOTE_CLICK, APP_REMOTE_MAP_CONSUMER, CONS_VOL_UP,     "音量+ / 快门" },
    { APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_DOWN, APP_REMOTE_CLICK, APP_REMOTE_MAP_CONSUMER, CONS_VOL_DOWN,   "音量−" },
    { APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_UP,   APP_REMOTE_LONG,  APP_REMOTE_MAP_CONSUMER, CONS_NEXT_TRACK, "下一曲" },
    { APP_REMOTE_MODE_MEDIA, APP_REMOTE_BTN_DOWN, APP_REMOTE_LONG,  APP_REMOTE_MAP_CONSUMER, CONS_PREV_TRACK, "上一曲" },
};

static const remote_binding_t *find_binding(app_remote_mode_t mode, app_remote_btn_t btn,
                                            app_remote_press_t press)
{
    for (size_t i = 0; i < sizeof(BINDINGS) / sizeof(BINDINGS[0]); i++) {
        const remote_binding_t *b = &BINDINGS[i];
        if (b->mode == mode && b->btn == btn && b->press == press) return b;
    }
    return NULL;
}

// 把一条映射填成 HID 报文。
static void fill_reports(const remote_binding_t *b, app_remote_report_t *down,
                         app_remote_report_t *up)
{
    memset(down, 0, sizeof(*down));
    memset(up, 0, sizeof(*up));

    if (b->map_index == APP_REMOTE_MAP_CONSUMER) {
        down->map_index = APP_REMOTE_MAP_CONSUMER;
        down->report_id = APP_REMOTE_RID_CONSUMER;
        down->length = APP_REMOTE_CONS_LEN;
        down->data[0] = (uint8_t)(b->usage & 0xFF);        // 小端
        down->data[1] = (uint8_t)((b->usage >> 8) & 0xFF);

        up->map_index = APP_REMOTE_MAP_CONSUMER;
        up->report_id = APP_REMOTE_RID_CONSUMER;
        up->length = APP_REMOTE_CONS_LEN;                  // 全 0 即"松开"
    } else {
        down->map_index = APP_REMOTE_MAP_KEYBOARD;
        down->report_id = APP_REMOTE_RID_KEYBOARD;
        down->length = APP_REMOTE_KB_LEN;
        down->data[0] = 0;                                 // 修饰键
        down->data[1] = 0;                                 // 保留
        down->data[2] = (uint8_t)b->usage;                 // 第一个按键槽

        up->map_index = APP_REMOTE_MAP_KEYBOARD;
        up->report_id = APP_REMOTE_RID_KEYBOARD;
        up->length = APP_REMOTE_KB_LEN;
    }
}

bool app_remote_press(app_remote_mode_t mode, app_remote_btn_t btn, app_remote_press_t press,
                      app_remote_report_t *down, app_remote_report_t *up)
{
    if (mode < 0 || mode >= APP_REMOTE_MODE_COUNT) return false;
    if (btn < 0 || btn > APP_REMOTE_BTN_OK) return false;
    if (press < 0 || press > APP_REMOTE_LONG) return false;
    if (!down || !up) return false;

    const remote_binding_t *b = find_binding(mode, btn, press);
    if (!b) return false;

    fill_reports(b, down, up);
    return true;
}

const char *app_remote_mode_name(app_remote_mode_t mode)
{
    switch (mode) {
    case APP_REMOTE_MODE_READER: return "电子书翻页";
    case APP_REMOTE_MODE_VOLUME: return "音量控制";
    case APP_REMOTE_MODE_SLIDES: return "PPT 演示";
    case APP_REMOTE_MODE_MEDIA:  return "万能遥控";
    default:                     return "";
    }
}

const char *app_remote_action_text(app_remote_mode_t mode, app_remote_btn_t btn,
                                   app_remote_press_t press)
{
    if (mode < 0 || mode >= APP_REMOTE_MODE_COUNT) return NULL;
    if (btn < 0 || btn > APP_REMOTE_BTN_OK) return NULL;
    if (press < 0 || press > APP_REMOTE_LONG) return NULL;

    const remote_binding_t *b = find_binding(mode, btn, press);
    return b ? b->text : NULL;
}

app_remote_mode_t app_remote_mode_next(app_remote_mode_t mode)
{
    if (mode < 0 || mode >= APP_REMOTE_MODE_COUNT) return APP_REMOTE_MODE_READER;
    return (app_remote_mode_t)((mode + 1) % APP_REMOTE_MODE_COUNT);
}