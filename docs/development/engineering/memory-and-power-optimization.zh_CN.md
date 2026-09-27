<p align="right">
  <a href="memory-and-power-optimization.md">English</a> · <strong>简体中文</strong>
</p>

# 内存与节电优化策划

状态：**阶段 2 的内存改造已部分落地；测量与节电仍待做**。下文低风险的内存削减现已
随固件发布；阶段 1 的测量工具与全部节电改动尚未开始。凡标注"估算"的数字，都必须先
用实测值替换，才能据此定案。

## 已实现

以下改动针对赛事拉取、信道扫描、校时/配网时出现的"内存已满"提示：

- LVGL 池 `CONFIG_LV_MEM_SIZE_KILOBYTES` 48 → 40，把 8 KB 还给系统堆。
- 作息页在隐藏标签页时用 `lv_obj_clean()` 释放其控件，重新显示时再构建，四个标签页
  不再同时存活。
- `ui_row_create()` 的选中态改为行自身左边框，不再建独立子对象；`ui_row_t` 直接保存
  标题句柄。
- HTTP JSON 响应按内容长度分配缓冲（未知长度从 4 KB 起增长），不再固定分配
  `NET_HTTP_MAX_BODY`（64 KB）。
- 赛程列表分页（每页 10 行）；战队页新增胜率横条，由块元素字形拼成，不增加对象。
- `app_input` 任务栈由 4 KB 提升到 8 KB；`ui_theme.c` 的控件创建均加 NULL 防护。

这些改动是否足够，仍需实测确认：没有设备上的 `lv_mem_monitor()` 与内部堆水位，
余量只是估算，不是结论。

## 范围与方法

目标芯片：ESP32-C3，8 MB Flash，**无 PSRAM**。因此所有动态数据都落在内部 SRAM
——这才是紧缺资源。

推进顺序是刻意安排的：**先测量，再削峰值，最后才动休眠行为。** 内存修复风险更低，
同时也能降低促使我们做崩溃处理的白屏重启频率；而节电会改变可被用户感知的行为，
必须排在测量之后分阶段推进。

## 当前内存预算

### 常驻的应用状态

[`app_state.c`](../../../main/app_state.c) 持有唯一的 `app_runtime_t`。按模型头文件
计算，占用最大的成员如下：

| 成员 | 大小（计算值） | 说明 |
| --- | --- | --- |
| `routine` | 约 13.2 KB | `APP_ROUTINE_WEEKS(2) × APP_ROUTINE_DAYS(7) × 24 节点 × 40 B`。即使用户没有作息表，也一直常驻。 |
| `esports` | 约 8 KB | 48 条比赛 + 32 条战队 + 一份嵌套的对局详情。 |
| `s_vault_blob` | 最大 `APP_VAULT_CONTAINER_MAX`（约 4.7 KB） | 静态暂存缓冲，不占栈。 |
| `badges`、`reminders`、`totp`、`pomodoro` | 合计几 KB | 5 张工牌、16 条提醒、10 个动态口令账户。 |

前两个大块无论对应模块是否被用到，都会在应用整个生命周期内占用。这是主要的静态
RAM 开销。

### 流水线缓冲与任务栈

| 项目 | 大小 | 来源 |
| --- | --- | --- |
| LVGL 绘制缓冲 | 240 × 40 × 2 B = **19.2 KB**，单缓冲，内部 DMA RAM | [`bsp_display_lvgl.c`](../../../components/bsp/src/bsp_display_lvgl.c) |
| LVGL 堆池 | **40 KB** 静态，与系统堆相互独立 | `CONFIG_LV_MEM_SIZE_KILOBYTES=40`（由 48 下调） |
| LVGL port 任务栈 | 约 7 KB | `ESP_LVGL_PORT_INIT_CONFIG()` 默认值 |
| `app_input` 任务栈 | 8 KB | 已由 4 KB 提升，见 [`main.c`](../../../main/main.c) |
| 网络 worker | 每个 4–8 KB，按请求创建、用完退出 | [`app_net.c`](../../../main/net/app_net.c) |
| `ui_beep` 3 KB、节拍器 4 KB、配网 6 KB | 临时 | 各处 |

