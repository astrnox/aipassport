<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 通行证随身工具箱

面向 **FoloToy AI Passport** 可穿戴设备（ESP32-C3，240 × 320 竖屏 LCD，三个实体按键）
的离线优先日常工具箱。它是在仓库硬件测试基线上开发的衍生应用：`components/bsp`
中可复用的板级逻辑保持不变，而应用页面、导航、视觉设计与按键行为全部围绕本产品重新设计。

产品目标很简单：没有网络时设备依然有用。日历、作息倒计时、番茄钟、电子工牌与离线动态口令
在完全断网时都能正常使用。唯一的在线模块是英雄联盟赛事中心，断网时降级为清晰标注新鲜度的本地缓存。

完整需求、交互规则与验收标准见[产品需求文档](docs/product/passport-toolbox-prd.md)；
可滚动的交互原型随文档一同提供：[`passport-toolbox-ui-prototype.html`](docs/product/passport-toolbox-ui-prototype.html)。

## 应用提供什么

| 板块 | 要点 |
| --- | --- |
| 主页 | 三张常驻信息卡片（时间、下一作息节点倒计时、进行中或下一场关注比赛）位于六项模块列表之上，配状态栏与逐页按键提示条。 |
| 时间与日历 | 公历日历叠加农历、节气与宜忌；今日／本周／本月／今年／人生五档时间进度；秒表与倒计时。 |
| 专注与效率 | 可配置专注与休息时长的番茄钟，记录掉电不丢；最多 16 条按星期重复的本地提醒。 |
| 作息与倒计时 | 按星期定义作息节点并支持单双周，当前节点高亮，到下一节点大字倒计时。 |
| 身份与工具 | 最多五张电子工牌与离线二维码；离线 RFC 6238 动态口令，同时显示当前口令、剩余有效时间与下一个口令；硬件自检。 |
| 英雄联盟赛事中心 | 今日与本周赛程、实时比分、积分榜、战队与单场对局详情，按固定优先级排序并缓存优先呈现。 |
| 系统 | 配网（热点网页与 BLE）、校时、息屏与省电、主题、声音、关注战队、数据备份与清除。 |

## 离线优先设计

- 除赛事中心与校时外，所有页面在无网络时都可进入并完成操作，不存在"必须联网"的阻断页。
- 全部用户数据（工牌、作息、提醒、番茄钟状态、关注战队、动态口令密钥、赛事缓存）保存在本地 NVS。
- 赛事中心先渲染缓存结果，再后台刷新；离线时标注数据新鲜度。
- 界面为中文，应用内置生成的点阵中文字库与覆盖校验，界面文案与用户自定义名称都不会出现缺字方框。

## 硬件能力契约

| 能力 | 已确认实现 |
| --- | --- |
| 显示 | ST7789P3，240 × 320 竖屏 RGB565，SPI，LEDC 背光 |
| 输入 | `UP`、`DOWN`、`OK` 共用一个 ADC 电阻分压（GPIO0），不支持组合键 |
| 音频 | ES8311 codec，走 I2S，支持播放与录音 |
| 电池 | CW2017 电量计（剩余电量与电压） |
| Wi-Fi | 2.4 GHz STA，仅赛事中心、配网与校时使用 |
| 蓝牙 | NimBLE peripheral，仅配网使用 |
| 存储 | 8 MB Flash 上的 NVS，无 PSRAM |

所有引脚、地址与面板参数均从
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h) 读取，
应用层不复制硬件常量。

## 构建与测试

需要 ESP-IDF **5.5.3** 与 ESP32-C3 目标。参见
[环境搭建](docs/development/engineering/environment-setup.zh_CN.md)与
[构建与测试](docs/development/engineering/build-and-test.zh_CN.md)。

```bash
./tools/validate.sh --static     # 仓库检查 + 纯逻辑主机测试
./tools/validate.sh --firmware   # ESP-IDF 构建 + 合并镜像校验
./tools/validate.sh              # 完整门禁（需已激活 ESP-IDF 5.5.3）
```

固件门禁在 `build/FoloToy-AI-Passport-full.bin` 产出经校验的合并镜像，从 `0x0` 偏移烧录。

构建通过不等于硬件验证。交付时请分别报告构建结果、主机测试结果、真机测试结果与未验证项。

## 交付状态

经校验的合并镜像已随仓库交付，见
[`dist/FoloToy-AI-Passport-full.bin`](dist/FoloToy-AI-Passport-full.bin)，校验值见
[`dist/SHA256SUMS`](dist/SHA256SUMS)。单个文件已包含 bootloader、分区表与应用，写入 `0x0` 偏移。

```bash
esptool.py --chip esp32c3 -b 460800 --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 dist/FoloToy-AI-Passport-full.bin
```

| 检查项 | 结果 |
| --- | --- |
| 固件构建与合并镜像校验 | PASS |
| 仓库检查与主机侧纯逻辑测试 | PASS |
| 真机测试 | NOT RUN —— 需连接开发板并取得烧录授权 |
| 未验证项 | 真机渲染与中文字形覆盖、列表选中项自动滚动到可视区域、按键时序（每次按压一个动作、长按 500 毫秒、连续快按）、BLE 配网端到端、赛事实时数据与离线降级、时间校准后的单双周切换、功耗表现 |

`dist/` 是交付快照：重新运行固件门禁只刷新 `build/`，需要更新 `dist/` 及其校验值时应显式操作。
本次交付不含浅睡眠与深睡眠，已实现自动息屏与省电选项。

按键语义属于 [BSP 按键契约](components/bsp/include/bsp_button.h)：每次按压一个动作，
抬起发 `BSP_BTN_CLICK`，按住到 `BSP_BTN_LONG_PRESS_MS` 发 `BSP_BTN_LONG`。
不做双击手势——它的判定窗口会拖慢每一次单击，还会把连续快按合并成被丢弃的动作。

## 项目结构

```text
components/bsp/   可复用板级支持（显示、按键、音频、电池、共享 I2C）
main/             应用：纯逻辑、页面、导航、任务与资源
assets/fonts/     生成的中文字库及其可复现来源
tests/            无需硬件的纯逻辑主机测试
tools/            本地与 CI 共用的校验脚本
docs/product/     产品需求文档与界面原型
dist/             交付的合并固件与校验和
```

## 文档

| 资源 | 内容 |
| --- | --- |
| [产品需求](docs/product/passport-toolbox-prd.zh_CN.md) | 完整功能、交互与验收规范 |
| [AI 开发指南](docs/development/ai-guide.zh_CN.md) | 开发流程与运行时约束 |
| [中文字库](docs/development/engineering/lvgl-chinese-fonts.zh_CN.md) | 字库生成、接入与字形覆盖校验 |
| [硬件指南](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) | 引脚表、总线与验收清单 |
| [文档索引](docs/README.zh_CN.md) | 完整文档地图 |

## 许可

沿用上游仓库，采用 MIT 许可。见 [LICENSE](LICENSE)。
