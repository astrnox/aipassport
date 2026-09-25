// main/net/app_blufi.c —— BLE 配网（BLUFI）实现。
//
// 手机端用官方 EspBlufi App 连上设备广播的 FoloPassport，下发 2.4 GHz Wi-Fi 凭证；
// 设备保存凭证、发起连接，并把结果回报给手机。
//
// 生命周期与热点配网对齐：只在用户明确选择"蓝牙配网"的这段时间里持有 BLE 协议栈与
// BT 控制器，退出即全部释放。Wi-Fi 射频只"拉起不连接"（凭证还没到），凭证到手后交给
// app_net 的凭证存储与 STA 逻辑，避免两处各管一份 Wi-Fi 状态。
//
// 线程约定：app_ble_prov_start()/stop() 由界面派生出的 worker 调用（协议栈初始化耗时
// 较长）；BLUFI 事件回调运行在 BTC 任务，Wi-Fi 事件回调运行在系统事件任务，两者都只
// 读写本文件的短状态，不做界面操作。
#include "app_blufi.h"

#include "app_net.h"

#include "esp_bt.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <stdlib.h>
#include <string.h>

// 设备广播名。手机端在 BLE 列表里按这个名字找设备。
#define BLE_PROV_NAME "FoloPassport"

static const char *TAG = "app_blufi";

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

static app_ble_prov_state_t s_state = APP_BLE_PROV_OFF;
static char s_error[48];

static volatile bool s_ble_connected;   // 手机是否已连上，决定要不要回报状态
static bool s_bt_up;                    // BT 控制器与 NimBLE 主机是否已就绪
static bool s_events_on;                // Wi-Fi 事件回调是否已注册

// 手机下发的凭证先存这里，等对方发出"请求连接"再一次性交给 app_net，
// 避免只收到 SSID 就急着重连。
static char s_ssid[33];
static char s_pass[65];
static bool s_have_cred;

// 手机主动要求扫描 Wi-Fi 列表时置位，扫描完成的回报只发一次。
static volatile bool s_scan_wanted;

// ---------------------------------------------------------------------------
// BLUFI 安全层：DH 协商共享密钥 + AES-CFB128 加解密 + CRC16 校验
// ---------------------------------------------------------------------------
// 手机端 EspBlufi 默认走"协商密钥"流程，协商失败会直接断开，所以这一层要实现。
// 数据包类型码是 BLUFI 的私有约定，与手机端保持一致。

#define SEC_TYPE_DH_PARAM_LEN  0x00
#define SEC_TYPE_DH_PARAM_DATA 0x01
#define SEC_TYPE_DH_P          0x02
#define SEC_TYPE_DH_G          0x03
#define SEC_TYPE_DH_PUBLIC     0x04

#define SEC_PUB_KEY_MAX 128
#define SEC_PARAM_MAX   1024
#define SEC_SHARE_MAX   128
#define SEC_PSK_LEN     16

typedef struct {
    uint8_t pub[SEC_PUB_KEY_MAX];
    uint8_t share[SEC_SHARE_MAX];
    size_t  share_len;
    uint8_t psk[SEC_PSK_LEN];
    uint8_t *param;
    int      param_len;
    uint8_t  iv[16];
    mbedtls_dhm_context dhm;
    mbedtls_aes_context aes;
} ble_prov_sec_t;

static ble_prov_sec_t *s_sec;

