# WiFi 热点（SoftAP）与蓝牙启动失败：原因深入分析与排查优先级

> 适用范围：ESP32-C3、8MB Flash、**无 PSRAM**、ESP-IDF 5.5.3、本仓库当前 `main` 分支持久代码。
> 覆盖对象：Wi-Fi SoftAP 热点配网（`main/net/app_net.c`）、Wi-Fi STA / 信道扫描、BLE 配网 BLUFI（`main/net/app_blufi.c`）、BLE「找设备 / 万能遥控」（`main/net/app_ble.c`）。
> 结论基于源码取证，未做真机复现；标「待验证」的条目需要在设备上加日志确认。
>
> 说明：本文件应要求仅提供简体中文，未按仓库「英文 + `.zh_CN.md` 成对」的维护文档约定；它是排查分析用的临时文档，不是纳入索引的维护文档。

---

## 0. 结论速览（先看这张表）

| 排名 | 现象 | 最可能原因 | 最快验证方式 |
| --- | --- | --- | --- |
| 1 | 热点开启失败，日志 `配网网页启动失败 ...（最大连续块 N 字节）` 且 N 很小 | 内部堆 / 最大连续块不足（无 PSRAM，碎片化） | 看日志里「最大连续块」是否 < 约 20KB |
| 2 | 蓝牙开启失败，日志 `BT 控制器初始化失败: ESP_ERR_INVALID_STATE` | 上一次失败把 BT 控制器留在 init/enable 脏状态 | 重启设备后第一次能否成功 |
| 3 | 第一次能开、退出再进就失败 | 状态标志短路 / 回滚不彻底（脏状态累积） | 重启后是否恢复；连续进出 5 次 |
| 4 | 提示「已在使用，请先退出另一个页面」 | Wi-Fi/BLE/扫描/信道体检互相争用同一路 2.4G 射频 | 检查是否同时开着「找设备 / 遥控 / 信道体检 / 赛事中心」 |
| 5 | 热点起得来但手机连不上 / 打不开网页 | AP 参数、DHCP、HTTP socket、网页资源问题 | 看 `WIFI_EVENT_AP_STACONNECTED` 与 `IP_EVENT_AP_STAIPASSIGNED` 是否都出现 |
| 6 | 蓝牙扫描（找设备）失败 `开始扫描失败 rc=...` | NimBLE `OBSERVER` 角色未编入 / 内存不足 | 检查 `CONFIG_BT_NIMBLE_ROLE_OBSERVER` |
| 7 | 长时间运行后随机失败、或伴随重启 | 电源 / 射频校准（PHY）/ 功耗管理（DFS） | 换稳定电源、关 DFS 复测 |

---

## 1. 排查前置：先把失败分成三类

「热点启动失败」和「蓝牙启动失败」在日志上往往是一句笼统的错误，先分型能省掉大量时间：

- **A 类：射频 / 协议栈压根没起来**（`esp_wifi_start` / `esp_bt_controller_init` / `esp_nimble_enable` 返回非 0）。
- **B 类：协议栈起来了，但对外不可见**（AP 不广播、BLE 不广播 / 扫不到）。
- **C 类：对外可见，但交互失败**（手机连上 AP 却拿不到 IP / 打不开配置页；BLE 连上却配网失败）。

先采集这些日志锚点（都在现网源码里，直接对照）：

