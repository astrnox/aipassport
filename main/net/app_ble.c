// main/net/app_ble.c —— "找设备"（BLE 观察者扫描）与"万能遥控"（BLE HID 外设）的实现。
//
// 两条功能共用同一份 BLE 协议栈，但同一时刻只有一个在跑：s_mode 标明当前角色，
// 进入另一功能前必须先停掉当前角色。协议栈的生命周期与配网一致——需要时才拉起，
// 离开页面立即拆掉（见 app_ble.h 的说明）。
//
// HID 服务是手写的，没有用 esp_hid 组件：esp_hid 自带一套 GAP/安全初始化，会和
// app_blufi 已有的 NimBLE 初始化重复，而这里只需要"一个 HID 服务 + 两条通知"。
// 手写反而更清楚，也便于把"哪个模式发哪条报文"完全交给 logic/app_remote 决定。
// 报文用 ble_gatts_notify_custom 直接发出，与 ESP-IDF 官方 esp_hid 在 NimBLE 上的
// 做法一致（components/esp_hid/src/nimble_hidd.c）。
//
// 线程约定：start/stop 由页面派生的 worker 调用；NimBLE 回调运行在主机任务，只更新
// 内部状态；界面通过 app_ble_finder_snapshot() 取一份拷贝，不直接读内部表。
#include "app_ble.h"

#include "app_blufi.h"
#include "app_net.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nvs.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_ble";

// 主机端在蓝牙列表里看到的名字。ASCII，避免个别主机对非 ASCII 广播名显示成乱码。
#define BLE_REMOTE_NAME "Passport Remote"

// 成对报文之间的间隔：先发"按下"、再发"松开"，主机才认成一次敲击。15ms 足够短，
// 不给用户"按了没反应"的感觉，又不会被主机合并成长按。
#define BLE_REMOTE_TAP_MS 15

// 收藏设备的 NVS 命名空间。
#define BLE_NVS_NS  "finder"
#define BLE_NVS_KEY "fav"

void ble_store_config_init(void);   // NimBLE port 提供的弱符号（NVS 落盘配对信息）

typedef enum {
    BLE_MODE_IDLE = 0,
    BLE_MODE_FINDER,
    BLE_MODE_REMOTE,
} ble_mode_t;

// ---------------------------------------------------------------------------
// 模块状态
// ---------------------------------------------------------------------------

static ble_mode_t s_mode = BLE_MODE_IDLE;
static bool s_stack_up;              // BT 控制器 + NimBLE 主机是否已拉起
static uint8_t s_own_addr_type;

static SemaphoreHandle_t s_lock;     // 保护 s_finder（扫描回调 vs 界面快照）
static bool s_lock_ready;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static app_finder_t s_finder;
static bool s_saved_loaded;

// 万能遥控：连接状态与发送队列。
static volatile bool s_connected;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_h_kb;              // 键盘报文特征值句柄（注册 GATT 服务时回填）
static uint16_t s_h_cons;            // 消费类报文特征值句柄
static uint8_t s_kb_val[APP_REMOTE_KB_LEN];
static uint8_t s_cons_val[APP_REMOTE_CONS_LEN];
static QueueHandle_t s_txq;
static TaskHandle_t s_tx_task;

// 异步请求队列：界面线程只把"开/关"放进队列即返回，真正的协议栈拉起/拆除由常驻
// worker 串行完成。串行很关键——快速进出页面若让 start/stop 并发，协议栈会当场崩。
typedef enum {
    BLE_REQ_FINDER_START = 0,
    BLE_REQ_FINDER_STOP,
    BLE_REQ_REMOTE_START,
    BLE_REQ_REMOTE_STOP,
} ble_req_t;

static QueueHandle_t s_req_q;
static TaskHandle_t s_req_task;
static volatile esp_err_t s_finder_err = ESP_OK;
static volatile esp_err_t s_remote_err = ESP_OK;

// 失败时给普通用户看的一句话原因。只在 worker（唯一写入者）里改；界面在 last_error()
// 非 ESP_OK 时读，与 s_finder_err/s_remote_err 用同样的"单写者 + 无锁读"约定。
static char s_finder_reason[72];
static char s_remote_reason[72];

