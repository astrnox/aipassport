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

#include "app_state.h"
#include "logic/app_esports.h"
#include "logic/app_time.h"
#include "logic/app_totp.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
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

// 配网
static httpd_handle_t s_httpd;
static bool s_prov_active;
static char s_prov_note[80];

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

// 发 GET 请求并读取响应体。成功时 *out 为 malloc 出来的 NUL 结尾缓冲，调用方负责 free。
static esp_err_t http_get_json(const char *url, char **out, int *out_len)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = NET_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .user_agent = "FoloPassport/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "x-api-key", LOLESPORTS_KEY);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char *buf = (char *)malloc(NET_HTTP_MAX_BODY + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    bool too_big = false;
    while (total < NET_HTTP_MAX_BODY) {
        int r = esp_http_client_read(client, buf + total, NET_HTTP_MAX_BODY - total);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;
        total += r;
    }
    if (total >= NET_HTTP_MAX_BODY) too_big = true;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) { free(buf); return err; }
    if (too_big) { free(buf); return ESP_ERR_INVALID_SIZE; }

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
        if (http_get_json(url, &body, NULL) != ESP_OK) {
            copy_trunc(err, sizeof(err), "赛程请求失败");
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
        if (http_get_json(url, &body, NULL) == ESP_OK) {
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
        if (http_get_json(url, &body, NULL) != ESP_OK) {
            copy_trunc(err, sizeof(err), "详情请求失败");
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
        if (http_get_json(url, &body, NULL) == ESP_OK) {
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
    if (http_get_json(url, &body, NULL) == ESP_OK) {
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
        if (http_get_json(url, &body, NULL) != ESP_OK) goto done;

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
        if (http_get_json(url, &body, NULL) != ESP_OK) goto done;

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
    "<title>FoloToy AI Passport 配网</title>\n"
    "<style>\n"
    "body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:20px;background:#f5f5f0;color:#222;}\n"
    "h1{font-size:20px;margin:0 0 4px;}\n"
    ".hint{color:#666;font-size:13px;margin:0 0 16px;}\n"
    "form{background:#fff;border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.08);}\n"
    "label{display:block;font-size:13px;color:#444;margin:8px 0 4px;}\n"
    "input,textarea{width:100%;box-sizing:border-box;padding:10px;border:1px solid #ccc;border-radius:8px;font-size:15px;}\n"
    "textarea{font-size:14px;font-family:ui-monospace,Menlo,Consolas,monospace;line-height:1.5;}\n"
    "button{margin-top:12px;width:100%;padding:12px;border:0;border-radius:8px;background:#2f6f4f;color:#fff;font-size:15px;}\n"
    "</style></head><body>\n"
    "<h1>FoloToy AI Passport</h1>\n"
    "<p class=\"hint\">连上本热点后填写家里的 Wi-Fi，设备即可校时并同步赛事。配网全程不需要互联网。</p>\n"
    "<form method=\"post\" action=\"/wifi\">\n"
    "<label>Wi-Fi 名称 (SSID)</label><input name=\"ssid\" maxlength=\"32\" required>\n"
    "<label>Wi-Fi 密码</label><input name=\"pass\" type=\"password\" maxlength=\"64\">\n"
    "<button type=\"submit\">保存并连接</button>\n"
    "</form>\n"
    "<form method=\"post\" action=\"/time\" id=\"tf\">\n"
    "<input type=\"hidden\" name=\"t\" id=\"t\">\n"
    "<label>时间校准</label>\n"
    "<button type=\"submit\">用手机时间校准设备</button>\n"
    "</form>\n"
    "<form method=\"post\" action=\"/totp\">\n"
    "<label>动态口令链接（otpauth://... ，可选）</label>\n"
    "<input name=\"uri\" maxlength=\"180\" placeholder=\"otpauth://totp/...\">\n"
    "<button type=\"submit\">添加口令</button>\n"
    "</form>\n"
    "<form method=\"post\" action=\"/routine\">\n"
    "<label>作息表（每行一节：08:00-08:45 第一节，可带 # 注释）</label>\n"
    "<textarea name=\"text\" rows=\"8\" maxlength=\"5000\" "
    "placeholder=\"08:00-08:45 第一节&#10;08:45-08:55 课间&#10;# 用 @单周 / @双周 分别写两套作息&#10;# @周一 起只改某一天，@周三 再换一天\"></textarea>\n"
    "<button type=\"submit\">导入作息表</button>\n"
    "</form>\n"
    "<script>document.getElementById('t').value=Math.floor(Date.now()/1000);</script>\n"
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

static void fill_ap_config(wifi_config_t *ap)
{
    memset(ap, 0, sizeof(*ap));
    size_t n = strlen(PROV_SSID);
    if (n > sizeof(ap->ap.ssid)) n = sizeof(ap->ap.ssid);
    memcpy(ap->ap.ssid, PROV_SSID, n);
    ap->ap.ssid_len = (uint8_t)n;
    strncpy((char *)ap->ap.password, PROV_PASS, sizeof(ap->ap.password) - 1);
    ap->ap.channel = 1;
    ap->ap.max_connection = 4;
    ap->ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap->ap.pmf_cfg.required = false;
}

esp_err_t app_net_prov_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_prov_active) return ESP_OK;
    if (!s_ap_netif) return ESP_ERR_INVALID_STATE;

    esp_err_t err = wifi_ensure_started();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网 Wi-Fi 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    wifi_config_t ap_cfg;
    fill_ap_config(&ap_cfg);
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 7;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGE(TAG, "配网网页启动失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = prov_get_root, .user_ctx = NULL };
    httpd_uri_t u_wifi = { .uri = "/wifi", .method = HTTP_POST, .handler = prov_post_wifi, .user_ctx = NULL };
    httpd_uri_t u_time = { .uri = "/time", .method = HTTP_POST, .handler = prov_post_time, .user_ctx = NULL };
    httpd_uri_t u_totp = { .uri = "/totp", .method = HTTP_POST, .handler = prov_post_totp, .user_ctx = NULL };
    httpd_uri_t u_routine = { .uri = "/routine", .method = HTTP_POST, .handler = prov_post_routine, .user_ctx = NULL };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_wifi);
    httpd_register_uri_handler(s_httpd, &u_time);
    httpd_register_uri_handler(s_httpd, &u_totp);
    httpd_register_uri_handler(s_httpd, &u_routine);

    net_lock();
    s_prov_active = true;
    s_prov_note[0] = '\0';
    net_unlock();

    ESP_LOGI(TAG, "配网已开启: SSID=%s URL=%s", PROV_SSID, PROV_URL);
    return ESP_OK;
}

void app_net_prov_stop(void)
{
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
