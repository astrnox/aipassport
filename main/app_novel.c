// main/app_novel.c —— novel 分区读写实现。接口与掉电安全约定见 app_novel.h。
#include "app_novel.h"

#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "logic/app_text.h"

#include <string.h>

static const char *TAG = "app_novel";

#define NOVEL_TYPE     0x40
#define NOVEL_SUBTYPE  0x01
#define SECTOR_SIZE    0x1000u

// 头部与索引共用一个扇区（0x0~0x1000），正文从 0x1000 开始。
_Static_assert(sizeof(app_novel_header_t) == APP_NOVEL_HEADER_SIZE,
               "app_novel_header_t 必须与 APP_NOVEL_HEADER_SIZE 一致");
_Static_assert(sizeof(app_novel_chapter_t) == 36,
               "章节索引条目的布局是分区格式的一部分");
_Static_assert(APP_NOVEL_HEADER_SIZE + APP_NOVEL_MAX_CHAPTERS * sizeof(app_novel_chapter_t)
                   <= APP_NOVEL_TEXT_OFFSET,
               "章节索引必须放得进头部与正文之间的扇区");

static const esp_partition_t *s_part;
static bool s_ready;

// 同一时刻只保留一份正文映射。
static esp_partition_mmap_handle_t s_map_handle;
static bool s_mapped;
static const uint8_t *s_map_text;
static uint32_t s_map_len;

static bool header_blank(const uint8_t *probe, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (probe[i] != 0xFF) return false;
    }
    return true;
}