static void set_reason(char *dst, size_t cap, const char *text)
{
    snprintf(dst, cap, "%s", text);
}

// ---------------------------------------------------------------------------
// 初始化（互斥锁与设备表只建一次）
// ---------------------------------------------------------------------------

static void ensure_init(void)
{
    portENTER_CRITICAL(&s_init_mux);
    if (!s_lock_ready) {
        s_lock = xSemaphoreCreateMutex();
        app_finder_init(&s_finder);
        s_lock_ready = (s_lock != NULL);
    }
    portEXIT_CRITICAL(&s_init_mux);
}

static inline void ble_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static inline void ble_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------------
// 收藏持久化
// ---------------------------------------------------------------------------

static void load_saved_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = 0;
    if (nvs_get_blob(h, BLE_NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len <= 1024) {
        uint8_t *buf = malloc(len);
        if (buf) {
            if (nvs_get_blob(h, BLE_NVS_KEY, buf, &len) == ESP_OK) {
                app_finder_saved_deserialize(&s_finder, buf, len);
            }
            free(buf);
        }
    }
    nvs_close(h);
}

static void save_saved_locked(void)
{
    uint8_t buf[512];
    size_t len = app_finder_saved_serialize(&s_finder, buf, sizeof(buf));
    if (len == 0) return;

    nvs_handle_t h;
    if (nvs_open(BLE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, BLE_NVS_KEY, buf, len) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// HID 报文映射表（键盘 + 消费类控制）
// ---------------------------------------------------------------------------
// 与 app_remote.h 的约定一致：报告 ID 1 = 键盘（8 字节：修饰键 + 保留 + 6 键码），
// 报告 ID 2 = 消费类控制（2 字节，小端 16 位用途码）。两份 report map 合成一份，
// 主机把它识别成"带媒体键的键盘"。
static const uint8_t HID_REPORT_MAP[] = {
    // ---- 键盘 ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute) —— 8 个修饰键
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant) —— 保留字节
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array) —— 6 个键码
    0xC0,              // End Collection
    // ---- 消费类控制（媒体键） ----
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x03FF)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x03FF)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection
};

enum {
    ARG_HID_INFO = 0,
    ARG_REPORT_MAP,
    ARG_HID_CTRL,
    ARG_PROTO_MODE,
    ARG_REPORT_KB,
    ARG_REPORT_CONS,
    ARG_DSC_KB,
    ARG_DSC_CONS,
};

static const ble_uuid16_t UUID_HID_SVC     = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t UUID_HID_INFO    = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t UUID_REPORT_MAP  = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t UUID_HID_CTRL    = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t UUID_REPORT      = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t UUID_PROTO_MODE  = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t UUID_REPORT_REF  = BLE_UUID16_INIT(0x2908);

static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x02 };   // bcdHID 1.11 / 无国家码 / 可连接
// 报告参考描述符：[报告 ID, 报告类型(1=Input)]。
static const uint8_t DSC_KB[]   = { APP_REMOTE_RID_KEYBOARD, 0x01 };
static const uint8_t DSC_CONS[] = { APP_REMOTE_RID_CONSUMER, 0x01 };