static int sec_rand(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

static void sec_report_error(esp_blufi_error_state_t state)
{
    esp_blufi_send_error_info(state);
}

static void sec_init(void)
{
    if (s_sec) return;
    s_sec = calloc(1, sizeof(*s_sec));
    if (!s_sec) {
        ESP_LOGE(TAG, "安全上下文分配失败");
        return;
    }
    mbedtls_dhm_init(&s_sec->dhm);
    mbedtls_aes_init(&s_sec->aes);
}

static void sec_deinit(void)
{
    if (!s_sec) return;
    free(s_sec->param);
    s_sec->param = NULL;
    mbedtls_dhm_free(&s_sec->dhm);
    mbedtls_aes_free(&s_sec->aes);
    free(s_sec);
    s_sec = NULL;
}

// 处理手机发来的协商数据。四种类型里只有"参数长度"和"参数内容"需要真正处理，
// 另外三种（分别下发 p、g、公钥）在本流程中由参数块一并给出，忽略即可。
static void sec_negotiate(uint8_t *data, int len, uint8_t **output_data, int *output_len,
                         bool *need_free)
{
    if (!s_sec) {
        ESP_LOGE(TAG, "安全上下文未初始化");
        sec_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }
    if (!data || len < 3) {
        ESP_LOGE(TAG, "协商数据格式错误");
        sec_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint8_t type = data[0];
    if (type == SEC_TYPE_DH_PARAM_LEN) {
        int plen = (data[1] << 8) | data[2];
        if (plen <= 0 || plen > SEC_PARAM_MAX) {
            ESP_LOGE(TAG, "DH 参数长度非法: %d", plen);
            sec_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        free(s_sec->param);
        s_sec->param = malloc((size_t)plen);
        if (!s_sec->param) {
            s_sec->param_len = 0;
            sec_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            return;
        }
        s_sec->param_len = plen;
        return;
    }

    if (type != SEC_TYPE_DH_PARAM_DATA) return;

    if (!s_sec->param || len < s_sec->param_len + 1) {
        ESP_LOGE(TAG, "DH 参数不完整");
        sec_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }

    memcpy(s_sec->param, &data[1], (size_t)s_sec->param_len);
    uint8_t *cursor = s_sec->param;
    int ret = mbedtls_dhm_read_params(&s_sec->dhm, &cursor, cursor + s_sec->param_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "读取 DH 参数失败: -0x%04x", -ret);
        sec_report_error(ESP_BLUFI_READ_PARAM_ERROR);
        return;
    }
    free(s_sec->param);
    s_sec->param = NULL;
    s_sec->param_len = 0;

    const int dhm_len = mbedtls_dhm_get_len(&s_sec->dhm);
    if (dhm_len <= 0 || dhm_len > SEC_PUB_KEY_MAX) {
        ESP_LOGE(TAG, "DH 长度不支持: %d", dhm_len);
        sec_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }

    ret = mbedtls_dhm_make_public(&s_sec->dhm, dhm_len, s_sec->pub, SEC_PUB_KEY_MAX,
                                  sec_rand, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "生成公钥失败: -0x%04x", -ret);
        sec_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
        return;
    }

    ret = mbedtls_dhm_calc_secret(&s_sec->dhm, s_sec->share, SEC_SHARE_MAX,
                                  &s_sec->share_len, sec_rand, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "计算共享密钥失败: -0x%04x", -ret);
        sec_report_error(ESP_BLUFI_DH_PARAM_ERROR);
        return;
    }

    ret = mbedtls_md5(s_sec->share, s_sec->share_len, s_sec->psk);
    if (ret != 0) {
        ESP_LOGE(TAG, "派生会话密钥失败: -0x%04x", -ret);
        sec_report_error(ESP_BLUFI_CALC_MD5_ERROR);
        return;
    }
    mbedtls_aes_setkey_enc(&s_sec->aes, s_sec->psk, SEC_PSK_LEN * 8);

    // 把自己的公钥原样回给手机，由对方算出同一份共享密钥。
    *output_data = s_sec->pub;
    *output_len = dhm_len;
    *need_free = false;
}

static int sec_encrypt(uint8_t iv8, uint8_t *data, int len)
{
    if (!s_sec) return -1;
    size_t offset = 0;
    uint8_t iv[16];
    memcpy(iv, s_sec->iv, sizeof(iv));
    iv[0] = iv8;   // BLUFI 用包序号充当 IV 首字节
    if (mbedtls_aes_crypt_cfb128(&s_sec->aes, MBEDTLS_AES_ENCRYPT, (size_t)len, &offset,
                                 iv, data, data) != 0) {
        return -1;
    }
    return len;
}

static int sec_decrypt(uint8_t iv8, uint8_t *data, int len)
{
    if (!s_sec) return -1;
    size_t offset = 0;
    uint8_t iv[16];
    memcpy(iv, s_sec->iv, sizeof(iv));
    iv[0] = iv8;
    if (mbedtls_aes_crypt_cfb128(&s_sec->aes, MBEDTLS_AES_DECRYPT, (size_t)len, &offset,
                                 iv, data, data) != 0) {
        return -1;
    }
    return len;
}

static uint16_t sec_checksum(uint8_t iv8, uint8_t *data, int len)
{
    (void)iv8;   // 校验与包序号无关
    return esp_crc16_be(0, data, len);
}

// ---------------------------------------------------------------------------
// 状态回报
// ---------------------------------------------------------------------------

static void report_sta_state(esp_blufi_sta_conn_state_t state)
{
    if (!s_ble_connected) return;   // 手机不在线，没人接收

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);

    esp_blufi_extra_info_t info;
    memset(&info, 0, sizeof(info));
    if (s_ssid[0]) {
        info.sta_ssid = (uint8_t *)s_ssid;
        info.sta_ssid_len = (int)strlen(s_ssid);
    }
    // BLE 配网期间设备不开热点，连接数恒为 0。
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

