// main/ui/ui_tools.c —— 工具模块页：一列小工具，选中后用 OK 打开。
//
// 用户明确要求"身份页与工具页分开"，并采用"列表 + 子页"：工具页本身只做两件事——把
// 六个工具列清楚、把用户送进对应的全屏子页。每个工具都占一屏，子页在屏时工具页不处理
// 按键，只做转发；子页退出时会删掉自己的屏幕，因此工具页必须重建，否则屏幕上留下的是
// 一张已被删除的屏。
//
// 列表行右侧不是"状态值"，而是这个工具对普通用户到底有什么用（"蓝牙耳机丢哪了"），
// 因为"找设备""信道体检"这类名字本身不说明用途。
//
// 按键：↑↓ 选择   OK 打开   长按OK 返回主页。
//
// ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "lvgl.h"

#include <string.h>

#define TOOLS_CW (UI_W - 2 * UI_MARGIN_X)

typedef enum {
    TOOL_TOTP = 0,
    TOOL_VAULT,
    TOOL_FINDER,
    TOOL_REMOTE,
    TOOL_CHANNEL,
    TOOL_METRONOME,
    TOOL_BLELAB,
    TOOL_BLEDETECT,
    TOOL_WIFILAB,
    TOOL_COUNT,
} tool_t;

static const char *const TOOL_NAMES[TOOL_COUNT] = {
    "动态口令", "密码本", "找设备", "万能遥控", "信道体检", "节拍器", "BLE 实验", "BLE 检测", "Wi-Fi 实验",
};

// 右侧一句话说明：普通用户先看懂"这能干嘛"，再决定要不要打开。
static const char *const TOOL_HINTS[TOOL_COUNT] = {
    "验证码离线可算",
    "加密存账号密码",
    "耳机手环丢哪了",
    "当电脑电视遥控",
    "家里 Wi-Fi 卡不卡",
    "练琴打拍子",
    "向周围广播测试报文",
    "附近蓝牙在刷屏吗",
    "CTF/实验室无线测试",
};

static struct {
    ui_page_t page;
    int focus;
    int open;                 // 正在屏上的子页工具下标，-1 表示列表
    ui_row_t rows[TOOL_COUNT];
} s;

// ---------------------------------------------------------------------------
// 子页路由
// ---------------------------------------------------------------------------

static bool subpage_active(int tool)
{
    switch (tool) {
    case TOOL_TOTP:      return page_totp_active();
    case TOOL_VAULT:     return page_vault_active();
    case TOOL_FINDER:    return page_finder_active();
    case TOOL_REMOTE:    return page_remote_active();
    case TOOL_CHANNEL:   return page_channel_active();
    case TOOL_METRONOME: return page_metronome_active();
    case TOOL_BLELAB:    return page_blelab_active();
    case TOOL_BLEDETECT: return page_bledetect_active();
    case TOOL_WIFILAB:   return page_wifilab_active();
    default:             return false;
    }
}

static void subpage_enter(int tool)
{
    switch (tool) {
    case TOOL_TOTP:      page_totp_enter();      break;
    case TOOL_VAULT:     page_vault_enter();     break;
    case TOOL_FINDER:    page_finder_enter();    break;
    case TOOL_REMOTE:    page_remote_enter();    break;
    case TOOL_CHANNEL:   page_channel_enter();   break;
    case TOOL_METRONOME: page_metronome_enter(); break;
    case TOOL_BLELAB:    page_blelab_enter();    break;
    case TOOL_BLEDETECT: page_bledetect_enter(); break;
    case TOOL_WIFILAB:   page_wifilab_enter();   break;
    default: break;
    }
}

static void subpage_key(int tool, bsp_btn_t btn, bsp_btn_ev_t ev)
{
    switch (tool) {
    case TOOL_TOTP:      page_totp_key(btn, ev);      break;
    case TOOL_VAULT:     page_vault_key(btn, ev);     break;
    case TOOL_FINDER:    page_finder_key(btn, ev);    break;
    case TOOL_REMOTE:    page_remote_key(btn, ev);    break;
    case TOOL_CHANNEL:   page_channel_key(btn, ev);   break;
    case TOOL_METRONOME: page_metronome_key(btn, ev); break;
    case TOOL_BLELAB:    page_blelab_key(btn, ev);    break;
    case TOOL_BLEDETECT: page_bledetect_key(btn, ev); break;
    case TOOL_WIFILAB:   page_wifilab_key(btn, ev);   break;
    default: break;
    }
}