static int hid_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    intptr_t tag = (intptr_t)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        switch (tag) {
        case ARG_HID_INFO:
            return os_mbuf_append(ctxt->om, HID_INFO, sizeof(HID_INFO)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_REPORT_MAP:
            return os_mbuf_append(ctxt->om, HID_REPORT_MAP, sizeof(HID_REPORT_MAP)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_REPORT_KB:
            return os_mbuf_append(ctxt->om, s_kb_val, sizeof(s_kb_val)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_REPORT_CONS:
            return os_mbuf_append(ctxt->om, s_cons_val, sizeof(s_cons_val)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        case ARG_PROTO_MODE: {
            const uint8_t mode = 0x01;   // Report Protocol
            return os_mbuf_append(ctxt->om, &mode, 1) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }

    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (tag == ARG_DSC_KB) {
            return os_mbuf_append(ctxt->om, DSC_KB, sizeof(DSC_KB)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (tag == ARG_DSC_CONS) {
            return os_mbuf_append(ctxt->om, DSC_CONS, sizeof(DSC_CONS)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        // HID 控制点（挂起 / 退出挂起）与协议模式写入都直接接受：本机只按报告模式
        // 发送，不需要对挂起做特别处理，主机侧也不会因此收不到按键。
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static struct ble_gatt_dsc_def s_dsc_kb[] = {
    { .uuid = (const ble_uuid_t *)&UUID_REPORT_REF, .att_flags = BLE_ATT_F_READ,
      .access_cb = hid_access, .arg = (void *)ARG_DSC_KB },
    { 0 }
};

static struct ble_gatt_dsc_def s_dsc_cons[] = {
    { .uuid = (const ble_uuid_t *)&UUID_REPORT_REF, .att_flags = BLE_ATT_F_READ,
      .access_cb = hid_access, .arg = (void *)ARG_DSC_CONS },
    { 0 }
};

static const struct ble_gatt_chr_def s_chrs[] = {
    { .uuid = (const ble_uuid_t *)&UUID_HID_INFO,   .access_cb = hid_access,
      .arg = (void *)ARG_HID_INFO,   .flags = BLE_GATT_CHR_F_READ },
    { .uuid = (const ble_uuid_t *)&UUID_REPORT_MAP, .access_cb = hid_access,
      .arg = (void *)ARG_REPORT_MAP, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
    { .uuid = (const ble_uuid_t *)&UUID_HID_CTRL,   .access_cb = hid_access,
      .arg = (void *)ARG_HID_CTRL,   .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = (const ble_uuid_t *)&UUID_REPORT,     .access_cb = hid_access,
      .arg = (void *)ARG_REPORT_KB,  .descriptors = s_dsc_kb,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
      .val_handle = &s_h_kb },
    { .uuid = (const ble_uuid_t *)&UUID_REPORT,     .access_cb = hid_access,
      .arg = (void *)ARG_REPORT_CONS, .descriptors = s_dsc_cons,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
      .val_handle = &s_h_cons },
    { .uuid = (const ble_uuid_t *)&UUID_PROTO_MODE, .access_cb = hid_access,
      .arg = (void *)ARG_PROTO_MODE,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { 0 }
};

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = (const ble_uuid_t *)&UUID_HID_SVC,
      .characteristics = s_chrs },
    { 0 }
};

// ---------------------------------------------------------------------------
// 万能遥控：发送
// ---------------------------------------------------------------------------

static void send_report(const app_remote_report_t *r)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    uint16_t handle = 0;
    if (r->map_index == APP_REMOTE_MAP_KEYBOARD) {
        handle = s_h_kb;
        memcpy(s_kb_val, r->data, sizeof(s_kb_val));
    } else if (r->map_index == APP_REMOTE_MAP_CONSUMER) {
        handle = s_h_cons;
        memcpy(s_cons_val, r->data, sizeof(s_cons_val));
    }
    if (handle == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(r->data, r->length);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) ESP_LOGD(TAG, "HID 通知失败 rc=%d", rc);
}

typedef struct {
    app_remote_report_t down;
    app_remote_report_t up;
} remote_job_t;

static void remote_tx_task(void *arg)
{
    (void)arg;
    remote_job_t job;
    while (xQueueReceive(s_txq, &job, portMAX_DELAY) == pdTRUE) {
        send_report(&job.down);
        vTaskDelay(pdMS_TO_TICKS(BLE_REMOTE_TAP_MS));
        send_report(&job.up);
    }
}

// ---------------------------------------------------------------------------
// GAP 回调
// ---------------------------------------------------------------------------

static int remote_gap_event(struct ble_gap_event *event, void *arg);

static void start_adv(void)
{
    struct ble_gap_adv_params adv = { 0 };
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv,
                               remote_gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "开始广播失败 rc=%d", rc);
}

static int remote_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "主机已连接，可以开始遥控");
        } else {
            // 连接没建立成功（主机主动取消等）：继续广播等下一次。
            start_adv();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "主机已断开(%d)，重新等待连接", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_adv();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_adv();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "配对/加密状态变化 status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 主机想用旧配对信息重新配对：删掉旧记录，允许它重新配对。
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

    default:
        return 0;
    }
}

static int finder_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
        return 0;
    }

    const char *name = NULL;
    int name_len = 0;
    if (fields.name && fields.name_len > 0) {
        name = (const char *)fields.name;
        name_len = fields.name_len;
    }

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    ble_lock();
    app_finder_feed(&s_finder, event->disc.addr.val, name, name_len,
                    event->disc.rssi, now_ms);
    ble_unlock();
    return 0;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位, reason=%d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();          // 直到 nimble_port_stop() 才返回
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "没有可用的蓝牙地址");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "推断蓝牙地址类型失败");
        return;
    }

    if (s_mode == BLE_MODE_FINDER) {
        struct ble_gap_disc_params dp = { 0 };
        dp.itvl = 0;                 // 0 = 控制器默认（约 100ms）
        dp.window = 0;
        dp.filter_duplicates = 0;    // 不去重：需要同一设备的后续广播来刷新信号强度
        dp.passive = 0;              // 主动扫描，才能收到带名字的扫描响应
        dp.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
        dp.limited = 0;
        int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, finder_gap_event, NULL);
        if (rc != 0) ESP_LOGE(TAG, "开始扫描失败 rc=%d", rc);
        else ESP_LOGI(TAG, "正在扫描附近蓝牙设备");
    } else if (s_mode == BLE_MODE_REMOTE) {
        struct ble_hs_adv_fields f = { 0 };
        f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        f.uuids16 = (const ble_uuid16_t *)&UUID_HID_SVC;
        f.num_uuids16 = 1;
        f.uuids16_is_complete = 1;
        f.appearance = 0x03C1;       // HID Keyboard
        f.appearance_is_present = 1;
        f.name = (const uint8_t *)BLE_REMOTE_NAME;
        f.name_len = strlen(BLE_REMOTE_NAME);
        f.name_is_complete = 1;

        int rc = ble_gap_adv_set_fields(&f);
        if (rc != 0) {
            ESP_LOGE(TAG, "设置广播内容失败 rc=%d", rc);
            return;
        }
        start_adv();
    }
}

