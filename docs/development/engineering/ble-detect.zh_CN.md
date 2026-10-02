<p align="right">
  <strong>简体中文</strong> · <a href="ble-detect.md">English</a>
</p>

# BLE 检测（被动蓝牙侦测）

`BLE 检测` 是一个工具页子页，它**只听不发射**地监听附近蓝牙广播并做分类。它是 GhostESP
`detect_ble_spam_callback` 与 `airtag_scanner_callback` 思路的"防御/被动"移植：不发射、不欺骗、
不断开连接、不开任何主动无线电角色。它复用 `main/net/app_ble.c` 里已有的**"找设备"观察者扫描**
——没有第二个 NimBLE 角色，也没有第二份协议栈。

## 仅被动 / 防御用途——先读这一段

本模块是一个侦测器，不是攻击工具。它：

- 只读取设备本来就公开广播的**公开**数据；
- 做启发式分类并给出一句人话结论；
- **不发射任何东西**。

它不对任何设备下"身份"结论：蓝牙 MAC 会随机化、多数设备不广播名字，而厂商/服务数据模式只是
保守的"像什么"信号。判定都是建议性的（"这附近像在刷屏""你身边有个像 AirTag 的设备"），绝非证据。

请只用它了解自己所处的环境，不要用它去骚扰、跟踪或干扰与你无关的他人。

## 它检测什么

从观察到的广播流里算出三种结论：

1. **Apple 连续广播轰炸**——大量不同广播源，都是 Apple（`company_id 0x004C`）且其厂商数据前缀
   匹配已知的连续广播载荷：
   - `4C 00 07 19 07`（空中载荷 `1e ff 4c 00 07 19 07`），以及
   - `4C 00 04 04 2a 00`（空中载荷 `16 ff 4c 00 04 04 2a 00`）。

   当滑动窗口内出现过的此类不同源数量达到 `APP_BLEDETECT_SPAM_THRESHOLD`（8）时，判为"检测到疑似
   BLE 广播轰炸"。
2. **AirTag / Find My 存在**——某 Apple 广播源的厂商数据匹配 Find My 服务数据 `12 19`，或 `07 19`
   配对模式，或广播了 Find My 网络服务 UUID `0xFD5A`。判为"发现 AirTag / Find My 设备"。
3. **其它 / 未知广播源**——既非轰炸候选也非 AirTag 的源数量（含非 Apple 设备、以及没有已知模式的
   Apple 设备）。

判定优先级：轰炸 > AirTag > 正常。

## 厂商数据 / 服务数据从哪来

纯逻辑侦测器需要原始厂商数据字节来匹配连续广播前缀，但 `app_finder_t`（finder 暴露的快照）原本
只带了**启发式类别**，并不携带底层标识。因此扩展了该快照（`main/logic/app_finder.h` / `.c`），
让每台设备额外携带：

- `company_id`（16 位）、`mfg_type`（厂商 ID 之后的第一个字节）——由 `app_finder_feed` 从它已有的
  参数直接写入（未改动 feed 的公开签名）；
- `mfg_data[APP_FINDER_MFG_BYTES]` + `mfg_data_len`——厂商自定义数据字段（NimBLE 的 `fields.mfg_data`，
  小端 2 字节厂商 ID 在前）的**前导**字节。完整数据最长 31 字节，但前 8 字节已足够匹配本侦测器需要的
  所有前缀，故只存前导。由新增的 `app_finder_attach_mfg()` 写入；
- `svc16[APP_FINDER_SVC_MAX]` + `svc16_count`——16 位服务 UUID 列表，从 `app_finder_feed` 已有的
  `uuid16` 参数复制而来。

`main/net/app_ble.c` 的扫描回调（`finder_gap_event`）现在在 `app_finder_feed()` 之后、同一把锁内调用
`app_finder_attach_mfg()`，于是 UI 每次 poll 到的快照就已经包含侦测器所需的全部信息。没有新蓝牙角色，
也没有重复协议栈。

### 关于"速率"

UI 轮询的是"找设备"快照（按地址去重，约 800ms 一刷），每个节拍把每台设备喂给侦测器。因此侦测器统计
的是**滑动窗口内出现过的不同 Apple 连续广播源数量**，而非原始报文数。对随机 MAC 的轰炸——每个报文都是
新地址——计数照样会冲高并触发阈值；对少数合法 Apple 设备则稳定在很低的数字。这是对 GhostESP 按报文计
速率的一种适配（因为 UI 基于去重快照），属于启发式，已在文档中说明。

## 纯逻辑层 —— `main/logic/app_bledetect.{c,h}`

不引用任何 ESP-IDF / LVGL（只有 `<stdbool.h>`、`<stdint.h>`、`<stddef.h>`、`<string.h>`）。暴露：

- `app_bledetect_reset()`——清空全部内部状态（进页时调用一次）。
- `app_bledetect_feed(const app_bledetect_ad_t *)`——喂入一条观察到的广播。`now_ms` 须单调；同一地址
  重复喂会刷新其窗口时间戳并合并类别标记。
- `app_bledetect_report(app_bledetect_report_t *)`——依据当前滑动窗口算出判定 + 计数 + 几条样例地址。
- `app_bledetect_verdict_text()` / `app_bledetect_kind_text()`——给 UI 用的中文标签。

一切状态都由喂入的 `(地址, 时间戳)` 驱动，**不调用 `esp_random`**，因此主机测试完全确定性可复现。

## 界面 —— `main/ui/ui_bledetect.c`

沿用 `ui_finder.c` / `ui_blelab.c` 的结构：

- `enter`——`app_bledetect_reset()`，然后 `app_ble_finder_request_start()`（异步；绝不在 LVGL 锁下拉起
  协议栈）。
- `exit`——`app_ble_finder_request_stop()`，再删屏。
- `tick`（每 `BD_TICK_MS = 800`）——poll `app_ble_finder_snapshot()`，把每台设备喂给侦测器，渲染报告。
- `key`——长按 OK 返回工具页；短按 OK 暂停/恢复监听（只控制接收）。

屏幕上显示：被动声明横幅（"被动监听，不发送任何广播"）、一行判定（正常 / 轰炸 / AirTag，按结论配色）、
一行计数、以及最多 `APP_BLEDETECT_EG_MAX` 条样例地址的小列表。复用 `ui_list_create` / `ui_row_create` /
`ui_page_set_hint`。

在 `main/ui/ui_tools.c` 注册为 `TOOL_BLEDETECT`（名称"BLE 检测"，提示"附近蓝牙在刷屏吗"），加入全部五个
`subpage_*` 分支，并在 `main/ui/ui_pages.h` 声明。

## 构建 / 测试接线

- `main/CMakeLists.txt` 登记了 `logic/app_bledetect.c` 与 `ui/ui_bledetect.c`。
- `tools/validate.sh` 在 `--static` 的逻辑测试循环里加入 `bledetect`，编译并运行 `tests/test_app_bledetect.c`
  （连同 `main/logic/app_bledetect.c` + `main/logic/app_text.c`）。

## 主机测试

`tests/test_app_bledetect.c` 用确定性输入断言每一种结论：正常环境（非 Apple 源）、AirTag / Find My（含
`12 19` / `07 19` 厂商模式与 `0xFD5A` 服务 UUID）、Apple 连续广播轰炸（两种已知前缀、超过阈值）、"源不足不
误报"、滑动窗口衰减、以及结论文案。由 `tools/validate.sh --static` 编译执行。
