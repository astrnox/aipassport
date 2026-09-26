// main/app_assets.h —— assets 分区的读写入口（动图帧存储）。
//
// 为什么单独开分区：一段动图的帧数据在几十到几百 KB 量级，远超 NVS 单条 blob 的稳妥
// 上限，也不适合反复擦写 NVS 分区。assets 用 ESP-IDF 保留给应用自定义格式的 type
// 0x40/0x00，格式完全由本模块定义，不涉及任何文件系统。
//
// 布局：分区被切成 APP_ANIM_SLOT_MAX 个定长槽位（app_assets_slot_size()），槽位 i 的
// 起始地址固定。固定偏移是刻意的：名片在 NVS 里只记一个槽位号，若槽位大小随分区尺寸
// 浮动，换个分区表就会让已存的动图错位。
//
// 槽位内容 = 48 字节头部（app_anim_header_t） + 连续的 RGB565 帧数据。
//
// 掉电安全：写流程是"先擦头部 -> 写帧 -> 最后写头部"。任何一步断电，槽位头部都还是
// 擦除态，因此会稳定地表现为"空槽位"，不会留下指向半截数据的合法头部。代价是上传
// 失败会丢掉该槽位原有动图——槽位语义本就如此，界面在上传前会明确提示覆盖。
//
// 读取走 esp_partition_mmap：帧数据可以零拷贝直接交给 LVGL 当图源，不占内部 RAM。
// 同一时刻只保留一份映射，切槽位时先解映射。
#pragma once

#include "esp_err.h"
#include "esp_partition.h"

#include "logic/app_anim.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 查找 assets 分区并校验其容量。失败时其余接口都不可用，界面应提示"动图不可用"。
esp_err_t app_assets_init(void);
bool      app_assets_ready(void);

// 单个槽位容量；分区未就绪返回 0。
uint32_t app_assets_slot_size(void);
// 槽位号越界或分区未就绪返回 false。
bool     app_assets_slot_valid(int slot);

// 读槽位头部并校验头部自洽性。空槽位返回 ESP_ERR_NOT_FOUND，损坏返回
// ESP_ERR_INVALID_CRC。
esp_err_t app_assets_slot_header(int slot, app_anim_header_t *out);

// 槽位是否存有可用动图（头部合法）。用于界面判断"未设置/已设置"。
bool app_assets_slot_present(int slot);

// 清空槽位：只擦头部所在扇区，帧数据留在原地不再被引用，因此瞬间完成。
esp_err_t app_assets_slot_erase(int slot);

// 全量校验：读回全部帧数据并比对头部记录的 CRC-32。上传结束后调用一次，
// 之后播放不必重复校验。需要一次 mmap，耗时与数据量成正比。
esp_err_t app_assets_slot_verify(int slot, const app_anim_header_t *header);

// 把帧数据映射到只读地址空间。成功时 *frames 指向第一帧，*map_handle 需在播放
// 结束后交给 app_assets_unmap。同一时刻只允许一份映射。
esp_err_t app_assets_map(int slot, const app_anim_header_t *header,
                         const uint8_t **frames, esp_partition_mmap_handle_t *map_handle);
// 解除映射并更新缓存指针。重复调用或传入无效句柄是安全的。
void app_assets_unmap(esp_partition_mmap_handle_t map_handle);

// 已映射区域中第 index 帧的指针；没有映射或 index 越界返回 NULL。
const uint8_t *app_assets_frame(const app_anim_header_t *header, int index);

// ---------------------------------------------------------------------------
// 流式写入：HTTP 上传边收边写，避免把整段动图放进内存
// ---------------------------------------------------------------------------

typedef struct {
    int              slot;
    app_anim_meta_t  meta;
    char             name[APP_ANIM_NAME_LEN];
    uint32_t         expected;    // 手机声明的帧数据字节数
    uint32_t         written;     // 已落盘字节数
    uint32_t         crc;         // 增量 CRC-32 状态
    bool             active;
} app_assets_writer_t;

// 开始写入。会先校验参数，再把头部扇区与本次所需的帧数据扇区擦掉。
// 校验失败或分区不可用时返回错误，writer 保持非活动状态。
esp_err_t app_assets_write_begin(app_assets_writer_t *writer, int slot,
                                 const app_anim_meta_t *meta, const char *name,
                                 uint32_t payload_bytes);
// 追加一段帧数据。超出声明长度会报错（不静默截断）。
esp_err_t app_assets_write_chunk(app_assets_writer_t *writer, const void *data, size_t len);
// 收齐后提交：写头部、回读校验头部与数据 CRC。提交成功槽位才真正可用。
esp_err_t app_assets_write_commit(app_assets_writer_t *writer);
// 放弃写入：擦掉头部扇区，让槽位回到"空"。可重复调用。
void app_assets_write_abort(app_assets_writer_t *writer);

// 供界面显示的槽位总用量（已占用槽位数 / 总槽位数）。
int app_assets_used_slots(void);