| 日志字符串 | 位置 | 含义 |
| --- | --- | --- |
| `配网[%s] 空闲堆 %u 字节，最大连续块 %u 字节` | `main/net/app_net.c:2692-2697` | 配网各阶段内存水位 |
| `配网 Wi-Fi 启动失败: %s` | `main/net/app_net.c:2771` | `wifi_ensure_started()` 失败（A 类） |
| `配网 AP 配置失败: %s` | `main/net/app_net.c:2787` | `esp_wifi_set_config(AP)` 失败 |
| `配网网页启动失败: %s（最大连续块 %u 字节）` | `main/net/app_net.c:2807` | `httpd_start` 失败（最典型的 P0） |
| `配网启动失败已回滚: %s` | `main/net/app_net.c:2753` | 走了 `prov_rollback()` |
| `配网已开启: SSID=%s URL=%s` | `main/net/app_net.c:2852` | 成功锚点 |
| `BT 控制器初始化失败/启用失败`、`NimBLE 初始化失败` | `main/net/app_ble.c:566/571/578` | BLE 协议栈拉起失败（A 类） |
| `启动 NimBLE 主机任务失败: %s` | `main/net/app_ble.c:624` | `esp_nimble_enable()` 失败 |
| `开始扫描失败 rc=%d` | `main/net/app_ble.c:532` | 找设备扫描失败 |
| `推断蓝牙地址类型失败` / `没有可用的蓝牙地址` | `main/net/app_ble.c:519/515` | 地址相关问题 |
| `Wi-Fi 射频启动失败`、`注册 BLUFI 回调失败` | `main/net/app_blufi.c:662/672` | BLUFI 启动失败 |
| `蓝牙配网开启失败` | `main/net/app_blufi.c:703` | BLUFI 最终失败 |
| `联网基础服务就绪（未打开射频）` | `main/net/app_net.c:2955` | 开机基础设施 OK 锚点 |

**内存口径统一**：同时打 `esp_get_free_heap_size()` 与 `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`。在无 PSRAM 的 C3 上，**最大连续块比空闲总量更能预测失败**（仓库自身注释即强调这点，见 `main/net/app_net.c:2689-2691`）。

---

## 2. Wi-Fi 热点（SoftAP）启动失败

启动主流程：`app_net_prov_start()` = `wifi_ensure_started()` → `set_mode(APSTA)` → `set_config(AP)` → `httpd_start()` → 注册 URI（`main/net/app_net.c:2756-2854`）。下面按排查优先级排序。

### P0（最高）内部堆 / 最大连续块不足 —— 无 PSRAM 下的首因

**机理**：热点配网是全固件最吃**连续内存**的一步。它要同时容纳：httpd 任务栈 6144B（`app_net.c:2796`）、4 个 HTTP socket 各带 lwIP 收发缓冲（`app_net.c:2800`）、AP netif + DHCP 服务、以及已常驻的 LVGL 40KB 独立池（`sdkconfig.defaults:36`）与各类业务缓存（约 13KB 作息 + 8KB 赛事，见 `docs/development/engineering/memory-and-power-optimization.md`）。**碎片化**会让「空闲总量够、连续块不够」从而导致 `httpd_start` 失败。

**判据**：日志出现 `配网网页启动失败 ...（最大连续块 N 字节）`，N 明显偏小（经验阈值：小于约 20KB 即高度可疑）。

**解决方法（任选，建议按序叠加）**：
1. **用现成的「手动清理内存」出口**：长按下键触发 `app_net_prov_reclaim_memory()`（`app_net.c:2702-2718`），它会清赛区/积分榜缓存并异步停掉蓝牙角色，然后重试。碎片场景下「释放后重分配」本身就可能成功。
2. **降低常驻占用**：把 `CONFIG_LV_MEM_SIZE_KILOBYTES` 从 40 再往下调（`sdkconfig.defaults:36`，注意留出绘制峰值余量）；或把作息/赛事等大缓存改为离开页面即释放。
3. **缩减配网自身预算**：`hc.max_open_sockets` 已从默认 7 降到 4（`app_net.c:2800`），可进一步降到 2–3；`hc.stack_size` 6144 可试降到 4096（需实测网页上传不崩）。
4. **延迟/释放无关子系统**：在进入配网页前确保音频、动图解码等大块未初始化；参考 `docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md`「Release optional subsystems」一节（实测最大连续块 13KB→31KB）。
5. **架构级方案**：将 LVGL 改用系统堆（`CONFIG_LV_USE_CLIB_MALLOC`），消除「LVGL 池 / 系统堆两个筒仓」——代价是 LVGL 失败会波及全局堆，文档明确标注**不建议作为第一步**。

**验证**：改造前后对比 `配网[开启前]/[射频启动后]/[AP 配置后]/[配置网页启动后]` 四条堆日志的最大连续块。

