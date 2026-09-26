// main/ui/ui_theme.h —— 应用统一视觉规范与通用控件。
//
// 本文件是界面层的唯一样式来源：颜色、字号、布局尺寸与通用控件都从这里取，
// 页面代码不再各自决定颜色或字号，避免同一个"进行中"状态在不同页面显示成两种红。
//
// 视觉取向：240x320 竖屏、三键操作、常亮小屏。信息密度高但克制，卡片用 1px 描边
// 与左侧 2px 语义色指示条区分层级，不依赖阴影或渐变——单 DMA 缓冲、无 PSRAM 的
// 设备上大面积重绘会掉帧。
#pragma once

#include "bsp_button.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------
// 中文使用 assets/fonts 生成的 app_font_*（完整覆盖常用汉字集，见 tools/gen_fonts.py）。
// 展示级数字只用 ASCII，使用 LVGL 自带 Montserrat，避免为几个数字多带一套中文字形。
LV_FONT_DECLARE(app_font_12);
LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

// 挂载可写字体描述符并把同号 Montserrat 作为符号回退。必须在创建任何页面之前调用一次。
void ui_fonts_init(void);

extern const lv_font_t *const ui_font_display;   // 40px，仅数字：时钟、验证码、比分
extern const lv_font_t *const ui_font_display_s; // 32px，仅数字：倒计时、计时器
extern const lv_font_t *const ui_font_title;     // 20px，页面标题与主数据
extern const lv_font_t *const ui_font_body;      // 16px，列表项与正文
extern const lv_font_t *const ui_font_hint;      // 12px，提示条与辅助状态

// ---------------------------------------------------------------------------
// 主题
// ---------------------------------------------------------------------------
// 深色用于夜间护眼，浅色用于白天。本机是透射式 LCD、背光常亮，深色并不省电，
// 任何界面文案都不得暗示深色更省电——省电只由息屏负责。
typedef enum {
    UI_THEME_DARK = 0,
    UI_THEME_LIGHT,
} ui_theme_mode_t;

void ui_theme_set(ui_theme_mode_t mode);
ui_theme_mode_t ui_theme_mode(void);
// 依据本地时间选择主题：19:00 至次日 07:00 用深色。auto_mode 为 false 时保持当前主题。
void ui_theme_apply_auto(bool auto_mode, int hour);

uint32_t ui_c_bg(void);
uint32_t ui_c_card(void);
// 卡片内部再嵌一层容器（凭证面板、分组框）时用的底色。比 card 再亮/再深一档，
// 用"容器叠容器"表达层级，不靠阴影——Material 3 的 surface container 也是这个思路。
uint32_t ui_c_panel(void);
uint32_t ui_c_text(void);
uint32_t ui_c_dim(void);
uint32_t ui_c_accent(void);
uint32_t ui_c_live(void);
uint32_t ui_c_soon(void);
uint32_t ui_c_done(void);
uint32_t ui_c_ok(void);
uint32_t ui_c_warn(void);
uint32_t ui_c_sel(void);      // 选中行背景
uint32_t ui_c_border(void);

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
#define UI_W           240
#define UI_H           320
#define UI_STATUS_H    26
// 提示条高 34（两道 12px 行）。按键约定在 240px 宽里一行写不完：单行只放得下约
// 19 个全宽字，而"↑↓翻月 OK进度 长按↑农历 长按↓换页 长按OK返回"这类完整说明最长
// 可达 330px。压到一行只能靠删信息，所以这里给两行，让提示条始终能把按键讲清楚。
#define UI_HINT_H      34
#define UI_CONTENT_H   (UI_H - UI_STATUS_H - UI_HINT_H)   // 260
#define UI_MARGIN_X    8
#define UI_ROW_H       40
#define UI_CARD_GAP    8

// ---------------------------------------------------------------------------
// 页面骨架：状态栏 + 内容区 + 提示条
// ---------------------------------------------------------------------------
typedef struct {
    lv_obj_t *scr;
    lv_obj_t *content;   // 可滚动内容区，页面把控件挂到这里
    lv_obj_t *hint;      // 底部提示条
} ui_page_t;

// 新建整页。hint_text 为底部提示条文案（UTF-8，可为 NULL）。
ui_page_t ui_page_create(const char *hint_text);
void ui_page_set_hint(const char *text);
// 临时把提示条换成一次性反馈（如"已保存"），ms 毫秒后自动恢复按键提示。
void ui_hint_flash(const char *text, uint32_t ms);

