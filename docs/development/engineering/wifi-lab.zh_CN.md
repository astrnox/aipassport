<p align="right">
  <strong>简体中文</strong> · <a href="wifi-lab.md">English</a>
</p>

# Wi-Fi 实验（Wi-Fi Lab / CTF）

`Wi-Fi 实验` 是一个工具页子页面，把设备变成一台**原始 802.11 发射机**，用于 **学习目的与授权环境下的 Wi-Fi 协议栈行为测试**。它是 `main/net/app_net.c` 管理的第四个射频角色，与信道扫描、热点配网、三个 BLE 角色并列。

本模块是用户**自有、已公开的 GhostESP 代码**的**忠实移植**——具体对应 `main/managers/wifi_manager.c` 里构造的那些帧。它**不编写任何新的攻击逻辑**，不改进射程、隐蔽性、选靶或规避手段；它逐字节复刻了原有的帧模板、随机化方式与已知的小瑕疵（含 beacon 模板里一处"多带 2 字节"的 off-by-two），使行为与 GhostESP 完全一致。其意义是让这些公开、文档完备的 802.11 模式，能够在**受控、经授权的环境**中被轻松复现，并强制经过一层明确的法律/授权确认。

本模块**从不决定"对谁发"**。界面在射频发射任何字节之前，强制要求用户显式确认一份中文法律/授权告知；该确认标志仅存在于页面内存，退出页面即清除。

## 仅限授权环境使用 —— 请先阅读

对不属于你、或未获授权测试的网络或站点发射精心构造的 802.11 帧，可能违反当地的无线电频谱管理法规（例如非授权频段的发射规则）、计算机滥用/反黑客相关法律以及平台服务条款，并可能干扰周边用户。模块在发射任何字节之前，都要求用户在中文法律/授权告知上显式点击确认。

仅在以下情形使用：

- 在你**拥有**的网络与设备上，或
- 在你**已获得书面授权**测试的网络与设备上，且
- 处于合法、经授权的环境（例如使用隔离设备的私有实验室）。

你须对输出内容的使用方式**独自承担全部责任**。作者与本移植版本对滥用行为不承担任何责任。

## 模式

四种顶层模式复刻了来自 GhostESP 的帧（见下方来源说明）；信标模式另有三种子模式。

| 模式 | 帧类型 | 报文长度 | 说明 |
| --- | --- | ---: | --- |
| 解除认证 / 解除关联（`APP_WIFILAB_DEAUTH`） | 解除认证（及解除关联） | 26 B | 含 AP→站点 与 站点→AP 两个方向；原因码可配置 |
| EAPOL 下线（`APP_WIFILAB_EAPOL_LOGOFF`） | 802.1X EAPOL Logoff | 36 B | 让已关联站点以为自己被踢下线 |
| SAE 洪泛（`APP_WIFILAB_SAE_FLOOD`） | SAE 认证（Commit） | 128 B | WPA3 握手提交帧洪泛；scalar+element 随机化 |
| 信标洪泛（`APP_WIFILAB_BEACON_SPAM`） | Beacon | 38 + ssid_len + 12 + 3 + 13 | 三种子模式（见下） |

信标子模式（`app_wifilab_beacon_t`）：

| 子模式 | SSID 来源 | 说明 |
| --- | --- | --- |
| `APP_WIFILAB_BEACON_RANDOM` | 随机 8 位字母数字 | 随机本地 MAC；在信道 1..11 上逐信道广播 |
| `APP_WIFILAB_BEACON_RICKROLL` | Rickroll 歌词（前 5 句） | 每轮循环使用一句歌词作为 SSID |
| `APP_WIFILAB_BEACON_AP_LIST` | 信道扫描发现的 SSID | 把周边 AP 的 SSID 重新广播出去 |

在 `BEACON_SPAM` 下，发射机遍历信道 1..11（与 GhostESP 的 `wifi_manager_broadcast_ap` 信道范围一致）；在 `AP_LIST` 下则复用最近一次信道扫描的结果（`app_net_channel_report()`），而非生成随机 SSID。

## 纯逻辑层 —— `main/logic/app_wifilab.{c,h}`