// ---------------------------------------------------------------------------
// Wi-Fi 事件：把连接结果回报给手机
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == APP_BLE_PROV_APPLYING) {
            s_state = APP_BLE_PROV_FAILED;
            snprintf(s_error, sizeof(s_error), "Wi-Fi 连接失败，请核对密码后重试");
            ESP_LOGW(TAG, "配网连接失败");
        }
        report_sta_state(ESP_BLUFI_STA_CONN_FAIL);
        return;
    }

    if (id != WIFI_EVENT_SCAN_DONE) return;
    if (!s_scan_wanted) return;
    s_scan_wanted = false;

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    if (ap_count > 10) ap_count = 10;   // 手机端列表也用不了更多

    wifi_ap_record_t *aps = calloc(ap_count, sizeof(*aps));
    esp_blufi_ap_record_t *out = calloc(ap_count, sizeof(*out));
    if (!aps || !out) {
        free(aps);
        free(out);
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }

    uint16_t found = ap_count;
    if (esp_wifi_scan_get_ap_records(&found, aps) == ESP_OK && s_ble_connected) {
        for (uint16_t i = 0; i < found; i++) {
            memcpy(out[i].ssid, aps[i].ssid, sizeof(out[i].ssid));
            out[i].rssi = aps[i].rssi;
        }
        esp_blufi_send_wifi_list(found, out);
    }
    free(aps);
    free(out);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;

    if (s_state == APP_BLE_PROV_APPLYING || s_state == APP_BLE_PROV_FAILED) {
        s_state = APP_BLE_PROV_DONE;
        s_error[0] = '\0';
        ESP_LOGI(TAG, "配网成功: %s", s_ssid);
    }
    report_sta_state(ESP_BLUFI_STA_CONN_SUCCESS);
}

// ---------------------------------------------------------------------------
// BLUFI 事件
// ---------------------------------------------------------------------------

static void blufi_event_cb(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        // 广播名用产品名而不是 BLUFI 默认名，手机端按名字就能认出设备。
        esp_blufi_adv_start_with_name(BLE_PROV_NAME);
        s_state = APP_BLE_PROV_ADVERTISING;
        ESP_LOGI(TAG, "BLE 配网已广播: %s", BLE_PROV_NAME);
        break;

    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();   // 单连接：连上就停止广播
        sec_init();
        s_state = APP_BLE_PROV_CONNECTED;
        ESP_LOGI(TAG, "手机已连接");
        break;

    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        sec_deinit();
        s_state = APP_BLE_PROV_ADVERTISING;
        report_sta_state(s_state == APP_BLE_PROV_DONE ? ESP_BLUFI_STA_CONN_SUCCESS
                                                      : ESP_BLUFI_STA_CONN_FAIL);
        esp_blufi_adv_start();   // 允许换一台手机重试
        ESP_LOGI(TAG, "手机已断开，继续广播");
        break;

    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        // 设备侧只用 STA；手机若要求 AP/APSTA，一律收敛成 STA。
        esp_wifi_set_mode(WIFI_MODE_STA);
        break;

    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        if (!s_have_cred) {
            snprintf(s_error, sizeof(s_error), "没收到完整 Wi-Fi 信息");
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        s_state = APP_BLE_PROV_APPLYING;
        s_error[0] = '\0';
        ESP_LOGI(TAG, "开始连接 Wi-Fi: %s", s_ssid);
        // app_net 负责落盘与连接；失败时它只返回错误码，界面靠状态与事件判断结果。
        app_net_wifi_set_credentials(s_ssid, s_pass);
        break;

    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        if (s_state == APP_BLE_PROV_APPLYING) s_state = APP_BLE_PROV_CONNECTED;
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (s_state == APP_BLE_PROV_DONE) {
            report_sta_state(ESP_BLUFI_STA_CONN_SUCCESS);
        } else if (s_state == APP_BLE_PROV_APPLYING) {
            report_sta_state(ESP_BLUFI_STA_CONNECTING);
        } else {
            report_sta_state(ESP_BLUFI_STA_CONN_FAIL);
        }
        break;

    case ESP_BLUFI_EVENT_RECV_STA_SSID: {
        int len = param->sta_ssid.ssid_len;
        if (len <= 0 || len >= (int)sizeof(s_ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_ssid, param->sta_ssid.ssid, (size_t)len);
        s_ssid[len] = '\0';
        s_have_cred = true;
        if (s_state == APP_BLE_PROV_FAILED) {
            s_state = APP_BLE_PROV_CONNECTED;
            s_error[0] = '\0';
        }
        break;
    }

    case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
        int len = param->sta_passwd.passwd_len;
        if (len < 0 || len >= (int)sizeof(s_pass)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_pass, param->sta_passwd.passwd, (size_t)len);
        s_pass[len] = '\0';
        break;
    }

    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        // 不按 BSSID 绑定：同一 SSID 的多个热点由设备自行择优。
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        s_scan_wanted = true;
        {
            wifi_scan_config_t scan = { 0 };
            if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
                s_scan_wanted = false;
                esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
            }
        }
        break;

    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGW(TAG, "手机报告错误: %d", (int)param->report_error.state);
        break;

    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;

    default:
        // 热点类事件不处理：设备侧配网热点由"热点网页配网"那条路径负责。
        break;
    }
}