### P1 状态标志短路 / 回滚不彻底（表现为「第一次能开、重试永远失败」）

**机理**：`wifi_ensure_started()` 在 `s_wifi_started == true` 时直接返回成功（`app_net.c:300`）。若某次配置失败却把 `s_wifi_started=true`、APSTA 模式、AP 配置留在原地，下一次重试会**跳过配置直接进 `httpd_start`**，而堆比上次更差 → 永远失败。源码注释明确记录了这段历史（`app_net.c:2724-2727`），并提供了统一回滚 `prov_rollback()`（`app_net.c:2728-2754`）。

**判据**：连续多次点开热点，第一次失败后每次都立即失败且日志跳过 `射频启动后` 直接到 `配网页启动失败`。

**解决方法**：
1. **确认走的是带回滚的路径**：所有失败分支必须调用 `prov_rollback()`，不要各点直接 `return`。
2. **配合显式停止清理**：`app_net_prov_stop()` 会清 `s_prov_opened_wifi` 标记（`app_net.c:2856-2888`），退出配网页务必真正调用它，而非只切页面。
3. **加自愈**：在 `app_net_prov_start()` 入口检测「射频已开但 `s_prov_active==false`」的矛盾态，主动执行一次 `esp_wifi_stop()` + `esp_wifi_deinit()` 复位后再重试。
4. **兜底**：给用户一个「重置网络状态」入口，或异常时软重启，避免脏状态跨会话累积。

### P2 射频被其它功能占用 / 模式冲突

**机理**：C3 只有一路 2.4G 射频。热点需 `WIFI_MODE_APSTA`；若此时 BLE 配网（`app_blufi`）、找设备/遥控（`app_ble`）、信道体检扫描（`app_net_channel_scan_*`）正在占用，或 Wi-Fi 处于纯 STA 且被 `esp_wifi_stop()` 关过，都会让 `set_mode`/`start` 或后续共存不稳。

**判据**：日志出现互斥提示（`蓝牙配网进行中...`、`信道体检正在扫描...`，`main/net/app_ble.c:681/688`）；或 `配网 Wi-Fi 启动失败`。

**解决方法**：
1. **产品流程上强互斥**：进入热点配网页前，先 `app_ble_finder_request_stop()` / `app_ble_remote_request_stop()` / `app_ble_prov_stop()`，并等待信道体检结束（界面给出「请先退出 X 页面」的明确提示，仓库已有此设计）。
2. **模式切换顺序修正**：始终先 `esp_wifi_set_mode(WIFI_MODE_APSTA)` 再 `esp_wifi_start()`，避免在 STA-only 状态下直接加 AP 配置。
3. **等待而非硬开**：对短时的信道体检（仅数秒）采用「提示 + 重试」而不是强行抢占（与 `app_ble.c:686-691` 现有策略一致）。
4. **若已合入 PR#10 的 WiFi 实验室**：`deauth/SAE/信标` 会切信道并设 STA 模式，务必保证退出实验页时完整停止任务并恢复到 APSTA/STA 一致状态（见第 5 节）。

### P3 基础设施缺失（`app_net_init` 不完整）

**机理**：`app_net_prov_start()` 首查 `s_inited` 与 `s_ap_netif`，任一不满足直接返回 `ESP_ERR_INVALID_STATE`（`app_net.c:2758-2762`）。`s_ap_netif` 由 `esp_netif_create_default_wifi_ap()` 创建，失败即 `ESP_ERR_NO_MEM`（`app_net.c:2942-2945`）；NVS 异常会走格式化重试（`app_net.c:2923-2929`）。

**判据**：无 `联网基础服务就绪（未打开射频）` 日志；或日志出现 `联网基础设施初始化失败(...)`（`main/main.c:153-157`）。