本层**不含任何 ESP-IDF 或 LVGL 头文件**（仅依赖 `<stdbool.h>`、`<stdint.h>`、`<stddef.h>`、`<string.h>`）。它是一个纯粹的"给定模式与目标，算出该在空中发出的精确字节"的函数库，因此每一帧都可以在主机上验证，而不必打扰任何真实网络。真正的发射由 `app_net` 的 Wi-Fi 实验角色完成。

对外 API：

- `app_wifilab_build_deauth(buf, cap, bssid, sta, reason, *out_len)` —— 解除认证（AP→站点）。
- `app_wifilab_build_disassoc(buf, cap, bssid, sta, reason, *out_len)` —— 解除关联（AP→站点）。
- `app_wifilab_build_deauth_rev(buf, cap, bssid, sta, reason, *out_len)` —— 解除认证（站点→AP）。
- `app_wifilab_build_disassoc_rev(buf, cap, bssid, sta, reason, *out_len)` —— 解除关联（站点→AP）。
- `app_wifilab_build_eapol_logoff(buf, cap, ap_bssid, sta_mac, *out_len)` —— EAPOL 下线。
- `app_wifilab_build_sae_commit(buf, cap, target_bssid, src_mac, frame_counter, *out_len)` —— SAE 提交。
- `app_wifilab_build_beacon(buf, cap, sub, ssid, ssid_len, bssid, channel, *out_len)` —— 信标。
- `app_wifilab_rickroll_count(void)` / `app_wifilab_rickroll_line(index)` —— Rickroll 歌词集合。

所有构造器都把实际（或所需）长度写入 `*out_len`；当 `cap` 不足时返回 `APP_WIFILAB_ERR_BUF_TOO_SMALL` 并把所需长度写入 `*out_len`；`buf` / `out_len` 为 `NULL`，或必填地址为 `NULL`，返回 `APP_WIFILAB_ERR_INVALID_ARG`。

确定性 PRNG（不依赖 `esp_random`）：

- `app_wifilab_set_seed(uint32_t)` —— 设定模块 PRNG 种子。
- `app_wifilab_xorshift32(uint32_t *state)` —— 标准 xorshift32：`x ^= x<<13; x ^= x>>17; x ^= x<<5`。同种子必得同序列，主机测试正依赖此特性。
- `app_wifilab_random_mac(uint8_t out[6])` —— 随机本地单播地址，首字节 `& 0xFE | 0x02`（与 GhostESP 的 `generate_random_mac` 一致）。
- `app_wifilab_random_ssid(char *out, size_t len)` —— 随机字母数字 SSID，字符集与 GhostESP 的 `generate_random_ssid` 一致（`ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789`，`RANDOM_SSID_LEN = 8`）。

### 精确字节布局（忠实复刻 GhostESP）

- **解除认证 / 解除关联（26 B）：** `C0 00 3A 01`（解除关联为 `A0 00 3A 01`）+ `dst[6]` + `src[6]` + `bssid[6]` + 序列控制 `[2]`（高 12 位取自 PRNG）+ `07 00`（原因码 7，可配置）。`_rev` 变体交换方向：dst = AP BSSID，src/BSSID = 站点 MAC。
- **EAPOL 下线（36 B）：** `08 01 00 00` + `dst = ap_bssid[6]` + `src = sta_mac[6]` + `bssid = ap_bssid[6]` + 序列 `00 00` + LLC/SNAP `AA AA 03 00 00 00 88 8E` + EAPOL `01 02 00 00`（版本 1、类型 Logoff、长度 0）。
- **SAE 提交（128 B）：** `B0 00` + `dst = target_bssid[6]` + `src = src_mac[6]` + `bssid = target_bssid[6]` + 序列 `[2]` = `(prng & 0xFFF0) | (frame_counter & 0x000F)` + 认证体 `03 00`（算法 SAE）`01 00`（事务 = Commit）`00 00`（状态 0）`13 00`（组 19）+ scalar `[32]` + element `[64]`（二者均由 PRNG 完全随机化）。
- **信标：** 长度 `38 + ssid_len + 12 + 3 + 13`。
  - `80 00 00 00`（beacon）+ `dst = ff:ff:ff:ff:ff:ff` + `src[6]` + `bssid[6]` + 序列 `C0 6C` + 时间戳 `[8]`（清零）+ beacon 间隔 `64 00` + 能力 `11 04`。
  - SSID IE：`00`（tag）+ `len` + `ssid[len]`。
  - 支持的速率 IE：`01 08 82 84 8B 96 24 30 48 6C`（GhostESP 源码把这一块按 12 字节计数，导致帧尾**多带 2 字节 `0x00`**——这是 GhostESP 真实存在的小瑕疵，此处逐字节复刻）。
  - DS Parameter Set IE：`03 01 <channel>`。
  - HE IE：`FF 0D 50 6F 9A 00 08 00 00 40 00 00 01`。