// ---------------------------------------------------------------------------
// 协议栈生命周期
// ---------------------------------------------------------------------------

static esp_err_t stack_up(void)
{
    if (s_stack_up) return ESP_OK;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器启用失败: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // HID 外设：Just Works 配对 + 绑定落盘。没有键盘/屏幕输配对码，只能用 Just Works；
    // 绑定信息写进 NVS 后，主机下次开机才能自动认出本机。
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_REMOTE_NAME);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "统计 GATT 服务失败 rc=%d", rc);
        goto fail;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败 rc=%d", rc);
        goto fail;
    }

    ble_store_config_init();

    err = esp_nimble_enable(host_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 NimBLE 主机任务失败: %s", esp_err_to_name(err));
        goto fail;
    }

    s_stack_up = true;
    return ESP_OK;

fail:
    esp_nimble_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    return ESP_FAIL;
}

static void stack_down(void)
{
    if (!s_stack_up) return;
    if (nimble_port_stop() != 0) ESP_LOGW(TAG, "停止 NimBLE 主机失败");
    esp_nimble_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_stack_up = false;
}

// ---------------------------------------------------------------------------
// 找设备
// ---------------------------------------------------------------------------

esp_err_t app_ble_finder_start(void)
{
    ensure_init();
    s_finder_reason[0] = '\0';

    if (s_mode == BLE_MODE_FINDER) return ESP_OK;
    if (s_mode != BLE_MODE_IDLE) {
        set_reason(s_finder_reason, sizeof(s_finder_reason),
                   "蓝牙正被另一功能占用，请先退出那个页面");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_ble_prov_active()) {
        ESP_LOGW(TAG, "蓝牙配网进行中，暂不能开启找设备");
        set_reason(s_finder_reason, sizeof(s_finder_reason),
                   "蓝牙配网正在使用，请先关闭配网");
        return ESP_ERR_INVALID_STATE;
    }
    // 射频互斥：信道体检正在扫 Wi-Fi。它只持续几秒，直接拒绝比硬开更稳。
    if (app_net_channel_scan_running()) {
        ESP_LOGW(TAG, "信道体检正在扫描，暂不能开启找设备");
        set_reason(s_finder_reason, sizeof(s_finder_reason),
                   "信道体检正在扫描 Wi-Fi，请等几秒后重试");
        return ESP_ERR_INVALID_STATE;
    }

    ble_lock();
    if (!s_saved_loaded) {
        load_saved_locked();
        s_saved_loaded = true;
    }
    app_finder_clear_devices(&s_finder);
    ble_unlock();

    s_mode = BLE_MODE_FINDER;
    esp_err_t err = stack_up();
    if (err != ESP_OK) {
        s_mode = BLE_MODE_IDLE;
        set_reason(s_finder_reason, sizeof(s_finder_reason),
                   "蓝牙协议栈启动失败，请长按↑ 重试");
        return err;
    }
    return ESP_OK;
}

