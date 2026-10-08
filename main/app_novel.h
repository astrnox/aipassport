// main/app_novel.h —— novel 分区的读写入口（离线小说的正文存储）。
//
// 为什么单独开分区：一本小说的正文在几百 KB 到 1MB 量级，远超 NVS 单条 blob 的稳妥
// 上限，也不适合反复擦写 NVS 分区。novel 用 ESP-IDF 保留给应用自定义格式的
// type 0x40 / subtype 0x01，格式完全由本模块与 logic/app_novel 定义，不涉及文件系统。
//
// 布局（详见 logic/app_novel.h）：头部 64B -> 章节索引（最多 96 条）-> 正文（从 0x1000
// 开始）。正文固定偏移，因此容量计算与擦除都按扇区对齐，且章节索引不占常驻内存——
// 界面按需读单条，正文直接 mmap 零拷贝交给 LVGL。
//
// 掉电安全：写流程是"先擦头部扇区（0x0~0x1000，含索引）-> 写正文 -> 扫章节写索引 ->
// 最后写头部"。任何一步断电，分区都表现为"没有书"，不会留下指向半截正文的合法头部。
// 代价是上传失败会丢掉原来那本书——上传前配置页会明确提示覆盖。
#pragma once

#include "esp_err.h"
#include "esp_partition.h"

#include "logic/app_novel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 查找 novel 分区并校验容量。失败时其余接口都不可用，界面应提示"小说不可用"。
esp_err_t app_novel_init(void);
bool      app_novel_ready(void);

// 分区能容纳的正文上限（分区大小 - 正文偏移）。
uint32_t app_novel_capacity(void);

// 读头部并校验。空分区返回 ESP_ERR_NOT_FOUND，损坏返回 ESP_ERR_INVALID_CRC。
esp_err_t app_novel_header(app_novel_header_t *out);
// 是否存有一本可读的书。
bool app_novel_present(void);

// 把正文映射到只读地址空间（零拷贝）。成功时 *text 指向正文首字节，*len 为正文长度。
// 用完必须把 handle 交给 app_novel_unmap。
esp_err_t app_novel_map(const uint8_t **text, uint32_t *len,
                        esp_partition_mmap_handle_t *handle);
void      app_novel_unmap(esp_partition_mmap_handle_t handle);

// 章节索引：条数来自头部；单条按需读取，界面不需要整张表。
int  app_novel_chapter_count(void);
bool app_novel_chapter(int index, app_novel_chapter_t *out);

// ---------------------------------------------------------------------------
// 流式写入：HTTP 上传边收边写，避免把整本书放进内存
// ---------------------------------------------------------------------------

typedef struct {
    char     title[APP_NOVEL_TITLE_LEN];
    uint32_t expected;      // 手机声明的正文字节数
    uint32_t written;       // 已落盘字节数
    uint32_t crc;           // 增量 CRC-32
    uint32_t utf8_state;    // 流式 UTF-8 校验状态
    bool     active;
} app_novel_writer_t;

// 开始写入：校验长度上限与书名，擦掉头部扇区与本次需要的正文扇区。
// 校验失败或分区不可用时返回错误，writer 保持非活动状态。
esp_err_t app_novel_write_begin(app_novel_writer_t *writer, const char *title,
                                uint32_t payload_bytes);
// 追加一段正文。超出声明长度、或不是合法 UTF-8 都会报错（不静默截断）。
esp_err_t app_novel_write_chunk(app_novel_writer_t *writer, const void *data, size_t len);
// 收齐后提交：扫章节写索引、写头部，再回读校验正文 CRC。
esp_err_t app_novel_write_commit(app_novel_writer_t *writer);
// 放弃写入：擦掉头部扇区，分区回到"没有书"。可重复调用。
void app_novel_write_abort(app_novel_writer_t *writer);

// 删除当前这本书（只擦头部扇区，正文不再被引用）。
esp_err_t app_novel_erase(void);