esp_err_t app_novel_init(void)
{
    if (s_ready) return ESP_OK;

    const esp_partition_t *part = esp_partition_find_first(
        (esp_partition_type_t)NOVEL_TYPE, (esp_partition_subtype_t)NOVEL_SUBTYPE, "novel");
    if (!part) {
        ESP_LOGE(TAG, "找不到 novel 分区；小说功能不可用");
        return ESP_ERR_NOT_FOUND;
    }
    if (part->size <= APP_NOVEL_TEXT_OFFSET) {
        ESP_LOGE(TAG, "novel 分区 %u 字节太小，装不下头部与正文", (unsigned)part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_part = part;
    s_ready = true;
    s_mapped = false;
    ESP_LOGI(TAG, "novel 就绪: 偏移 0x%lx 大小 %u 字节, 正文上限 %u 字节",
             (unsigned long)part->address, (unsigned)part->size,
             (unsigned)(part->size - APP_NOVEL_TEXT_OFFSET));
    return ESP_OK;
}

bool app_novel_ready(void)
{
    return s_ready;
}

uint32_t app_novel_capacity(void)
{
    return s_ready ? (uint32_t)(s_part->size - APP_NOVEL_TEXT_OFFSET) : 0;
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

esp_err_t app_novel_header(app_novel_header_t *out)
{
    if (!out || !s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t probe[APP_NOVEL_HEADER_SIZE];
    esp_err_t err = esp_partition_read(s_part, 0, probe, sizeof(probe));
    if (err != ESP_OK) return err;
    if (header_blank(probe, sizeof(probe))) return ESP_ERR_NOT_FOUND;

    app_novel_header_t header;
    memcpy(&header, probe, sizeof(header));

    app_novel_status_t status = app_novel_header_check(&header, app_novel_capacity(), NULL);
    if (status != APP_NOVEL_OK) {
        ESP_LOGW(TAG, "小说头部无效: %d", (int)status);
        return ESP_ERR_INVALID_CRC;
    }
    *out = header;
    return ESP_OK;
}

bool app_novel_present(void)
{
    if (!s_ready) return false;
    app_novel_header_t header;
    return app_novel_header(&header) == ESP_OK;
}

// 直接映射一段正文。写入提交阶段分区里还没有合法头部，因此不能走"先读头部"的
// app_novel_map()。两种调用都遵守"同一时刻只保留一份映射"。
static esp_err_t map_text_range(uint32_t bytes, const uint8_t **text,
                                esp_partition_mmap_handle_t *handle)
{
    if (s_mapped) return ESP_ERR_INVALID_STATE;
    if (bytes == 0 || bytes > app_novel_capacity()) return ESP_ERR_INVALID_ARG;

    const void *ptr = NULL;
    esp_partition_mmap_handle_t h = 0;
    esp_err_t err = esp_partition_mmap(s_part, APP_NOVEL_TEXT_OFFSET, bytes,
                                       ESP_PARTITION_MMAP_DATA, &ptr, &h);
    if (err != ESP_OK) return err;

    s_mapped = true;
    s_map_handle = h;
    s_map_text = (const uint8_t *)ptr;
    s_map_len = bytes;
    if (text) *text = s_map_text;
    if (handle) *handle = h;
    return ESP_OK;
}

esp_err_t app_novel_map(const uint8_t **text, uint32_t *len,
                        esp_partition_mmap_handle_t *handle)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    app_novel_header_t header;
    esp_err_t err = app_novel_header(&header);
    if (err != ESP_OK) return err;

    err = map_text_range(header.data_bytes, text, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "映射正文失败: %s", esp_err_to_name(err));
        return err;
    }
    if (len) *len = s_map_len;
    return ESP_OK;
}

void app_novel_unmap(esp_partition_mmap_handle_t handle)
{
    if (!s_mapped) return;
    (void)handle;
    esp_partition_munmap(s_map_handle);
    s_mapped = false;
    s_map_handle = 0;
    s_map_text = NULL;
    s_map_len = 0;
}

int app_novel_chapter_count(void)
{
    if (!s_ready) return 0;
    app_novel_header_t header;
    if (app_novel_header(&header) != ESP_OK) return 0;
    return (int)header.chapter_count;
}

bool app_novel_chapter(int index, app_novel_chapter_t *out)
{
    if (!s_ready || !out || index < 0) return false;
    int count = app_novel_chapter_count();
    if (index >= count) return false;

    uint32_t offset = APP_NOVEL_HEADER_SIZE + (uint32_t)index * sizeof(app_novel_chapter_t);
    if (esp_partition_read(s_part, offset, out, sizeof(*out)) != ESP_OK) return false;

    // 索引自洽性：偏移必须落在正文范围内且有序。任何一条不满足就视为索引损坏，
    // 界面据此退化为按百分比跳转，而不是拿着坏偏移去切正文。
    app_novel_header_t header;
    if (app_novel_header(&header) != ESP_OK) return false;
    if (out->offset >= header.data_bytes) return false;
    if (index > 0) {
        app_novel_chapter_t prev;
        uint32_t prev_off = APP_NOVEL_HEADER_SIZE + (uint32_t)(index - 1) * sizeof(prev);
        if (esp_partition_read(s_part, prev_off, &prev, sizeof(prev)) != ESP_OK) return false;
        if (prev.offset >= out->offset) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 写
// ---------------------------------------------------------------------------

static uint32_t round_up_sector(uint32_t bytes)
{
    return (bytes + SECTOR_SIZE - 1) / SECTOR_SIZE * SECTOR_SIZE;
}

esp_err_t app_novel_write_begin(app_novel_writer_t *writer, const char *title,
                                uint32_t payload_bytes)
{
    if (!writer || !s_ready) return ESP_ERR_INVALID_STATE;
    memset(writer, 0, sizeof(*writer));

    if (payload_bytes == 0) return ESP_ERR_INVALID_ARG;
    if (payload_bytes > app_novel_capacity()) {
        ESP_LOGW(TAG, "上传 %u 字节超过上限 %u", (unsigned)payload_bytes,
                 (unsigned)app_novel_capacity());
        return ESP_ERR_INVALID_SIZE;
    }

    // 书名必须是合法 UTF-8，否则界面上会显示乱码；超长按字符截断。
    uint32_t st = 0;
    const char *t = title ? title : "";
    if (!app_novel_utf8_feed(&st, (const uint8_t *)t, strlen(t)) ||
        !app_novel_utf8_finish(st)) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先擦头部扇区（含索引）：这一步之后分区立即表现为"没有书"，之后任何失败都不会
    // 留下指向半截正文的合法头部。
    esp_err_t err = esp_partition_erase_range(s_part, 0, SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除头部失败: %s", esp_err_to_name(err));
        return err;
    }

    // 再擦本次要写入的正文扇区。
    uint32_t text_bytes = round_up_sector(payload_bytes);
    err = esp_partition_erase_range(s_part, APP_NOVEL_TEXT_OFFSET, text_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除正文失败: %s", esp_err_to_name(err));
        return err;
    }

    app_utf8_copy_prefix(t, 8, writer->title, sizeof(writer->title));
    writer->expected = payload_bytes;
    writer->written = 0;
    writer->crc = 0;
    writer->utf8_state = 0;
    writer->active = true;
    return ESP_OK;
}

esp_err_t app_novel_write_chunk(app_novel_writer_t *writer, const void *data, size_t len)
{
    if (!writer || !writer->active) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;
    if (!data) return ESP_ERR_INVALID_ARG;
    if (writer->written + len > writer->expected) {
        ESP_LOGW(TAG, "上传超出声明长度 %u", (unsigned)writer->expected);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!app_novel_utf8_feed(&writer->utf8_state, (const uint8_t *)data, len)) {
        ESP_LOGW(TAG, "上传内容不是合法 UTF-8");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t offset = APP_NOVEL_TEXT_OFFSET + writer->written;
    esp_err_t err = esp_partition_write(s_part, offset, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写正文偏移 %u 失败: %s", (unsigned)offset, esp_err_to_name(err));
        return err;
    }

    writer->crc = esp_crc32_le(writer->crc, (const uint8_t *)data, (uint32_t)len);
    writer->written += (uint32_t)len;
    return ESP_OK;
}

// 回读正文并重算 CRC：闪存写错不会返回错误，只能靠读回来发现。
static esp_err_t verify_text_crc(uint32_t bytes, uint32_t expected_crc)
{
    static uint8_t buf[1024];      // 静态而非栈：提交发生在 HTTP worker 的栈上
    uint32_t crc = 0;
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t n = bytes - done;
        if (n > sizeof(buf)) n = sizeof(buf);
        esp_err_t err = esp_partition_read(s_part, APP_NOVEL_TEXT_OFFSET + done, buf, n);
        if (err != ESP_OK) return err;
        crc = esp_crc32_le(crc, buf, n);
        done += n;
    }
    return crc == expected_crc ? ESP_OK : ESP_ERR_INVALID_CRC;
}

// 扫章节并把索引写进头部扇区。整本书扫一遍是毫秒级，换来的是打开目录时零扫描。
static uint16_t write_chapter_index(uint32_t bytes)
{
    const uint8_t *text = NULL;
    if (map_text_range(bytes, &text, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "正文映射失败，这本书将没有章节索引");
        return 0;
    }

    uint16_t count = 0;
    size_t from = 0;
    app_novel_chapter_t entry;
    while (count < APP_NOVEL_MAX_CHAPTERS) {
        memset(&entry, 0, sizeof(entry));
        size_t at = app_novel_next_chapter((const char *)text, bytes, from,
                                           entry.title, sizeof(entry.title));
        if (at >= bytes) break;
        entry.offset = (uint32_t)at;

        uint32_t off = APP_NOVEL_HEADER_SIZE + (uint32_t)count * sizeof(entry);
        if (esp_partition_write(s_part, off, &entry, sizeof(entry)) != ESP_OK) {
            ESP_LOGW(TAG, "写章节索引失败，已写入 %u 条", (unsigned)count);
            break;
        }
        count++;
        from = at + 1;      // 同一行不会重复命中
    }

    app_novel_unmap(s_map_handle);
    ESP_LOGI(TAG, "章节索引: %u 条", (unsigned)count);
    return count;
}

esp_err_t app_novel_write_commit(app_novel_writer_t *writer)
{
    if (!writer || !writer->active) return ESP_ERR_INVALID_STATE;
    if (writer->written != writer->expected) {
        ESP_LOGW(TAG, "上传未收齐: %u / %u", (unsigned)writer->written,
                 (unsigned)writer->expected);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!app_novel_utf8_finish(writer->utf8_state)) {
        ESP_LOGW(TAG, "上传内容以半个汉字结尾");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t chapters = write_chapter_index(writer->expected);

    app_novel_header_t header;
    app_novel_header_init(&header, writer->title, writer->expected, writer->crc, chapters);

    // 头部最后写：它是"这本书完整可用"的提交标记。
    esp_err_t err = esp_partition_write(s_part, 0, &header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写头部失败: %s", esp_err_to_name(err));
        return err;
    }

    memset(writer, 0, sizeof(*writer));

    app_novel_header_t readback;
    err = app_novel_header(&readback);
    if (err != ESP_OK) return err;
    if (readback.data_bytes != header.data_bytes || readback.data_crc != header.data_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    return verify_text_crc(readback.data_bytes, readback.data_crc);
}

void app_novel_write_abort(app_novel_writer_t *writer)
{
    if (!writer) return;
    if (writer->active && s_ready) {
        // 擦掉头部扇区，分区回到"没有书"；正文不再被引用，无需一并擦除。
        esp_partition_erase_range(s_part, 0, SECTOR_SIZE);
    }
    memset(writer, 0, sizeof(*writer));
}

esp_err_t app_novel_erase(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_mapped) app_novel_unmap(s_map_handle);
    return esp_partition_erase_range(s_part, 0, SECTOR_SIZE);
}