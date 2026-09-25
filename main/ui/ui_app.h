// main/ui/ui_app.h —— 应用控制器：导航、息屏与全局节拍。
//
// 控制器是界面层唯一知道"当前在哪个页面"的地方。页面不直接删除或创建彼此，
// 需要跳转时调用 ui_app_go_home() / ui_app_open_module()，由控制器统一处理
// 旧页面的 exit 与新页面的 enter，避免页面互相引用导致的生命周期混乱。
//
// 锁约定：ui_app_* 的页面创建/删除与按键分发都在 bsp_lvgl_lock() 保护下进行，
// 因此页面回调不需要自己再加锁；网络等慢操作必须交给 app_net 的工作任务，
// 页面回调里只做轻量的界面更新。
#pragma once

#include "bsp_button.h"

#include <stdbool.h>

// 模块页统一接口。enter/exit/tick 在 LVGL 锁内调用；key 也在锁内调用。
typedef struct {
    const char *title;
    void (*enter)(void);
    void (*exit)(void);
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);
    void (*tick)(void);
} ui_module_t;

// 创建主页、启动 1 秒全局节拍。必须在 bsp_lvgl_init() 之后调用一次（持锁）。
void ui_app_start(void);

// 由输入任务调用：分发按键、处理熄屏唤醒。内部自行加解锁。
void ui_app_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 页面请求导航（均要求调用方已持 LVGL 锁）。
void ui_app_go_home(void);
void ui_app_open_module(int index);
void ui_app_show_quick_panel(void);
void ui_app_show_onboarding(void);

// 模块表访问（主页构建列表时使用）。
int ui_app_module_count(void);
const char *ui_app_module_title(int index);

// 主页信息卡与状态栏刷新（赛事数据更新后调用）。
void ui_app_refresh_home(void);
void ui_app_refresh_status(void);

// 页面有用户可见变化时调用，重置息屏计时。
void ui_app_note_activity(void);

// 当前是否处于熄屏状态。
bool ui_app_is_asleep(void);
