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

// 4 身份：电子工牌
void page_identity_enter(void);
void page_identity_exit(void);
void page_identity_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_identity_tick(void);

// 5 工具：工具列表（动态口令 / 密码本 / 找设备 / 万能遥控 / 信道体检 / 节拍器）。
// 工具页只负责选择与转发，具体功能都在各自的全屏子页面里。
void page_tools_enter(void);
void page_tools_exit(void);
void page_tools_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_tools_tick(void);

// 5.1 动态口令（工具页子页）
void page_totp_enter(void);
void page_totp_exit(void);
void page_totp_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_totp_tick(void);
bool page_totp_active(void);

// 5.2 密码本（工具页子页）
void page_vault_enter(void);
void page_vault_exit(void);
void page_vault_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_vault_tick(void);
bool page_vault_active(void);

// 5.3 找设备（蓝牙查找器，工具页子页）
void page_finder_enter(void);
void page_finder_exit(void);
void page_finder_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_finder_tick(void);
bool page_finder_active(void);

// 5.4 万能遥控（蓝牙 HID，工具页子页）
void page_remote_enter(void);
void page_remote_exit(void);
void page_remote_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_remote_tick(void);
bool page_remote_active(void);

// 5.5 信道体检（工具页子页）
void page_channel_enter(void);
void page_channel_exit(void);
void page_channel_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_channel_tick(void);
bool page_channel_active(void);

// 5.6 节拍器（工具页子页）
void page_metronome_enter(void);
void page_metronome_exit(void);
void page_metronome_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_metronome_tick(void);
bool page_metronome_active(void);

// 5.7 BLE 实验（工具页子页：仅广播 broadcaster，见 net/app_ble 的 advertiser 角色）
void page_blelab_enter(void);
void page_blelab_exit(void);
void page_blelab_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_blelab_tick(void);
bool page_blelab_active(void);

// 5.8 BLE 检测（工具页子页：被动侦测 AirTag / 苹果连续广播轰炸，复用 finder 扫描，绝不发射）
void page_bledetect_enter(void);
void page_bledetect_exit(void);
void page_bledetect_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_bledetect_tick(void);
bool page_bledetect_active(void);

// 5.9 Wi-Fi 实验（工具页子页：仅自有 / 授权环境，复刻 GhostESP 的 CTF 无线测试报文）
void page_wifilab_enter(void);
void page_wifilab_exit(void);
void page_wifilab_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_wifilab_tick(void);
bool page_wifilab_active(void);

// 6 英雄联盟赛事中心
void page_esports_enter(void);
void page_esports_exit(void);
void page_esports_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_esports_tick(void);

// 7 小说：离线阅读（正文由手机配置页上传）
void page_novel_enter(void);
void page_novel_exit(void);
void page_novel_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_novel_tick(void);

// 8 系统设置与配网
void page_settings_enter(void);
void page_settings_exit(void);
void page_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_settings_tick(void);

// 主页（由 ui_app.c 持有）
void page_home_enter(void);
void page_home_exit(void);
void page_home_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_home_tick(void);

// 快捷面板（主页长按 UP）：静音 / 主题 / 亮度 / 开始番茄钟 / 免打扰 / 桌宠捣乱
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
