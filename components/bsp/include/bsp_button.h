// components/bsp/include/bsp_button.h
// 三个按键共用一个 ADC 引脚,靠分压电阻区分。电压窗口见 bsp_pins.h。
#pragma once

#include "esp_err.h"

// 按键索引。数量用 bsp_pins.h 的 BSP_BTN_COUNT(硬件属性,归引脚表管),
// 这里不再定义尾项计数,避免出现 BSP_BTN_COUNT / BSP_BTN_COUNT_ 两个近似名字。
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

// 一次物理按压最多产生两个事件，顺序固定：
//   BSP_BTN_PRESS  按下瞬间(低延迟,适合游戏类即时响应)
//   BSP_BTN_CLICK  抬起即发的一次短按
//   BSP_BTN_LONG   按住达到 BSP_BTN_LONG_PRESS_MS 时长按下；之后抬起不再补发 CLICK
// 即单次按住 500 毫秒以内只出 PRESS + CLICK，超过阈值只出 PRESS + LONG。
//
// 刻意不做双击手势：双击需要判定窗口，会把每次单击都拖到窗口结束才确认，手感变钝且
// 容易误触；连续快按还会被合并成双击而丢失单次动作。全部界面操作只由短按与长按构成，
// 见 docs/product/passport-toolbox-prd.zh_CN.md 的交互原则。
typedef enum {
    BSP_BTN_PRESS = 0,
    BSP_BTN_CLICK,
    BSP_BTN_LONG,
} bsp_btn_ev_t;

// 长按阈值(毫秒)。产品验收口径为达到 500 毫秒即触发。
#define BSP_BTN_LONG_PRESS_MS 500

// 按键事件回调。运行于 button 组件使用的共享 esp_timer 任务,只能入队或执行同等级
// 的有界操作；勿在其中阻塞、访问 LVGL 或做重活。
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 成功调用可重复，并更新回调与 user；失败会回滚本次已创建的按键和 ADC 资源。
// ADC 校准失败时返回错误而不是把无效电压解码为按键，修正故障后可重试。
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// 读当前 ADC 原始电压(mV)。松开时约 3300;按住某键时约为该键的分压值。
// ★ 换了分压/上拉阻值后,用它测出自己的三档电压,再改 bsp_pins.h 的 BSP_BTN_MV_TABLE。
// 读取失败返回 -1。
int bsp_button_read_mv(void);
