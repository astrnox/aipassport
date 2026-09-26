// main/ui/ui_pages.h —— 各模块页面的统一入口声明。
//
// 每个模块页实现 enter/exit/key/tick 四个回调，注册到 ui_app.c 的模块表。
// 主页由控制器直接持有，不在此表内。
#pragma once

#include "bsp_button.h"

#include <stdbool.h>

// 1 时间与日历：万年历 / 时间进度 / 秒表·计时器
void page_time_enter(void);
void page_time_exit(void);
void page_time_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_time_tick(void);

// 2 专注与效率：番茄钟 / 本地提醒
void page_focus_enter(void);
void page_focus_exit(void);
void page_focus_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_focus_tick(void);

// 3 作息与倒计时：时间轴 / 倒计时 / 配置
void page_routine_enter(void);
void page_routine_exit(void);
void page_routine_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_routine_tick(void);

// 4 身份与工具：电子工牌 / 动态口令 / 密码本
void page_identity_enter(void);
void page_identity_exit(void);
void page_identity_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_identity_tick(void);

// 4.1 密码本（从身份页进入，是全屏子页面而非独立模块）
void page_vault_enter(void);
void page_vault_exit(void);
void page_vault_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_vault_tick(void);
// 身份页返回时用：密码本当前是否占用着屏幕。
bool page_vault_active(void);

// 5 英雄联盟赛事中心
void page_esports_enter(void);
void page_esports_exit(void);
void page_esports_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_esports_tick(void);

// 6 系统设置与配网
void page_settings_enter(void);
void page_settings_exit(void);
void page_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_settings_tick(void);

// 主页（由 ui_app.c 持有）
void page_home_enter(void);
void page_home_exit(void);
void page_home_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_home_tick(void);

// 快捷面板（主页长按 UP）：静音 / 主题 / 亮度 / 开始番茄钟
void home_quick_open(void);
void home_quick_close(void);
void home_quick_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool home_quick_active(void);

// 首次引导（三步：时间 / 作息模板 / 口令密钥，均可跳过）
void onboarding_open(void);
void onboarding_close(void);
void onboarding_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void onboarding_tick(void);
bool onboarding_active(void);