### 峰值风险：LVGL 对象数量

系统堆与 40 KB 的 LVGL 池是分开的。当某个页面对象很多时，即便系统堆还有余量，池也
可能被耗尽——这正是"背光已亮却白屏"的故障形态。
[`ui_theme.c`](../../../main/ui/ui_theme.c) 里加的 NULL 防护把这种故障从崩溃降级为
优雅跳过，但并没有消除压力本身。

最坏情况曾是作息页（[`ui_routine.c`](../../../main/ui/ui_routine.c)）：它有四个标签页，
而 `show_tab()` 在构建某个标签页时不会释放此前已构建标签页的内容，四个标签页都访问过
之后，`今日(24 行) + 一周(7) + 编辑(26) + 设置(6)` 会同时存活；每个 `ui_row_t` 占四个
LVGL 对象（容器、指示条、标题、状态值），合计可达约 250 个对象。现已处理：`show_tab()`
释放被隐藏的标签页，行也不再带指示条对象。由此得到的峰值仍需设备上的
`lv_mem_monitor()` 确认；单靠估算不算结论。

## 内存优化方案

### 阶段 1 —— 先测量

- 在进入/离开每个模块页后，以及作息页每次切标签后，记录 `lv_mem_monitor()`
  （free、used、碎片率、历史峰值）。
- 在同样的位置记录 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 与
  `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`。
- 在一次有代表性的使用后，记录 `app_input`、LVGL port 任务与网络 worker 的
  `uxTaskGetStackHighWaterMark()`。
- 在曾经崩溃的确切操作序列上取数：套用/清空模板并切标签、进出模块页、反复按
  OK/上下键、以及配网/联网相关操作之后。

### 阶段 2 —— 削掉峰值（低风险）

1. **释放非当前标签页。** 在 [`ui_routine.c`](../../../main/ui/ui_routine.c) 的
   `show_tab()` 里，对将要隐藏的视图调用 `lv_obj_clean()`，显示时再构建。最坏对象数
   随即收敛到单个标签页。
2. **去掉每行的指示条对象。** `ui_row_create()` 现在为选中条创建了一个子对象。改用
   行自身的左边框（`border_side = LEFT`、2px、强调色）可以画出同样的视觉提示，却
   **不增加对象**，把每行对象数从 4 降到 3；选中状态变成对行的一次样式修改。
3. **改变标题的取法。** `ui_row_set_title_color()` 与 `render_today_rows()` 用
   `lv_obj_get_child(row.obj, 1)` 取标题，这依赖"指示条是子对象 1"。去掉指示条会改变
   该下标；应在 `ui_row_t` 里保存标题标签，而不是依赖子对象顺序。
4. 重新测量。若峰值仍逼近池上限，就在下一个最大的页面（设置、密码本、赛事详情）重复
   第 1–2 步。

### 阶段 3 —— 给池定容

- 池已先于测量从 48 KB 下调到 40 KB，因为观测到的症状是系统堆吃紧。用实测峰值确认
  40 KB 仍能覆盖真实峰值 + 余量；不够就调回去，有余量就继续下调。
- 备选：让 LVGL 直接用系统堆（`CONFIG_LV_USE_CLIB_MALLOC`），池上限与系统堆不再是两
  个孤岛。代价是把一个可预测的硬上限换成共享压力，必须对照堆水位重新评估；**不建议
  作为第一步。**
- 评估能否压缩常驻的 `routine`（约 13 KB）与 `esports`（约 8 KB）（减少缓存比赛数，
  或缩小节点表）。这些是产品取舍，不是白捡的收益。

## 当前功耗状况

- 休眠目前**只关背光**：`sleep_now()` 把背光调到 0 并置一个标志
  （[`ui_app.c`](../../../main/ui/ui_app.c)）。面板仍在被驱动，1 秒的 `app_tick`
  也仍在跑（必须跑，因为提醒、番茄钟计时与作息节点切换都靠它）。