static void subpage_tick(int tool)
{
    switch (tool) {
    case TOOL_TOTP:      page_totp_tick();      break;
    case TOOL_VAULT:     page_vault_tick();     break;
    case TOOL_FINDER:    page_finder_tick();    break;
    case TOOL_REMOTE:    page_remote_tick();    break;
    case TOOL_CHANNEL:   page_channel_tick();   break;
    case TOOL_METRONOME: page_metronome_tick(); break;
    case TOOL_BLELAB:    page_blelab_tick();    break;
    case TOOL_BLEDETECT: page_bledetect_tick(); break;
    case TOOL_WIFILAB:   page_wifilab_tick();   break;
    default: break;
    }
}

static void subpage_exit(int tool)
{
    switch (tool) {
    case TOOL_TOTP:      page_totp_exit();      break;
    case TOOL_VAULT:     page_vault_exit();     break;
    case TOOL_FINDER:    page_finder_exit();    break;
    case TOOL_REMOTE:    page_remote_exit();    break;
    case TOOL_CHANNEL:   page_channel_exit();   break;
    case TOOL_METRONOME: page_metronome_exit(); break;
    case TOOL_BLELAB:    page_blelab_exit();    break;
    case TOOL_BLEDETECT: page_bledetect_exit(); break;
    case TOOL_WIFILAB:   page_wifilab_exit();   break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------

static void render_focus(void)
{
    for (int i = 0; i < TOOL_COUNT; i++) {
        ui_row_set_selected(s.rows[i], i == s.focus);
    }
    if (s.focus >= 0 && s.focus < TOOL_COUNT) {
        ui_scroll_into_view(s.rows[s.focus].obj);
    }
}

static void build_list(void)
{
    lv_obj_clean(s.page.content);

    ui_header_create(s.page.content, "工具", "选一个打开", NULL, NULL);

    lv_obj_t *list = ui_list_create(s.page.content);
    lv_obj_set_width(list, TOOLS_CW);
    for (int i = 0; i < TOOL_COUNT; i++) {
        s.rows[i] = ui_row_create(list, TOOL_NAMES[i], TOOL_HINTS[i]);
    }

    if (s.focus < 0) s.focus = 0;
    if (s.focus >= TOOL_COUNT) s.focus = TOOL_COUNT - 1;
    render_focus();
    ui_page_set_hint("↑↓ 选择   OK 打开   长按OK 返回主页");
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_tools_enter(void)
{
    int focus = s.focus;
    memset(&s, 0, sizeof(s));
    s.focus = focus;
    s.open = -1;
    s.page = ui_page_create(NULL);
    build_list();
}

void page_tools_exit(void)
{
    // 子页仍占屏时先收掉它，避免它的定时任务/蓝牙角色在没人看管的情况下继续跑。
    if (s.open >= 0 && subpage_active(s.open)) subpage_exit(s.open);
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
    s.open = -1;
}

void page_tools_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 子页在屏：只做转发。它自己决定何时退出；退出后工具页必须重建。
    if (s.open >= 0) {
        subpage_key(s.open, btn, ev);
        if (!subpage_active(s.open)) {
            int focus = s.focus;
            page_tools_exit();
            s.focus = focus;
            s.open = -1;
            s.page = ui_page_create(NULL);
            build_list();
        }
        return;
    }

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        ui_app_go_home();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s.focus = (s.focus + TOOL_COUNT - 1) % TOOL_COUNT;
        render_focus();
    } else if (btn == BSP_BTN_DOWN) {
        s.focus = (s.focus + 1) % TOOL_COUNT;
        render_focus();
    } else if (btn == BSP_BTN_OK) {
        s.open = s.focus;
        subpage_enter(s.open);
        // 子页若因蓝牙/Wi-Fi 被占用等原因立即失败并退出，这里立刻把列表恢复回来，
        // 不让用户停在一张没有内容的屏幕上。
        if (!subpage_active(s.open)) {
            s.open = -1;
        }
    }
}

void page_tools_tick(void)
{
    if (s.open >= 0) subpage_tick(s.open);
}