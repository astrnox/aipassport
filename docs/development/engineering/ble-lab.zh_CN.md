<p align="right">
  <strong>简体中文</strong> · <a href="ble-lab.md">English</a>
</p>

# BLE 实验（BLE Advertising Lab）

`BLE 实验` 是工具页的一个子页：它把设备变成一个**仅广播（broadcaster）**角色——只向外
发出原始 BLE 广播报文，从不与任何设备建立连接。它是 `main/net/app_ble.c` 管理的第三个
蓝牙角色，前两个是"找设备"（观察者扫描）与"万能遥控"（HID 外设）。

本模块的意义，是把那些公开、有文档记载的 BLE 广播格式做成便于复现的形态，用于**学习研究
与授权测试**，并且放在一道明确的授权告知之后。在用户确认理解法律边界之前，它完全不发射
任何字节。授权标志只存在于页面内存中，退出即作废。

## 仅限授权使用——请先阅读

向你不拥有、或未获授权测试的设备广播特制报文，可能违反当地无线电管理法规与平台使用条款，
也可能打扰、干扰附近的用户。模块在发射任何字节之前，都要求用户在一条中文法律/授权告知上
按 OK 明确确认；该授权标志只存在于页面内存，退出即清除。

仅可在以下情形使用：

- 你拥有的设备；或
- 你已获书面授权测试的设备；并且
- 处于合法、已授权的环境。

由此产生的一切后果由使用者自行承担。

## 模式

四种模式复刻的载荷均来自公开仓库（来源见下文）：

| 模式 | 载荷长度 | 说明 |
| --- | ---: | --- |
| 苹果音频（`APPLE_AUDIO`） | 31 字节 | AirPods 式连续弹窗；`modelId` 在索引 7 |
| 苹果设置（`APPLE_SETUP`） | 23 字节 | AppleTV/HomePod 式设置弹窗；`modelId` 在索引 13 |
| Swift Pair（`SWIFT_PAIR`） | 10 字节（+ 可选名字） | Windows 快速配对；可配置 Class of Device 与名字 |
| iBeacon（`IBEACON`） | 27 字节 | 苹果 iBeacon 服务数据；可配置 UUID/major/minor/tx |

苹果模式下，广播器每轮换一轮就切到设备表的下一台，因此弹窗目标会不断变化。Swift Pair 与
iBeacon 保持固定身份（由对应 setter 设置）并持续重播。

## 纯逻辑层 —— `main/logic/app_blelab.{c,h}`

该层**不依赖 ESP-IDF 与 LVGL**（仅 `<stdbool.h>`、`<stdint.h>`、`<stddef.h>`、`<string.h>`）。
对外接口：

- `app_blelab_build_packet(mode, index, buf, cap, *out_len)` —— 填充 `buf` 并回报真实/所需
  长度。当 `cap` 不足时返回 `APP_BLELAB_ERR_BUF_TOO_SMALL`，并把所需长度写入 `*out_len`。
- `app_blelab_packet_size(mode)` —— 该模式固定长度。
- 苹果设备表访问器（`app_blelab_apple_count`、`app_blelab_apple_dev`）。
- 身份 setter：`app_blelab_set_ibeacon(...)`、`app_blelab_set_swift_cod(...)`、
  `app_blelab_set_swift_name(...)`。
- 确定性 PRNG 助手：`app_blelab_xorshift32(state)` 与
  `app_blelab_random_addr(state, out[6])`（首字节"或" `0xF0`，即随机静态地址）。不使用
  `esp_random`——同一 seed 必得同一序列，这正是主机测试所依赖的性质。

### 精确字节布局

- **苹果音频（31 字节）：** `1e ff 4c 00 07 19 07` + 索引 7 处的 `modelId` +
  `20 75 aa 30 01 00 00 45 12 12 12`（第 8–18 字节），其余以 0 补齐至 31。
- **苹果设置（23 字节）：** `16 ff 4c 00 04 04 2a 00 00 00 0f 05 c1`（13 字节）+
  索引 13 处的 `modelId` + `60 4c 95 00 00 10 00 00 00`（9 字节）。
- **Swift Pair（10 字节）：** `09 ff 06 00 03 02 80` + 3 字节 CoD（小端）。可选
  `Complete Local Name` 以 `长度 09 <名字...>` 追加。