**解决方法**：
1. 确保 `app_main` 里 `app_net_init()` 先于任何联网/配网 UI 操作（`main/main.c:153`）。
2. 若 `esp_netif_create_default_wifi_ap()` 返回空，先按 P0 释放内存，再重启设备让 netif 重新创建。
3. NVS 若反复 `ESP_ERR_NVS_NO_FREE_PAGES`，检查 `partitions.csv` 的 `nvs` 分区（当前 0x16000=88KB，`partitions.csv:9`）是否被业务写爆。
4. 事件循环注册失败：确认没有重复 `esp_event_loop_create_default()`（已容忍 `ESP_ERR_INVALID_STATE`，`app_net.c:2935`）。

### P4 AP 参数 / 认证配置问题

**机理**：`fill_ap_config()` 固定 `channel=1`、`max_connection=1`、`WIFI_AUTH_WPA2_PSK`、`pmf.required=false`（`app_net.c:2670-2687`）。SSID `FoloPassport`、密码 `folotoy123`（`app_net.c:74-75`，≥8 位满足 WPA2）。

**判据**：`配网 AP 配置失败: %s`。

**解决方法**：
1. **认证模式降级**：老设备/特殊客户端若 WPA2 握手异常，可临时改 `WIFI_AUTH_WPA_WPA2_PSK` 或开放（仅调试，勿出厂）。
2. **信道策略**：固定信道 1 在拥挤环境易受干扰；可改为开机扫描后选最空信道（复用 `app_net_channel_report()`）。
3. **PMF/合规**：某些环境要求 PMF，当前 `required=false`；若目标市场有强制要求需开启并回归测试。
4. **连接数**：`max_connection=1` 是内存保守值；若确有双客户端需求，需先测堆再上调（`app_net.c:2681-2684` 注释）。

### P5 功耗管理 / 时钟影响射频

**机理**：`esp_pm_configure` 启用 DFS，空闲降到 40MHz（`main/main.c:110-121`，未开 light sleep）。射频操作虽会拉高频率，但极端时序下可能影响 BLE/Wi-Fi 时序与共存。

**解决方法**：
1. 复现时把 `min_freq_mhz` 暂时提到 80/160 观察是否消失。
2. 配网/蓝牙期间临时关闭 DFS（`esp_pm_configure` 改 `light_sleep_enable=false` 且频率锁高，或用 `esp_pm_lock`）。
3. 长期方案：仅在配网阶段持锁，退出即释放（与仓库「按需开射频」理念一致）。

### P6 编译配置 / 分区 / 固件版本

**机理**：`sdkconfig.defaults` 未显式列出 Wi-Fi 缓冲、`SW_COEXIST`、国家码等，全部走 IDF 默认。若固件与源码不一致（例如烧的是旧 `dist/*.bin`）、或 `phy_init` 分区缺失导致射频校准数据异常，也会表现为「起不来 / 不稳」。

**解决方法**：
1. 确认烧录的是本次构建产物，核对 `dist/SHA256SUMS`。
2. 确认 `partitions.csv` 含 `phy_init`（当前 `partitions.csv:10`），必要时 `idf.py erase-flash` 后重烧（注意：会清用户数据）。
3. 在生成的 `sdkconfig` 里确认 Wi-Fi/BT/coex 相关项与预期一致（**待验证**：建议导出 `sdkconfig` 后逐项核对 `CONFIG_ESP_COEX_*`）。

### P7 硬件 / 供电 / 天线

**机理**：射频发射瞬时电流较大，供电不足或天线匹配差会导致关联/广播失败、甚至掉电重启。

**解决方法**：
1. 用稳定外部电源与数据线复测，排除电池低压。
2. 检查天线连接与屏蔽；对照 `components/bsp/include/bsp_pins.h` 确认无引脚冲突。
3. 若伴随重启，优先查 brownout（可在 `sdkconfig` 关 brownout 复位以定位）。

---

## 3. 蓝牙启动失败

BLE 协议栈拉起：`esp_bt_controller_init()` → `esp_bt_controller_enable(BLE)` → `esp_nimble_init()` → 配置 `ble_hs_cfg` → 起 GAP/GATT → `esp_nimble_enable(host_task)`（`main/net/app_ble.c:559-640`）；BLUFI 路径类似（`main/net/app_blufi.c:646-681`）。