static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = blufi_event_cb,
    .negotiate_data_handler = sec_negotiate,
    .encrypt_func = sec_encrypt,
    .decrypt_func = sec_decrypt,
    .checksum_func = sec_checksum,
};

// ---------------------------------------------------------------------------
// NimBLE 主机
// ---------------------------------------------------------------------------

void ble_store_config_init(void);

static void nimble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位, reason=%d", reason);
}

static void nimble_on_sync(void)
{
    // 主机就绪才能初始化 BLUFI profile，初始化完成会回调 INIT_FINISH 去开广播。
    esp_blufi_profile_init();
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();   // 直到 nimble_port_stop() 才返回
    nimble_port_freertos_deinit();
}

static esp_err_t host_init(void)
{
    esp_err_t err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = nimble_on_reset;
    ble_hs_cfg.sync_cb = nimble_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = 4;

    int rc = esp_blufi_gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "BLUFI GATT 服务初始化失败: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_PROV_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置设备名失败: %d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    esp_blufi_btc_init();

    err = esp_nimble_enable(nimble_host_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 NimBLE 主机任务失败: %s", esp_err_to_name(err));
        esp_blufi_btc_deinit();
        esp_blufi_gatt_svr_deinit();
        return err;
    }
    return ESP_OK;
}

static void host_deinit(void)
{
    esp_blufi_gatt_svr_deinit();
    if (nimble_port_stop() != 0) {
        ESP_LOGW(TAG, "停止 NimBLE 主机失败");
    }
    esp_nimble_deinit();
    esp_blufi_profile_deinit();
    esp_blufi_btc_deinit();
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

static void reset_transient(void)
{
    s_ble_connected = false;
    s_scan_wanted = false;
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_have_cred = false;
    s_error[0] = '\0';
}

esp_err_t app_ble_prov_start(void)
{
    if (s_state == APP_BLE_PROV_ADVERTISING || s_state == APP_BLE_PROV_CONNECTED ||
        s_state == APP_BLE_PROV_APPLYING || s_state == APP_BLE_PROV_DONE) {
        return ESP_OK;
    }

    reset_transient();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器启用失败: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        goto fail;
    }

    // BLUFI 需要在收到凭证的当下就能连 Wi-Fi，所以先把射频拉起来（不连接）。
    err = app_net_wifi_radio_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 射频启动失败: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        goto fail;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    s_events_on = true;

    err = esp_blufi_register_callbacks(&s_callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 BLUFI 回调失败: %s", esp_err_to_name(err));
        goto fail;
    }

    err = host_init();
    if (err != ESP_OK) goto fail;

    s_bt_up = true;
    s_state = APP_BLE_PROV_ADVERTISING;   // 广播在 INIT_FINISH 里真正开始
    return ESP_OK;

fail:
    if (s_events_on) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event);
        s_events_on = false;
    }
    s_state = APP_BLE_PROV_FAILED;
    snprintf(s_error, sizeof(s_error), "蓝牙配网开启失败");
    return err == ESP_OK ? ESP_FAIL : err;
}

void app_ble_prov_stop(void)
{
    if (s_bt_up) {
        esp_blufi_adv_stop();
        host_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_bt_up = false;
    }

    if (s_events_on) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event);
        s_events_on = false;
    }

    sec_deinit();
    reset_transient();
    s_state = APP_BLE_PROV_OFF;
}

bool app_ble_prov_active(void)
{
    return s_state != APP_BLE_PROV_OFF;
}

app_ble_prov_state_t app_ble_prov_state(void)
{
    return s_state;
}

const char *app_ble_prov_name(void)
{
    return BLE_PROV_NAME;
}

const char *app_ble_prov_error(void)
{
    return s_error[0] ? s_error : NULL;
}
