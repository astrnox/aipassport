// main/ui/ui_pet.h —— 桌宠火柴人的界面层。
//
// 逻辑层（logic/app_pet）决定"此刻该做什么动作、什么表情、说什么话"，本层只负责把姿态
// 画成线段，因此这里没有状态机，只有一份共享的 app_pet_t 和若干"绘制装置"：
//   - 主页的桌宠卡：大一点的装置，带一句台词（颜文字或短句）。
//   - 状态栏的小剪影：极小装置，覆盖所有页面，让桌宠在哪个页面都在场。
// 所有装置共用同一份状态，所以它们是同一只桌宠，而不是几只各做各的——用户能看出这是
// 同一个"人"，而不是每页一个不同的装饰。
//
// 为什么用折线而不是画布：这块屏没有 PSRAM，一块 60x60 的 RGB565 画布就要 7KB 常驻内存；
// 而一条折线只是一个轻量控件，且动作变化时只失效线条经过的小区域，不必重画整块位图，
// 单 DMA 缓冲下更不容易掉帧。
#pragma once

#include "logic/app_pet.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// 一只桌宠的绘制装置。随 parent 一起销毁，不需要（也不应该）手动释放。
typedef struct ui_pet_rig ui_pet_rig_t;

// 在 parent 的 (x, y) 创建 w×h 的装置。火柴人画在左侧 h×h 的方块里，其余横向空间留给
// 台词；speech_font 为 NULL 时不显示台词（状态栏剪影就是这样）。装置数超出上限时返回
// NULL，调用方跳过桌宠即可，不应因此失败。
ui_pet_rig_t *ui_pet_rig_create(lv_obj_t *parent, int x, int y, int w, int h,
                                const lv_font_t *speech_font, uint32_t speech_color);

// 立即按当前状态重绘一次。页面刚建好时调用，免得等到下一个动画节拍才出现。
void ui_pet_refresh(void);

// ---- 全局事件 ----
// 把"发生了什么"告诉桌宠：番茄钟到点、日程提醒、低电量、追踪到防丢器等。
// 会立刻重绘一次，因此必须在持有 bsp_lvgl_lock() 的任务里调用（与其它界面操作同一约定）。
void ui_pet_event(app_pet_event_t ev);
void ui_pet_set_mischief(bool on);
bool ui_pet_mischief(void);

// 情绪对应的语义色。
uint32_t ui_pet_mood_color(app_pet_mood_t mood);