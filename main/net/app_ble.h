// main/net/app_ble.h —— 新的蓝牙功能（"找设备"与"万能遥控"）共用的 BLE 传输层。
//
// 为什么单独一个模块：BLE 协议栈（BT 控制器 + NimBLE 主机）在本机只能有一份。
// 配网（app_blufi）已经自己管了一份生命周期，本模块再自己管一份就会互相踩。约定：
//   * 同一时刻只允许一个蓝牙角色运行，"找设备"（观察者扫描）与"万能遥控"（HID 外设）
//     也互斥——进入其中一个页面会先停掉另一个。
//   * 与配网同样互斥：配网在跑时拒绝开启，本模块在跑时配网也不能开启。
//   * BLE 与 Wi-Fi 共用一路 2.4G 射频，因此本模块只在对应页面停留期间持有协议栈，
//     离开页面立即释放，不做后台常驻，避免和赛事/校时/信道扫描抢射频。
//
// 线程约定：start/stop 会初始化或拆掉协议栈（耗时数百毫秒），必须在派生的 worker
// 任务里调用，界面回调不得阻塞。扫描结果与 HID 状态都只在本模块内部维护，界面通过
// 快照接口读取，不直接触碰 NimBLE。
#pragma once

#include "esp_err.h"
#include "logic/app_finder.h"
#include "logic/app_remote.h"

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 找设备（BLE 观察者扫描）
// ---------------------------------------------------------------------------
// 开启广播扫描并把结果喂进内部设备表。阻塞式，须在 worker 中调用。已开启时直接返回。
esp_err_t app_ble_finder_start(void);
// 停止扫描并释放协议栈。幂等，可在未开启时调用。
void      app_ble_finder_stop(void);
bool      app_ble_finder_running(void);

// 复制一份当前设备表（含收藏）到 out。界面在自己的节拍里调用，内部已加锁。
// 复制前会顺手淘汰超时设备并排序，保证界面拿到的总是"当前该显示的几台"。
void app_ble_finder_snapshot(app_finder_t *out);

// 收藏 / 取消收藏。内部会写 NVS，重启后仍在。返回值与 logic/app_finder 一致。
int  app_ble_finder_save(const uint8_t addr[6], const char *name);
bool app_ble_finder_unsave(const uint8_t addr[6]);

// 异步请求开启 / 停止扫描：内部有一个常驻 worker 串行处理，界面回调可安全调用、
// 不会因为拉起或拆掉协议栈（数百毫秒）而卡住 LVGL。重复请求合并为一次。
void    app_ble_finder_request_start(void);
void    app_ble_finder_request_stop(void);
// 最近一次开启请求的结果（ESP_OK 表示成功）。界面用它把失败原因显示给用户。
esp_err_t app_ble_finder_last_error(void);

// ---------------------------------------------------------------------------
// 万能遥控（BLE HID 外设：键盘 + 消费类控制）
// ---------------------------------------------------------------------------
// 开始广播并等待主机（手机 / 电脑 / 电视）配对连接。阻塞式，须在 worker 中调用。
esp_err_t app_ble_remote_start(void);
void      app_ble_remote_stop(void);
bool      app_ble_remote_running(void);
// 主机是否已连接。未连接时按键报文发不出去，界面据此提示"去手机蓝牙里连一下"。
bool      app_ble_remote_connected(void);

// 按当前模式发一次按键。内部把"按下 / 松开"两条报文排队交给发送任务，界面立刻返回，
// 不会因为 BLE 发送而卡住 LVGL。未连接或无映射返回 false。
bool app_ble_remote_press(app_remote_mode_t mode, app_remote_btn_t btn, app_remote_press_t press);

// 异步请求开启 / 停止遥控，语义与 app_ble_finder_request_* 一致。
void      app_ble_remote_request_start(void);
void      app_ble_remote_request_stop(void);
esp_err_t app_ble_remote_last_error(void);

// ---------------------------------------------------------------------------
// 通用
// ---------------------------------------------------------------------------
// 是否存在任一由本模块开启的蓝牙角色。配网开启前用它做互斥判断。
bool app_ble_active(void);