// main/net/app_net.h —— 联网服务：Wi-Fi STA、NTP 校时、赛事拉取与热点配网。
//
// 离线优先：本模块是设备上唯一会打开 Wi-Fi 的地方，且只在明确请求时打开（进入赛事
// 中心、设置里手动校时、开启配网）。退出对应页面即释放，不做后台常驻联网，避免常亮
// 小屏在无比赛时持续耗电。
//
// 线程约定：除 app_net_init()/app_net_wifi_* 外，网络请求都在内部 worker task 中执行；
// 页面回调只读取状态，不阻塞。所有对 app_state 的写入都在 worker 内完成。
#pragma once

#include "esp_err.h"

#include <stdbool.h>

// 全局一次性初始化：准备 NVS（幂等）、默认事件循环与 netif。app_main 启动时调用。
// 只准备基础设施，不打开 Wi-Fi 射频。
esp_err_t app_net_init(void);

// ---------------------------------------------------------------------------
// Wi-Fi STA
// ---------------------------------------------------------------------------
// 打开 Wi-Fi 并按已保存凭证连接。无凭证时返回 ESP_ERR_NOT_FOUND，保持离线，不报错。
// 幂等：已连接或正在连接时直接返回 ESP_OK。
esp_err_t app_net_wifi_start(void);
// 断开并释放 Wi-Fi（保留已保存凭证）。幂等。
void      app_net_wifi_stop(void);
bool      app_net_wifi_connected(void);

// 保存凭证并（重新）连接。ssid 为空返回 ESP_ERR_INVALID_ARG。
esp_err_t app_net_wifi_set_credentials(const char *ssid, const char *password);
bool      app_net_has_credentials(void);
// 已保存的 SSID，无则返回空串。
const char *app_net_saved_ssid(void);

// 只打开 Wi-Fi 射频、不连接：BLE 配网在收到手机下发的凭证之前就需要射频可用
// （扫描、以及凭证到手后立刻发起连接）。幂等，失败回滚到未启动状态。
esp_err_t app_net_wifi_radio_up(void);

// ---------------------------------------------------------------------------
// NTP 校时
// ---------------------------------------------------------------------------
// 阻塞等待一次 SNTP 校时（最长约 8 秒）。成功时通过 app_state_set_time() 写入本地
// 时间并返回 ESP_OK；未联网或超时返回错误。需在 worker task 调用。
esp_err_t app_net_sync_time(void);

// ---------------------------------------------------------------------------
// 赛事数据
// ---------------------------------------------------------------------------
typedef enum {
    APP_FETCH_IDLE = 0,
    APP_FETCH_RUNNING,
    APP_FETCH_OK,
    APP_FETCH_FAILED,
} app_fetch_state_t;

// 异步校时：按需拉起 Wi-Fi 并等待一次 SNTP 校时，全程在内部 worker 中执行，
// 界面线程只读状态。运行中重复调用会被忽略。完成后读 app_net_time_state()。
// 校时结束若既未停留在赛事中心、也没有正在进行的赛事拉取，则释放 Wi-Fi。
void              app_net_time_sync_request(void);
app_fetch_state_t app_net_time_state(void);
// 上次校时失败原因（简短中文），成功或未开始返回 NULL。
const char       *app_net_time_error(void);

// 异步拉取今日与本周赛程并写入 app_state_esports()。运行中重复调用会被忽略。
// 不要求已联网；内部会按需拉起 Wi-Fi。完成后调用 app_net_esports_state() 读取结果。
void              app_net_esports_fetch(void);
app_fetch_state_t app_net_esports_state(void);
// 上次失败原因（简短中文），成功或未开始返回 NULL。
const char       *app_net_esports_error(void);
// 主动结束拉取并释放 Wi-Fi（退出赛事中心时调用）。
void              app_net_esports_stop(void);

// 赛区（league）列表：从 getLeagues 拉取后缓存在内存中，供积分榜切换。
void        app_net_leagues_fetch(void);
int         app_net_league_count(void);
const char *app_net_league_name(int index);   // 中文显示名
const char *app_net_league_slug(int index);   // 接口 slug，如 "lpl"
// 拉取指定赛区积分榜写入缓存。slug 为 NULL 时用主要赛区（优先 LPL）。
void        app_net_standings_fetch(const char *slug);

// ---------------------------------------------------------------------------
// 热点配网（设备本地服务，不需要互联网）
// ---------------------------------------------------------------------------
// 开放临时热点并启动配置网页。成功后可用手机连接热点并打开 app_net_prov_url()。
esp_err_t app_net_prov_start(void);
void      app_net_prov_stop(void);
bool      app_net_prov_active(void);
const char *app_net_prov_ssid(void);
const char *app_net_prov_pass(void);
const char *app_net_prov_url(void);
// 配网网页最近一次成功保存的提示（供页面展示），无则返回 NULL。
const char *app_net_prov_note(void);