void app_ble_finder_stop(void)
{
    if (s_mode == BLE_MODE_FINDER) {
        if (s_stack_up) ble_gap_disc_cancel();
        stack_down();
        s_mode = BLE_MODE_IDLE;
    }
}

bool app_ble_finder_running(void)
{
    return s_mode == BLE_MODE_FINDER;
}

void app_ble_finder_snapshot(app_finder_t *out)
{
    if (!out) return;
    ensure_init();

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    ble_lock();
    app_finder_prune(&s_finder, now_ms);
    app_finder_sort(&s_finder);
    *out = s_finder;
    ble_unlock();
}

int app_ble_finder_save(const uint8_t addr[6], const char *name)
{
    ensure_init();
    if (!addr) return -1;

    ble_lock();
    int idx = app_finder_save(&s_finder, addr, name);
    if (idx >= 0) save_saved_locked();
    ble_unlock();
    return idx;
}

bool app_ble_finder_unsave(const uint8_t addr[6])
{
    ensure_init();
    if (!addr) return false;

    ble_lock();
    bool removed = app_finder_unsave(&s_finder, addr);
    if (removed) save_saved_locked();
    ble_unlock();
    return removed;
}

// ---------------------------------------------------------------------------
// 万能遥控
// ---------------------------------------------------------------------------

static void remote_tx_cleanup(void)
{
    if (s_tx_task) {
        vTaskDelete(s_tx_task);
        s_tx_task = NULL;
    }
    if (s_txq) {
        vQueueDelete(s_txq);
        s_txq = NULL;
    }
}

esp_err_t app_ble_remote_start(void)
{
    ensure_init();
    s_remote_reason[0] = '\0';

    if (s_mode == BLE_MODE_REMOTE) return ESP_OK;
    if (s_mode != BLE_MODE_IDLE) {
        set_reason(s_remote_reason, sizeof(s_remote_reason),
                   "蓝牙正被另一功能占用，请先退出那个页面");
        return ESP_ERR_INVALID_STATE;
    }
    if (app_ble_prov_active()) {
        ESP_LOGW(TAG, "蓝牙配网进行中，暂不能开启遥控");
        set_reason(s_remote_reason, sizeof(s_remote_reason),
                   "蓝牙配网正在使用，请先关闭配网");
        return ESP_ERR_INVALID_STATE;
    }
    // 射频互斥：信道体检正在扫 Wi-Fi。它只持续几秒，直接拒绝比硬开更稳。
    if (app_net_channel_scan_running()) {
        ESP_LOGW(TAG, "信道体检正在扫描，暂不能开启遥控");
        set_reason(s_remote_reason, sizeof(s_remote_reason),
                   "信道体检正在扫描 Wi-Fi，请等几秒后重试");
        return ESP_ERR_INVALID_STATE;
    }

    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    memset(s_kb_val, 0, sizeof(s_kb_val));
    memset(s_cons_val, 0, sizeof(s_cons_val));

    // 发送队列与任务先建好：主机一连上、用户一按键就能立刻发出报文。
    s_txq = xQueueCreate(8, sizeof(remote_job_t));
    if (!s_txq) {
        set_reason(s_remote_reason, sizeof(s_remote_reason), "内存不足，请稍后重试");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(remote_tx_task, "ble_hid_tx", 3072, NULL, 5, &s_tx_task) != pdPASS) {
        remote_tx_cleanup();
        set_reason(s_remote_reason, sizeof(s_remote_reason), "内存不足，请稍后重试");
        return ESP_ERR_NO_MEM;
    }

    s_mode = BLE_MODE_REMOTE;
    esp_err_t err = stack_up();
    if (err != ESP_OK) {
        remote_tx_cleanup();
        s_mode = BLE_MODE_IDLE;
        set_reason(s_remote_reason, sizeof(s_remote_reason),
                   "蓝牙协议栈启动失败，请长按↑ 重试");
        return err;
    }
    ESP_LOGI(TAG, "万能遥控已就绪，等待主机连接");
    return ESP_OK;
}

