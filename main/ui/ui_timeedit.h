// main/ui/ui_timeedit.h —— 数值字段编辑浮层：按 PRD 的"输入与设置页"约定操作。
//
// 三个按键在编辑态下语义固定，所有需要填数字的地方（设置时间、生日、提醒钟点）都复用
// 这一层，避免每个页面各自发明一套加减规则：
//   短按 UP / DOWN  当前字段 -1 / +1
//   长按 UP / DOWN  当前字段 -step / +step（大步）
//   短按 OK         跳到下一个字段；在最后一个字段上等同保存
//   长按 OK         保存并关闭
// 字段值在 min..max 间夹紧，不做循环，避免长按滑过目标值时绕回另一端。
#pragma once

#include "bsp_button.h"
#include "lvgl.h"

#include <stdbool.h>

typedef struct {
    const char *name;   // 字段名，如 "年"
    int min;
    int max;
    int step;           // 长按步长，>= 1
    // 非 NULL 时该字段是"选项"而非数值：值即选项下标，显示 names[值]，
    // 如上例的重复方式（每天 / 工作日 / 一次性）。min/max 需覆盖所有下标。
    const char *const *names;
} ui_timeedit_field_t;

// 在 parent（页面屏幕）上打开编辑浮层。values 为输入输出数组，长度 count，调用方持有。
// done 在保存或取消时回调一次；saved 为 true 表示用户按了保存。values 中的值始终是
// 用户最后的编辑结果，即使取消也可以安全忽略。
void ui_timeedit_open(lv_obj_t *parent, const char *title,
                      const ui_timeedit_field_t *fields, int *values, int count,
                      void (*done)(bool saved, void *user), void *user);

bool ui_timeedit_active(void);
bool ui_timeedit_handle(bsp_btn_t btn, bsp_btn_ev_t ev);
void ui_timeedit_close(void);
