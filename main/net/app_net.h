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
#include "logic/app_channel.h"
#include "logic/app_wifilab.h"

#include <stdbool.h>

// 全局一次性初始化：准备 NVS（幂等）、默认事件循环与 netif。app_main 启动时调用。
// 只准备基础设施，不打开 Wi-Fi 射频。
esp_err_t app_net_init(void);

// 异步请求的通用状态。放在最前面：信道、校时、赛事等多个接口都返回它。
typedef enum {
    APP_FETCH_IDLE = 0,
    APP_FETCH_RUNNING,
    APP_FETCH_OK,
    APP_FETCH_FAILED,
} app_fetch_state_t;

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
// 时间（同时写入 newlib 系统时钟——HTTPS 的证书有效期校验读的是 time(NULL)）并返回
// ESP_OK；未联网或超时返回错误。需在 worker task 调用。
esp_err_t app_net_sync_time(void);

// ---------------------------------------------------------------------------
// Wi-Fi 信道体检
// ---------------------------------------------------------------------------
// 异步扫描 2.4 GHz 频段，把每个信道的 AP 数与信号强度汇总成拥挤度与推荐信道。
// 运行中重复调用会被忽略；完成后用 app_net_channel_state() / app_net_channel_report()
// 读取。不要求已联网：内部会按需拉起 Wi-Fi，扫完若没有别的用途立即释放射频。
//
// 射频互斥：蓝牙（找设备 / 万能遥控）正在占用 2.4G 时本函数直接拒绝——它不启动扫描，
// 而是把结果置为 APP_FETCH_FAILED 并给出 app_net_channel_error() 里的一句人话。两者
// 抢同一路射频，硬开会让扫描结果和蓝牙连接双双不稳；反过来蓝牙角色开启前也会用
// app_net_channel_scan_running() 拒绝。用户侧表现为"告诉你去退出另一个页面"。
void              app_net_channel_scan_request(void);
app_fetch_state_t app_net_channel_scan_state(void);
// 信道体检是否正在占用射频（扫描进行中）。蓝牙角色开启前用它做互斥判断。
bool              app_net_channel_scan_running(void);
// 最近一次扫描成功的报告；尚未成功时返回 NULL。
const app_channel_report_t *app_net_channel_report(void);
// 上次失败原因（简短中文），成功或未开始返回 NULL。
const char       *app_net_channel_error(void);

// ---------------------------------------------------------------------------
// 赛事数据
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Wi-Fi 实验（仅自有 / 授权环境：CTF / 实验室无线测试）
// ---------------------------------------------------------------------------
// 本角色把 logic/app_wifilab 算好的原始 802.11 帧通过 esp_wifi_80211_tx 发射出去。危险来自
// "对谁发"，所以必须由上层界面先把用户挡在授权/法律告知之后（见 ui/ui_wifilab.c），本模块
// 只负责"按要求发射"与严格的状态机。
//
// 射频互斥：与"信道体检"、任意蓝牙角色、热点配网共用一路 2.4G 射频，硬互斥——本角色开启前
// 会拒绝（这些角色都已在跑时），反之信道体检 / 蓝牙角色 / 配网开启前也会用
// app_net_wifilab_running() 拒绝本角色。用户侧表现为"告诉你去退出另一个页面"。
//
// 与蓝牙层一致：start/stop 阻塞式，必须由异步请求 worker 调用，界面只发请求、读状态。
esp_err_t app_net_wifilab_start(app_wifilab_mode_t mode, app_wifilab_beacon_t beacon,
                                const uint8_t target_bssid[6], int target_channel,
                                const char *target_ssid, size_t target_ssid_len,
                                uint32_t seed);
void      app_net_wifilab_stop(void);
bool      app_net_wifilab_running(void);

// 异步请求开启 / 停止，语义与 app_ble_*_request_* 一致（worker 串行处理，界面不阻塞）。
void      app_net_wifilab_request_start(app_wifilab_mode_t mode, app_wifilab_beacon_t beacon,
                                       const uint8_t target_bssid[6], int target_channel,
                                       const char *target_ssid, size_t target_ssid_len,
                                       uint32_t seed);
void      app_net_wifilab_request_stop(void);

// 最近一次开启请求的结果（ESP_OK 表示成功）。界面用它把失败原因显示给用户。
esp_err_t app_net_wifilab_last_error(void);
// 失败时给普通用户看的一句话原因，成功或尚未请求返回 NULL。
const char *app_net_wifilab_error_text(void);

// 当前模式与累计发射统计，供界面状态行展示。sent 只统计驱动接受（esp_wifi_80211_tx
// 返回 ESP_OK）的帧，failed 统计被驱动拒绝的帧——分开显示才能区分"在发"与"发不出去"。
app_wifilab_mode_t app_net_wifilab_mode(void);
uint32_t  app_net_wifilab_packets_sent(void);
uint32_t  app_net_wifilab_packets_failed(void);
// 首个发射错误的一句话（如 "ESP_ERR_WIFI_IF"），尚未出现发射错误时返回 NULL。
const char *app_net_wifilab_tx_error_text(void);

// 异步拉取单场对局详情（阵容 / 经济 / 选手）写入 app_state_esports()->detail。
// 需要 match_id（赛程里的比赛 id）。运行中重复调用会被忽略；若赛程拉取正在跑，
// 会等它结束后再开始，避免同时占用 Wi-Fi 与内存。结果可能只有阵容——接口对
// 已结束多时的比赛不再返回采样帧。
void              app_net_esport_detail_fetch(const char *match_id);
app_fetch_state_t app_net_esport_detail_state(void);
// 上次失败原因（简短中文），成功或未开始返回 NULL。
const char       *app_net_esport_detail_error(void);

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

// 手动清理内存后再重试热点配网的辅助（"内存不足"时给用户一个按键出口）。
// 只释放可再生的缓存与后台请求（赛区缓存、正在跑的扫描、蓝牙角色），不触碰用户数据
// 与正在使用的射频，也不阻塞（蓝牙停止只是异步入队）。返回清理前后空闲堆增量，供
// 界面显示；即使增量为 0 也值得重试——碎片化场景下"释放后重分配"本身就可能成功。
size_t app_net_prov_reclaim_memory(void);