来源：GhostESP `main/managers/wifi_manager.c` —— `wifi_manager_broadcast_deauth`（含反向 deauth/disassoc）、`eapol_logoff_frame_template` + `eapol_logoff_task`、`SAE_COMMIT_TEMPLATE` + `inject_sae_commit_frame`，以及 `wifi_manager_broadcast_ap`（beacon）。

## Wi-Fi 角色 —— `main/net/app_net.c` 中的 `Wi-Fi 实验`

该角色与信道扫描、热点配网、三个 BLE 角色**共享同一颗 2.4 GHz 射频**，并接入**同一套互斥仲裁**：

- `app_net_wifilab_start()` 在以下情况拒绝：`app_ble_active()`（任何 BLE 角色在运行）、`app_net_channel_scan_running()`（信道扫描在运行）、`app_net_prov_active()`（配网在运行）。反向检查也已补齐：信道扫描请求、配网启动、以及三个 BLE 角色（`app_ble_finder_start`、`app_ble_remote_start`、`app_ble_adv_start`）在 `app_net_wifilab_running()` 为真时都会拒绝。以上射频使用者同一时刻只允许一个活动。
- 启停沿用与 BLE **一致的异步请求/worker 模式**：界面调用 `app_net_wifilab_request_start(...)` / `app_net_wifilab_request_stop()`，向请求队列投递 `WIFILAB_REQ_START` / `WIFILAB_REQ_STOP`；一个常驻的 `wifilab_ctrl_task` 串行执行（阻塞式的）Wi-Fi 启动/释放，从而 LVGL 线程永不在射频启动过程中被阻塞。
- 同样的 `last_error()` / `error_text()` 模式提供一行用户可读的原因（`app_net_wifilab_last_error`、`app_net_wifilab_error_text`）。

`wifilab_start_locked()` 中的生命周期：

1. 执行上述三处互斥检查；若任一射频使用者忙碌，以 `ESP_ERR_INVALID_STATE` + 人话原因返回。
2. `wifi_ensure_started()` 后 `esp_wifi_set_mode()`——除 `APP_WIFILAB_SAE_FLOOD` 用 `WIFI_MODE_STA` 外，其余均用 `WIFI_MODE_AP`（SAE Commit 从 STA 接口发出）。
3. 派生 `wifilab_attack_task`，循环执行：`esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE)` 然后 `esp_wifi_80211_tx(ifx, frame, len, false)`。接口选择复刻 GhostESP：deauth/eapol/beacon 用 `WIFI_IF_AP`，SAE 用 `WIFI_IF_STA`。
4. `BEACON_SPAM` 下循环遍历信道 1..11；`BEACON_AP_LIST` 复用上次扫描报告（`app_net_channel_report()`）的 SSID/BSSID/信道，而非随机值。
5. `wifilab_stop_locked()` 停止任务、清除 active 标志，并仅在角色自身启动了 Wi-Fi（`s_wifilab_was_started`）时才按常规回滚释放射频。

请求参数：`app_net_wifilab_start(mode, beacon, target_bssid[6], target_channel, target_ssid, target_ssid_len, seed)`。每次 `esp_wifi_80211_tx()` 的返回值都会被统计：`app_net_wifilab_packets_sent()` 只计驱动接受（`ESP_OK`）的帧，`app_net_wifilab_packets_failed()` 计被拒的帧，`app_net_wifilab_tx_error_text()` 给出首个拒绝的错误名。界面状态行同时显示三者，帧数不会再被误当成"已经上了空口"。

### 已核实的 `esp_wifi` 符号（ESP-IDF 5.5.3）

`esp_wifi_80211_tx`、`esp_wifi_set_channel`、`esp_wifi_set_mode`、`esp_wifi_get_mac`、`WIFI_IF_AP`、`WIFI_IF_STA`、`WIFI_MODE_AP`、`WIFI_MODE_STA`、`WIFI_SECOND_CHAN_NONE`。以上均已在 `components/esp_wifi/include/` 中确认存在。发射使用 `en_sys_seq = false`，因此我们构造的序列号（以及测试断言的序列号）就是实际被发出的序列号。