### P0（最高）BT 控制器被留在「已 init / 已 enable」脏状态

**机理**：`esp_bt_controller_init()` 对**重复初始化**返回 `ESP_ERR_INVALID_STATE`。若某次启动在「控制器已 enable、主机未起来」的中间态失败而未完整回滚，下一次重试会一直失败。源码对这段有明确注释（`main/net/app_blufi.c:683-700`），`app_ble.c:631-639` 的 `fail:` 分支也是为此补的回滚。

**判据**：`BT 控制器初始化失败: ESP_ERR_INVALID_STATE`，且**重启设备后第一次正常**。

**解决方法**：
1. **保证 fail 分支完整逆序回滚**：`disable → deinit` 控制器、注销事件、释放信号量（对照 `app_ble.c:631-639`、`app_blufi.c:683-704`）。
2. **stop 路径补齐中间态**：即使主机没起来，`stop` 也要拆掉已 enable 的控制器（`app_blufi.c:714-719` 已实现，检查其它路径是否同样覆盖）。
3. **入口自检自愈**：启动前用 `esp_bt_controller_get_status()` 判断；若非 `IDLE`，先 `disable+deinit` 复位再 init。
4. **兜底**：失败多次后提示重启，或程序内软重启清脏状态。

### P1 内存不足（无 PSRAM）

**机理**：控制器 + NimBLE 主机 + GATT 注册 + 主机任务栈是**同一时段最紧张**的内存点；此时若还开着 Wi-Fi 或大缓存，极易失败。

**判据**：`BT 控制器初始化失败: ESP_ERR_NO_MEM` / `NimBLE 初始化失败` / `启动 NimBLE 主机任务失败: ESP_ERR_NO_MEM`。

**解决方法**：
1. 开启蓝牙前先释放：关赛事拉取、清赛区缓存、退出信道体检（`app_net_prov_reclaim_memory()` 的思路可复用）。
2. 降低并发：严格「一个蓝牙角色 + 不并开 Wi-Fi 大任务」。
3. 削减常驻：LVGL 池、作息/赛事缓存（同 P0-WiFi）。
4. 调整常驻配置：`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`（当前已是 1，`sdkconfig.defaults:62`），避免多余连接资源。

### P2 与 Wi-Fi 共用 2.4G 射频 / 共存

**机理**：C3 单射频，Wi-Fi 与 BLE 需软件共存。若 Wi-Fi 正在扫描（信道体检）或拉流，BLE 启动/广播会不稳甚至失败。仓库用**互斥**而非硬开规避（`app_ble.c:686-691`、`app_net.h:62-69`）。

**解决方法**：
1. **产品层互斥**：蓝牙角色开启前检查 `app_net_channel_scan_running()`；反之信道体检检查 `app_ble_active()`（`main/net/app_ble.h:69,79`）。
2. **让出射频**：进入 BLE 页面前停掉 Wi-Fi STA 与扫描（`app_net_wifi_stop()`）。
3. **时序缓解**：BLE 广播/扫描放到 Wi-Fi 空闲窗口，或对 BLE 关键操作加短时重试。
4. **配置核对**（待验证）：确认 IDF 生成的 `sdkconfig` 中共存相关项为启用状态。

### P3 蓝牙角色 / 功能互斥

**机理**：同一时刻只允许一个蓝牙角色：配网（`app_blufi`）与「找设备 / 遥控」（`app_ble`）互斥，找设备与遥控之间也互斥（`main/net/app_ble.h:3-10`）。冲突时直接拒绝并给人话提示（`app_ble.c:675-684`、`app_blufi.c:636-642`）。

**判据**：`蓝牙正被另一功能占用，请先退出那个页面` / `请先退出找设备或万能遥控`。

**解决方法**：
1. 界面层保证「进入一个蓝牙页先停另一个」，并在离开页面时真正释放协议栈。
2. 若功能上确需并存，则合并为一个控制器实例、多角色共享（工作量大，需评估共存稳定性）。
3. 提供「一键停掉所有蓝牙角色」的重置入口。