// 状态栏。同一时刻只有一个页面在屏，故内部状态为单例。
void ui_status_bar_refresh(void);                    // 刷新时钟与电量
void ui_status_bar_set_net(const char *text, bool online);
void ui_status_bar_set_badge(const char *text, bool live);

// ---------------------------------------------------------------------------
// 通用控件
// ---------------------------------------------------------------------------
lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color);

// 卡片：1px 描边 + 可选左侧 2px 语义色指示条（accent 为 0 时不画）。
lv_obj_t *ui_card_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent);

// 等宽数字行：每个字符一个定宽标签，避免时钟跳变时整串左右抖动。
typedef struct {
    lv_obj_t *labels[10];
    int count;
} ui_digit_row_t;

ui_digit_row_t ui_digit_row_create(lv_obj_t *parent, int digits,
                                   const lv_font_t *font, uint32_t color, int digit_w);
void ui_digit_row_set(ui_digit_row_t *row, const char *text, uint32_t color);

// 列表行：左侧标题、右侧状态值、可选左侧选中指示条。
typedef struct {
    lv_obj_t *obj;
    lv_obj_t *indicator;
    lv_obj_t *value;
} ui_row_t;

lv_obj_t *ui_list_create(lv_obj_t *parent);
ui_row_t ui_row_create(lv_obj_t *parent, const char *title, const char *value);
void ui_row_set_value(ui_row_t row, const char *value);
void ui_row_set_selected(ui_row_t row, bool selected);
void ui_row_set_title_color(ui_row_t row, uint32_t color);

// 顶部标签页（赛事的赛程/积分榜/战队，时间进度的尺度切换）。
lv_obj_t *ui_tabs_create(lv_obj_t *parent, const char *const *names, int count);
void ui_tabs_select(lv_obj_t *tabs, int index);

// 页面顶部标题行：左侧标题、右侧状态。title_out/right_out 可为 NULL。
lv_obj_t *ui_header_create(lv_obj_t *parent, const char *title, const char *right,
                           lv_obj_t **title_out, lv_obj_t **right_out);

// 横向进度条（番茄钟进度、刷新进度）。permille 0..1000。
lv_obj_t *ui_progress_create(lv_obj_t *parent, int w, int h, uint32_t color);
void ui_progress_set(lv_obj_t *bar, int permille);

// 把 obj 滚动到其可滚动祖先的可见区域（列表选中项常用）。
void ui_scroll_into_view(lv_obj_t *obj);

// 环形进度（TOTP 剩余时间、番茄钟进度），中心可叠数字。
lv_obj_t *ui_ring_create(lv_obj_t *parent, int size, int x, int y, uint32_t color);
void ui_ring_set(lv_obj_t *ring, int permille, uint32_t color);

// 空状态：说明为什么为空与下一步动作，不出现纯空白页。
lv_obj_t *ui_empty_create(lv_obj_t *parent, const char *title, const char *body);

// 顶部横幅：离线、时间未同步、失败原因等。返回横幅对象，页面自行删除。
lv_obj_t *ui_banner_create(lv_obj_t *parent, const char *text, uint32_t color);

// ---------------------------------------------------------------------------
// 二次确认对话框（覆盖在当前页之上）
// ---------------------------------------------------------------------------
typedef void (*ui_dialog_cb_t)(bool confirmed, void *user);

void ui_dialog_open(lv_obj_t *parent, const char *title, const char *body,
                    const char *confirm_text, ui_dialog_cb_t cb, void *user);
bool ui_dialog_is_open(void);
// 对话框打开时由控制器把按键转交到这里；返回 true 表示已消费。
bool ui_dialog_handle(bsp_btn_t btn, bsp_btn_ev_t ev);
void ui_dialog_close(void);

// ---------------------------------------------------------------------------
// 通知弹层（提醒到点、错过提醒汇总）
// ---------------------------------------------------------------------------
// 与确认对话框的区别：只有一个"知道了"，不做二选一，用于"发生过什么事"的告知。
// 弹层挂在传入的屏幕上；调用方负责在该屏幕被删除前关闭，否则控件会随之失效。
void ui_alert_open(lv_obj_t *parent, const char *title, const char *body);
void ui_alert_close(void);
bool ui_alert_is_open(void);
// 弹层打开时由控制器把按键转交到这里；返回 true 表示已消费（任意按键关闭）。
bool ui_alert_handle(bsp_btn_t btn, bsp_btn_ev_t ev);