## 界面 —— `main/ui/ui_wifilab.c`

沿用 `ui_blelab.c` 的结构。三个视图：

1. **授权视图（`WL_CONSENT`）** —— 展示中文法律/授权告知，要求显式确认 OK 后才允许发射。确认标志是页面内存标志，退出页面即清除。
2. **实验室视图（`WL_LAB`）** —— 模式列表（↑↓ 选择）、相关模式下的信标子模式/目标行、OK 开始/停止、状态行显示"驱动接受的帧数 / 被拒帧数 / 首个错误名"（`app_net_wifilab_packets_sent()`、`_packets_failed()`、`_tx_error_text()`）。所有 Wi-Fi 工作均为异步（`app_net_wifilab_request_start/stop`）；`esp_wifi` 调用不会在 LVGL 锁内发生。
3. **目标视图（`WL_TARGET`）** —— 对需要目标的模式，复用 `app_net_channel_scan_request()` 扫描并展示发现的 AP 列表（上限 `WL_MAX_APS = 7`），供用户选择要作用的 BSSID/信道/SSID。行数已做截断，按键处理不会越界访问列表。

在 `main/ui/ui_tools.c` 中注册为 `TOOL_WIFILAB`（名称 `WiFi 实验`，提示 `CTF/实验室无线测试`），加入全部五个 `subpage_*` switch，并在 `main/ui/ui_pages.h` 中声明。

## 主机测试

`tests/test_app_wifilab.c` 断言了每种模式的精确字节/长度（解除认证、解除关联、反向解除认证、EAPOL 下线、含确定性随机区的 SAE 提交、带随机 SSID/MAC 且信道为 6 的 beacon 及帧尾多带的 2 字节 `0x00`、以及 Rickroll 歌词 beacon）、"容量不足返回所需长度"行为、非法入参处理、xorshift32 确定性（同种子同序列、不同种子不同）、以及 Rickroll 歌词集合（5 句、取模回卷）。它仅依赖标准库与 `logic/app_wifilab.h`，不碰 ESP-IDF/LVGL，并由 `tools/validate.sh --static` 编译执行（已在 `wifilab` 逻辑测试条目中加入）。

`tests/test_net_contract.py` 另外钉住发射统计契约：每一处 `esp_wifi_80211_tx()` 的返回值都必须交给 `wifilab_count_tx()`，两个计数器只允许在该函数内自增——后续改动不能再把"已发帧数"悄悄变回循环次数。

## 构建注意事项 —— `esp_wifi` / `sdkconfig`

- **802.11 原始发射**使用 `esp_wifi_80211_tx`。IDF 5.5.3 在 ESP32-C3 上无条件提供该接口：4.x 时代的 `CONFIG_ESP_WIFI_80211_TX_ENABLED` / `CONFIG_ESP_WIFI_ENABLE_WIFI_TX` 选项已从 `components/` 中整体移除（只余无关的 `ESP_WIFI_ENABLE_WIFI_TX_STATS`），因此"配置门控没开"不是可能的原因。某一帧是否被接受是驱动与芯片的运行时行为；本模块改为记录 `esp_wifi_80211_tx()` 的返回值，不再默认成功。
- **Wi-Fi 模式支持**：`WIFI_MODE_AP` 与 `WIFI_MODE_STA` 须同时可用。仅支持 STA 的目标（部分低端 ESP32 变体 / 某些功耗配置）无法运行 `WIFI_IF_AP` 的 beacon/deauth 路径。GhostESP 把 SAE 洪泛限定在 C5/C6 级别芯片；本移植采用更通用的平台抽象，**未做按 SoC 的门控**，因此在无法作为 AP 运行的目标上，AP 模式相关模式会在 `esp_wifi_set_mode` 处失败。请确认目标支持 AP + STA 模式。
- **区域 / 发射功率**：持续发射精心构造的帧可能超出当地的占空比或功率限制；这属于部署/合规问题，而非构建问题。
- 本模块不调用 `esp_random`，因此逻辑层无需额外的熵源配置；主机测试只覆盖确定性 PRNG。