### P4 编译配置缺失（角色 / BLUFI / 加密）

**机理**：`sdkconfig.defaults:51-70` 已开 `BT_ENABLED`、`NIMBLE`、`PERIPHERAL/BROADCASTER/OBSERVER`，关 `CENTRAL`，开 `NVS_PERSIST`、`BLUFI`、`MBEDTLS_DHM_C`。**扫描（找设备）依赖 `OBSERVER`**；BLUFI 依赖 `BLUFI_ENABLE` 与 `DHM_C`。

**判据**：`开始扫描失败 rc=...`（缺 OBSERVER）；BLUFI 里 DH 相关报错（`app_blufi.c:186-228` 一系列 `DH ...失败`）。

**解决方法**：
1. 核对生成的 `sdkconfig` 与 `sdkconfig.defaults` 一致（尤其 `CONFIG_BT_NIMBLE_ROLE_OBSERVER`）。
2. 若不用 BLUFI 可关闭相关项省内存；若用则确保 `MBEDTLS_DHM_C=y`。
3. `NVS_PERSIST` 打开时确保 NVS 分区充足（`partitions.csv:9`），否则 bond 落盘失败（`app_ble.c:429` 加密状态异常）。

### P5 地址 / 复位 / 主机任务问题

**机理**：地址推断失败（`app_ble.c:515-519`）、NimBLE 复位（`app_ble.c:499` `NimBLE 复位, reason=`）、主机任务启动失败（`app_ble.c:624`）、停机信号量创建失败（`app_ble.c:617`）都会导致启动或后续连接失败。

**解决方法**：
1. 地址问题：确认使用公共地址/随机地址策略合法；如读不到地址，检查控制器是否真正 enable。
2. 复位循环：查 `reason` 值定位（内存/断言/看门狗）。
3. 主机任务：确认任务栈与优先级充足；`host_task` 创建失败多与内存相关，回到 P1。
4. 信号量：`s_host_stopped` 创建失败即内存问题，先释放再重试。

### P6 硬件 / 供电 / 射频校准

同 WiFi P7：BLE 发射同样受电源与 PHY 校准影响；`phy_init` 分区缺失或校准数据异常会让广播/扫描不稳。方法：稳定供电复测、确认 `phy_init` 分区、必要时全片擦除后重烧（注意数据）。

---

## 4. Wi-Fi 与蓝牙互相导致的「启动失败」

这一节单独列出，因为它常被误判为「热点 bug」或「蓝牙 bug」，实为**资源争用**：

| 场景 | 表现 | 处理 |
| --- | --- | --- |
| 一边在蓝牙配网，一边开热点 | 热点起不来 / 不稳 | 两者互斥：先关配网再开热点 |
| 找设备扫描中开热点 | 热点启动失败或极慢 | 停扫描（`app_ble_finder_request_stop()`）再开 |
| 信道体检扫描中开蓝牙 | 提示「请等几秒」 | 等体检结束；或退出体检页 |
| 蓝牙 + Wi-Fi 同时跑且内存紧张 | 二者之一随机失败 | 一次只开一个射频使用者，配 `reclaim_memory` |
| 赛事拉取/校时占用 Wi-Fi 时开热点 | `set_mode`/`httpd` 失败 | 先 `app_net_esports_stop()` / 等拉取结束 |

**统一原则**：设备上「射频」是独占资源，`main/net/app_ble.h:8-10` 与 `main/net/app_net.h:62-69` 已把该原则写进接口契约——排查时优先确认「是否同时开了两个射频使用者」。

---

## 5. 若已合入 PR#10（WiFi 实验 / BLE 实验）新增的失败模式

> PR#10 尚未合入当前 `main`（当前 HEAD 为 `eaa25d1`）。若你测试的是该 PR 的固件，需额外关注：

