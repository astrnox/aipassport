// main/app_metrics.h —— 内存与任务栈水位测量（内存/功耗优化计划 阶段 1）。
//
// 只做记录，不做任何优化：把 LVGL 池、内部堆与任务栈的实时水位打进日志，供真机
// 跑一遍曾经崩溃的操作序列时逐点对照，把"还有余量"从估算变成实测。计划要求覆盖
// 这些采样点：进入/离开每个模块页、作息页每次切标签、以及一段代表性使用之后。
//
// 线程约定：app_metrics_mem() 会读 LVGL 池状态，必须在持有 bsp_lvgl_lock() 时调用；
// app_metrics_stack() 只读 FreeRTOS 任务状态，任何任务里都能调用。
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 一次内存快照：LVGL 池（空闲 / 碎片率 / 历史峰值用量）与内部堆（空闲 / 最大连续块）。
// where 是采样点标签，例如 "进入:时间与日历"。
void app_metrics_mem(const char *where);

// 记录调用任务自己的栈高水位（单位：字）。它表示历史最小剩余量，越小越危险。
// 用于"某任务退不退出"都成立的场景：网络 worker 在退出前自己打一条最准。
void app_metrics_stack(const char *where);

// 登记 app_input 任务句柄。登记后 app_metrics_report() 会连同它的栈水位一起记录，
// 不必让按键任务自己周期性打日志。重复登记只覆盖句柄。
void app_metrics_set_input_task(TaskHandle_t task);

// 汇总采样：内存快照 + 输入任务与当前（LVGL）任务的栈水位。须在持有 LVGL 锁时调用。
void app_metrics_report(const char *where);