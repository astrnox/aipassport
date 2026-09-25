// main/net/app_blufi.h —— BLE 配网（BLUFI）：设备以 FoloPassport 名义广播，手机端
// EspBlufi App 下发 2.4 GHz Wi-Fi 凭证，设备保存并连接。
//
// 与热点配网并列的另一条配网路径，适用于"设备已开机、手机在身边，但不想切 Wi-Fi"的
// 场景。两条路径最终都落到 app_net 的凭证存储与 STA 连接逻辑上，不各管一份 Wi-Fi。
#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef enum {
    APP_BLE_PROV_OFF = 0,      // 未开启
    APP_BLE_PROV_ADVERTISING,  // 正在广播，等待手机连接
    APP_BLE_PROV_CONNECTED,    // 手机已连接，等待下发凭证
    APP_BLE_PROV_APPLYING,     // 已收到凭证，正在连接 Wi-Fi
    APP_BLE_PROV_DONE,         // Wi-Fi 已连上，配网完成
    APP_BLE_PROV_FAILED,       // 开启或连接失败
} app_ble_prov_state_t;

// 阻塞式开启：拉起 BT 控制器与 NimBLE 主机、注册 BLUFI 回调、开始广播。
// BLE 协议栈初始化耗时较长，必须在 worker task 中调用，不要占用界面线程。
// 已开启时直接返回 ESP_OK。
esp_err_t app_ble_prov_start(void);

// 停止广播并释放 BLE 协议栈与 BT 控制器（Wi-Fi 侧状态不动，由 app_net 决定何时释放）。
// 幂等，可在未开启时调用。
void app_ble_prov_stop(void);

bool                app_ble_prov_active(void);
app_ble_prov_state_t app_ble_prov_state(void);
// 设备广播名，供界面展示。
const char         *app_ble_prov_name(void);
// 失败原因（简短中文），无则返回 NULL。
const char         *app_ble_prov_error(void);
