// main/ui/ui_novel.h —— 小说阅读页（主页模块之一）。
#pragma once

#include "bsp_button.h"

// 阅读页：正文由手机配置页上传（见 net/app_net 的 /novel），本页只负责显示。
void page_novel_enter(void);
void page_novel_exit(void);
void page_novel_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void page_novel_tick(void);