- 那个 1 秒节拍即便在休眠时，仍每 15 拍调用一次 `ui_app_refresh_status()`，会让控件
  失效并迫使"背光已关"的面板发生重绘。
- `CONFIG_LV_DEF_REFR_PERIOD=20` 使 LVGL 刷新定时器每秒唤醒 50 次。没有失效区时它做
  的事很少，但这些唤醒仍消耗 CPU 时间、阻止深度空闲。
- `CONFIG_FREERTOS_HZ=1000` 即 1 kHz 节拍。
- Wi-Fi 与蓝牙按需开启、退出即释放——这已经是很好的基线。参见
  [硬件开发指南](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) 与
  [SoftAP 配网与资源预算](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md)。

背光亮着时功耗由背光主导；背光熄灭后由面板刷新与 CPU 主导。因此现实的收益点是
(a) 休眠时停止无谓重绘，(b) 减少空闲唤醒。

## 节电优化方案

### 阶段 1 —— 休眠时停止无谓工作（低风险）

1. 在 `app_tick` 中，`s_asleep` 为真时跳过周期性的 `ui_app_refresh_status()`，改为
   唤醒时刷新一次。这样就去掉了每 15 秒重绘一次不可见面板的开销。
2. 确认休眠期间没有其它东西再让屏幕失效。各页面的 `tick()` 在休眠时本就被跳过，
   这是对的——把这条约束明确写死。
3. 评估把 `CONFIG_LV_DEF_REFR_PERIOD` 从 20 ms 提到 30–40 ms：刷新唤醒更少。只有在
   带动画的页面（名片动图、节拍器）观感仍然流畅时才可接受，需先实测。

### 阶段 2 —— 低功耗空闲（中等风险）

- 启用 ESP-IDF 电源管理做动态调频：`CONFIG_PM_ENABLE`、
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE`，并为 C3 配置合适的 `esp_pm_configure()` 频率
  区间。这会降低 CPU 频率，并允许在事件之间自动 light sleep。
- 采用前必须解决的交互风险：USB-Serial/JTAG 控制台（light sleep 期间可能掉线）、
  SPI/DMA 通路、I2S 音频通路。这些都必须在设备上验证，不能假设。
- 可考虑把 `CONFIG_FREERTOS_HZ=1000 → 100`。先检查所有依赖节拍时序的地方（按键消抖
  窗口、`lv_tick` 来源、网络超时），不要凭猜测修改。

### 阶段 3 —— 真休眠（产品决策）

- GPIO 唤醒的 light sleep 或 deep sleep，能把空闲功耗压到远低于"仅关背光"。两者在
  `demo_low_power` 里都有演示，并分别见
  [显示刷新与深度休眠](../../reference/shinku-chen/display-refresh-and-deep-sleep.md)
  与 [深度休眠外设断电](../../reference/shinku-chen/deep-sleep-peripheral-power-off.md)。
- 阻碍是行为层的：CPU 睡着或被复位后，提醒、番茄钟倒计时、作息节点提示与网络全部
  停止。这是产品决策，不是技术决策。尤其 deep sleep 会在唤醒时重启应用。
- 若选择 light sleep，它**不是**"关背光"的平替；唤醒路径、外设保持、提醒补报都需要
  重新设计。

## 验证

- 内存改动：重跑崩溃复现序列，确认记录的 LVGL 空闲内存与堆水位都远离上限。跑完整
  门禁（`./tools/validate.sh`），并把构建与主机测试结果分开报告。
- 节电改动：用功率计在至少三种状态下测电流——背光开（100% 与 30%）、休眠——测改动
  前后。报告实测 mA，而不是估算。设备续航再由电芯容量换算得出。
- 任何休眠改动都必须验证唤醒正确性：任意按键都能唤醒并恢复到之前的页面。

## 待定问题

- 哪个目标更重要：尽可能多的续航提升，还是空闲时仍保证后台提醒与计时？这决定阶段 3
  是否在范围内。
- 压缩常驻的 `routine`/`esports` 缓存是否可接受，还是它们当前的容量就是产品要求？
- 是否已有测量手段（功率计与可重复的操作脚本），还是阶段 1 需要先把测量工具搭起来？