void app_ble_remote_stop(void)
{
    if (s_mode != BLE_MODE_REMOTE) return;

    if (s_stack_up) ble_gap_adv_stop();
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    remote_tx_cleanup();
    stack_down();
    s_mode = BLE_MODE_IDLE;
}

bool app_ble_remote_running(void)
{
    return s_mode == BLE_MODE_REMOTE;
}

bool app_ble_remote_connected(void)
{
    return s_connected;
}

const char *app_ble_remote_name(void)
{
    return BLE_REMOTE_NAME;
}

bool app_ble_remote_press(app_remote_mode_t mode, app_remote_btn_t btn, app_remote_press_t press)
{
    if (s_mode != BLE_MODE_REMOTE) return false;
    if (!s_connected || !s_txq) return false;

    remote_job_t job;
    if (!app_remote_press(mode, btn, press, &job.down, &job.up)) return false;

    // 队列满说明用户按得太快：直接丢弃，不阻塞界面。
    return xQueueSend(s_txq, &job, 0) == pdTRUE;
}

// ---------------------------------------------------------------------------
// 异步请求（串行 worker）
// ---------------------------------------------------------------------------

static void ble_ctrl_task(void *arg)
{
    (void)arg;
    ble_req_t req;
    while (xQueueReceive(s_req_q, &req, portMAX_DELAY) == pdTRUE) {
        switch (req) {
        case BLE_REQ_FINDER_START:
            s_finder_err = app_ble_finder_start();
            break;
        case BLE_REQ_FINDER_STOP:
            app_ble_finder_stop();
            s_finder_err = ESP_OK;
            break;
        case BLE_REQ_REMOTE_START:
            s_remote_err = app_ble_remote_start();
            break;
        case BLE_REQ_REMOTE_STOP:
            app_ble_remote_stop();
            s_remote_err = ESP_OK;
            break;
        }
    }
}

// 首次请求时建队列与 worker。worker 常驻：退出页面后的停止请求也要有人接。
// 只由界面线程调用，因此不需要额外的锁。
static void ensure_ctrl(void)
{
    if (s_req_q) return;
    s_req_q = xQueueCreate(8, sizeof(ble_req_t));
    if (!s_req_q) return;
    if (xTaskCreate(ble_ctrl_task, "ble_ctrl", 4096, NULL, 5, &s_req_task) != pdPASS) {
        vQueueDelete(s_req_q);
        s_req_q = NULL;
    }
}

static void post_req(ble_req_t req)
{
    ensure_ctrl();
    if (s_req_q) (void)xQueueSend(s_req_q, &req, 0);
}

void app_ble_finder_request_start(void) { post_req(BLE_REQ_FINDER_START); }
void app_ble_finder_request_stop(void)  { post_req(BLE_REQ_FINDER_STOP); }
esp_err_t app_ble_finder_last_error(void) { return s_finder_err; }

const char *app_ble_finder_error_text(void)
{
    return s_finder_reason[0] ? s_finder_reason : NULL;
}

void app_ble_remote_request_start(void) { post_req(BLE_REQ_REMOTE_START); }
void app_ble_remote_request_stop(void)  { post_req(BLE_REQ_REMOTE_STOP); }
esp_err_t app_ble_remote_last_error(void) { return s_remote_err; }

const char *app_ble_remote_error_text(void)
{
    return s_remote_reason[0] ? s_remote_reason : NULL;
}

// ---------------------------------------------------------------------------
// 通用
// ---------------------------------------------------------------------------

bool app_ble_active(void)
{
    return s_mode != BLE_MODE_IDLE;
}