- **iBeacon（27 字节）：** `1a ff 4c 00 02 15` + 16 字节 UUID + major（大端）+
  minor（大端）+ tx power（有符号）。

来源：`EvilAppleJuice-ESP32/src/devices.cpp` 与 `devices.hpp`（苹果）、
`ESP32-SwiftSpam/ESP32-SwiftSpam.ino`（Swift Pair）、
`ESP32-BLEBeaconSpam/*.ino`（iBeacon）。

## BLE 角色 —— `main/net/app_ble.c` 中的 advertiser

advertiser 与 finder、remote 共用同一份协议栈，并接入**同一套互斥仲裁**：

- 单一 `ble_mode_t`（`BLE_MODE_IDLE` / `BLE_MODE_FINDER` / `BLE_MODE_REMOTE` /
  `BLE_MODE_ADV`）保证同一时刻只有一个角色。
- `app_ble_adv_start()` 在 `s_mode != BLE_MODE_IDLE`（finder/remote 占用）、
  `app_ble_prov_active()`（配网进行中）、`app_net_channel_scan_running()`（Wi-Fi 信道
  体检）时拒绝启动。反之：finder/remote 在 advertiser 占用时也会拒绝；而配网与信道体检因为
  `app_ble_active()` 在任一角色（含 advertiser）活动时返回 true 而拒绝——双向互斥成立。
- 开启/停止走**同一套异步请求/worker 模式**（`BLE_REQ_ADV_START` / `BLE_REQ_ADV_STOP` 投递
  到 `s_req_q`），界面因此不会被协议栈拉起/拆除阻塞。
- 同样的 `last_error()` / `error_text()` 模式，向用户回报一句话原因。

生命周期与其余代码一致：`app_ble_adv_start()` 先置 `s_mode = BLE_MODE_ADV` 再调用
`stack_up()`；真正的广播发生在 `on_sync` 回调（`adv_start_late`）里——它通过
`ble_hs_id_set_rnd()` 设置**随机静态地址**（由 `app_blelab_random_addr` 按 seed 派生），
用 `ble_gap_adv_set_data()` 推出首包，并以**不可连接、通用可发现**方式启动广播
（`BLE_GAP_CONN_MODE_NON`、`BLE_GAP_DISC_MODE_GEN`）。一个周期性 `esp_timer` 不断调用
`ble_gap_adv_set_data()` 轮换载荷（苹果模式轮换设备）。`app_ble_adv_stop()` 停止定时器、
停止广播并照常回滚调用 `stack_down()`。

### 已核对的 NimBLE 符号（ESP-IDF 5.5.3）

`ble_gap_adv_set_data`、`ble_gap_adv_start`/`ble_gap_adv_stop`、
`BLE_GAP_CONN_MODE_NON`、`BLE_GAP_DISC_MODE_GEN`、`BLE_GAP_EVENT_ADV_COMPLETE`、
`ble_hs_id_set_rnd`、`BLE_OWN_ADDR_RANDOM`、`ble_hs_util_ensure_addr`。broadcaster 角色已在
`sdkconfig.defaults` 中开启（`CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`），无需改动 sdkconfig。

## 界面 —— `main/ui/ui_blelab.c`

结构沿用 `ui_finder.c`，分两个视图：

1. **授权视图** —— 展示中文授权/法律告知，发射任何内容前必须按 OK 明确确认。授权是页面内存
   标志，退出即清除。
2. **实验室视图** —— 模式列表（↑↓ 选择）、OK 开始/停止、状态行，以及通过 `ui_page_set_hint`
   给出的提示。所有蓝牙工作均异步（`app_ble_adv_request_start/stop`），绝不在 LVGL 锁内
   触碰 NimBLE。

在 `main/ui/ui_tools.c` 中注册为 `TOOL_BLELAB`（名称 `BLE 实验`），并加入全部五个
`subpage_*` 分支；在 `main/ui/ui_pages.h` 中声明。

## 主机测试

`tests/test_app_blelab.c` 断言各模式的精确字节与长度、"缓冲不足时返回所需长度"行为、非法
入参处理、苹果设备表、iBeacon/Swift Pair 的 setter，以及 PRNG 的确定性。它由
`tools/validate.sh --static` 编译并执行。