1. **WiFi 实验室**（`deauth / EAPOL logoff / SAE flood / beacon spam`）会切信道并使用 `WIFI_IF_AP`/`WIFI_IF_STA` 发原始帧。它改变了 Wi-Fi 模式与信道，若退出实验页未完整恢复，会导致**随后热点启动失败**。→ 退出实验页必须停任务并把模式与信道恢复一致。
2. **BLE 实验**（Continuity/Swift Pair/iBeacon 广播）与**找设备/BLE 检测**共用同一套 NimBLE 栈，且都占用 2.4G。→ 与热点、配网同样需要互斥。
3. **原始帧注入依赖编译开关**：PR 文档提到若 `sdkconfig.defaults` 未开启对应 flag，代码能编译但调用会失败。→ 烧录前确认该开关已开（**待验证**具体项名）。
4. **固件版本**：PR 更新了 `dist/FoloToy-AI-Passport-full.bin`；若你烧的是旧 bin，上述「实验导致的模式残留」不会出现，但会看到其它行为差异。→ 核对 `dist/SHA256SUMS`。

---

## 6. 通用快速检查清单（按此顺序做，性价比最高）

1. **重启设备后第一次尝试**：还失败 → 偏 P0/P1（内存、配置）；第一次成功、之后失败 → 偏脏状态（WiFi P1 / BT P0）。
2. **看日志里的「最大连续块」**：小于约 20KB → 先按 WiFi P0 / BT P1 处理，其余先不动。
3. **确认没有第二个射频使用者**（配网 / 找设备 / 遥控 / 信道体检 / 赛事拉取 / 实验页）。
4. **用「手动清理内存」出口重试一次**（`app_net_prov_reclaim_memory()`）。
5. **连续进出目标页面 5 次**，观察是否单调恶化（脏状态 / 泄漏）——文档 `softap-provisioning-and-resource-budget.md` 的验证清单也要求此项。
6. **核对固件与源码一致**（`dist/SHA256SUMS`）+ `phy_init` 分区存在。
7. **换稳定电源 + 临时关 DFS** 复测，排除硬件/功耗因素。

**采集命令（示例，需已配置串口/串口监视器）**：

```text
idf.py -p <PORT> monitor          # 观察 app_net / app_ble / app_blufi 的 TAG 日志
idf.py -p <PORT> erase-flash      # 仅在需要清脏状态/校准时使用，会清用户数据
```

**建议补充的日志（当前缺失，能显著加速定位）**：
- 配网失败点统一打印 `esp_wifi_get_mode()` 与实际 mode、`esp_bt_controller_get_status()` 返回值；
- BLE 启动失败点打印 `esp_get_free_heap_size()` 与最大连续块；
- 蓝牙/热点互斥拒绝时打印「当前占用者」的具体状态位。

---

## 7. 已知未验证项（不要当成结论）

- 生成的 `sdkconfig` 中 Wi-Fi 缓冲、`CONFIG_ESP_COEX_*`、国家码等具体取值未逐项核对。
- 各失败场景（内存不足 / 脏状态 / 互斥 / 参数）未在真机复现，优先级排序基于源码机理与仓库注释推断。
- PR#10 的原始帧注入所需具体编译开关名称未确认。
- 内存经验阈值（约 20KB 最大连续块）为工程经验值，需用本机实测校准。

---

## 附：关键源码索引

- Wi-Fi 启动 / 热点：`main/net/app_net.c`（`wifi_ensure_started` 298-317、`app_net_wifi_start` 327-363、`app_net_prov_start` 2756-2854、`prov_rollback` 2728-2754、`app_net_init` 2914-2957）；接口契约 `main/net/app_net.h`。
- BLE 协议栈 / 找设备 / 遥控：`main/net/app_ble.c`（`stack_up` 559-640、`stack_down` 642-663、`app_ble_finder_start` 669+）；接口契约 `main/net/app_ble.h`。
- BLE 配网 BLUFI：`main/net/app_blufi.c`（`app_ble_prov_start` 629-705、`app_ble_prov_stop` 707-730）。
- 入口与功耗：`main/main.c`（`app_main` 98-189）。
- 配置与分区：`sdkconfig.defaults`、`partitions.csv`。
- 参考经验：`docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md`、`docs/development/engineering/memory-and-power-optimization.md`、`docs/development/engineering/wifi-provisioning.md`。