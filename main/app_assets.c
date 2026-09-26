// main/app_assets.c —— assets 分区读写实现。接口与掉电安全约定见 app_assets.h。
#include "app_assets.h"

#include "esp_log.h"
#include "esp_partition.h"

#include <string.h>

static const char *TAG = "app_assets";

// 头部长度是闪存布局的一部分：分区里的帧数据从槽位 +48 字节开始。
_Static_assert(sizeof(app_anim_header_t) == APP_ANIM_HEADER_SIZE,
               "app_anim_header_t 必须与 APP_ANIM_HEADER_SIZE 一致，否则槽位偏移全错");

// 每槽 448KB：上限 96x96 的 24 帧 RGB565 动图是 442368 字节，加 48 字节头部仍有余量。
// 这个数字必须与 partitions.csv 里 assets 的大小匹配（6 槽 x 448KB = 2.625MB）。
#define SLOT_SIZE      0x70000u
#define ASSETS_TYPE    0x40
#define ASSETS_SUBTYPE 0x00

static const esp_partition_t *s_part;
static bool s_ready;

// mmap 是全局资源：同一时刻只保留一份，切槽位先解映射。
static esp_partition_mmap_handle_t s_map_handle;
static bool        s_mapped;
static const uint8_t *s_map_frames;   // 帧数据起点
static uint32_t    s_map_slot_offset; // 已映射槽位的分区内偏移，0xFFFFFFFF 表示无
static app_anim_meta_t s_map_meta;    // 已映射槽位的几何，用于校验帧指针请求

// ---------------------------------------------------------------------------
// 初始化与几何
// ---------------------------------------------------------------------------

esp_err_t app_assets_init(void)
{
    if (s_ready) return ESP_OK;

    const esp_partition_t *part = esp_partition_find_first(
        (esp_partition_type_t)ASSETS_TYPE, (esp_partition_subtype_t)ASSETS_SUBTYPE, "assets");
    if (!part) {
        ESP_LOGE(TAG, "找不到 assets 分区；动图功能不可用");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t need = SLOT_SIZE * APP_ANIM_SLOT_MAX;
    if (part->size < need) {
        // 宁可整体禁用也不要"能跑但槽位语义变了"：名片里存的是槽位号，槽位大小一变
        // 已存的动图就会指向错误的位置。
        ESP_LOGE(TAG, "assets 分区 %u 字节，至少需要 %u 字节", (unsigned)part->size,
                 (unsigned)need);
        return ESP_ERR_INVALID_SIZE;
    }

    s_part = part;
    s_ready = true;
    s_mapped = false;
    s_map_slot_offset = 0xFFFFFFFFu;
    ESP_LOGI(TAG, "assets 就绪: 偏移 0x%lx 大小 %u 字节, %d 个槽位 x %u 字节",
             (unsigned long)part->address, (unsigned)part->size, APP_ANIM_SLOT_MAX,
             (unsigned)SLOT_SIZE);
    return ESP_OK;
}

bool app_assets_ready(void)
{
    return s_ready;
}

uint32_t app_assets_slot_size(void)
{
    return s_ready ? SLOT_SIZE : 0;
}

bool app_assets_slot_valid(int slot)
{
    return s_ready && slot >= 0 && slot < APP_ANIM_SLOT_MAX;
}

static uint32_t slot_offset(int slot)
{
    return (uint32_t)slot * SLOT_SIZE;
}

static uint32_t round_up_sector(uint32_t bytes)
{
    const uint32_t sector = 0x1000;
    return (bytes + sector - 1) / sector * sector;
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

esp_err_t app_assets_slot_header(int slot, app_anim_header_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!app_assets_slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    // 先判断头部扇区是不是全 0xFF（擦除态），再读结构；否则空槽位也会被当成
    // "损坏"，界面就无法区分"没设动图"和"动图坏了"。
    uint8_t probe[APP_ANIM_HEADER_SIZE];
    esp_err_t err = esp_partition_read(s_part, slot_offset(slot), probe, sizeof(probe));
    if (err != ESP_OK) return err;

    bool blank = true;
    for (size_t i = 0; i < sizeof(probe); i++) {
        if (probe[i] != 0xFF) { blank = false; break; }
    }
    if (blank) return ESP_ERR_NOT_FOUND;

    app_anim_header_t header;
    memcpy(&header, probe, sizeof(header));

    app_anim_status_t status = app_anim_header_check(&header);
    if (status != APP_ANIM_OK) {
        ESP_LOGW(TAG, "槽位 %d 头部无效: %s", slot, app_anim_status_text(status));
        return ESP_ERR_INVALID_CRC;
    }

    *out = header;
    return ESP_OK;
}

bool app_assets_slot_present(int slot)
{
    app_anim_header_t header;
    return app_assets_slot_header(slot, &header) == ESP_OK;
}

esp_err_t app_assets_slot_erase(int slot)
{
    if (!app_assets_slot_valid(slot)) return ESP_ERR_INVALID_ARG;

    // 只擦头部所在扇区就等价于"清空"：头部一旦不合法，后面的帧数据不会被任何路径引用。
    esp_err_t err = esp_partition_erase_range(s_part, slot_offset(slot), 0x1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除槽位 %d 头部失败: %s", slot, esp_err_to_name(err));
        return err;
    }
    if (s_map_slot_offset == slot_offset(slot)) app_assets_unmap(s_map_handle);
    return ESP_OK;
}

int app_assets_used_slots(void)
{
    if (!s_ready) return 0;

    int used = 0;
    for (int i = 0; i < APP_ANIM_SLOT_MAX; i++) {
        if (app_assets_slot_present(i)) used++;
    }
    return used;
}

// ---------------------------------------------------------------------------
// mmap
// ---------------------------------------------------------------------------

esp_err_t app_assets_map(int slot, const app_anim_header_t *header,
                         const uint8_t **frames, esp_partition_mmap_handle_t *map_handle)
{
    if (!app_assets_slot_valid(slot) || !header || !frames || !map_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_anim_header_check(header) != APP_ANIM_OK) return ESP_ERR_INVALID_CRC;

    uint32_t offset = slot_offset(slot);

    // 已映射同一槽位时直接复用，避免反复占用 MMU 表项。
    if (s_mapped && s_map_slot_offset == offset) {
        s_map_meta = (app_anim_meta_t){ header->width, header->height, header->frame_count };
        *frames = s_map_frames;
        *map_handle = s_map_handle;
        return ESP_OK;
    }

    app_assets_unmap(s_map_handle);

    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(s_part, offset, SLOT_SIZE,
                                       ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "映射槽位 %d 失败: %s", slot, esp_err_to_name(err));
        return err;
    }

    s_mapped = true;
    s_map_handle = handle;
    s_map_frames = (const uint8_t *)ptr + APP_ANIM_HEADER_SIZE;
    s_map_slot_offset = offset;
    s_map_meta = (app_anim_meta_t){ header->width, header->height, header->frame_count };

    *frames = s_map_frames;
    *map_handle = handle;
    return ESP_OK;
}

void app_assets_unmap(esp_partition_mmap_handle_t map_handle)
{
    if (!s_mapped) return;
    (void)map_handle;   // 只维护一份映射，句柄由本模块自己保管
    esp_partition_munmap(s_map_handle);
    s_mapped = false;
    s_map_frames = NULL;
    s_map_slot_offset = 0xFFFFFFFFu;
    memset(&s_map_meta, 0, sizeof(s_map_meta));
}

const uint8_t *app_assets_frame(const app_anim_header_t *header, int index)
{
    if (!s_mapped || !header) return NULL;
    if (index < 0 || index >= s_map_meta.frame_count) return NULL;
    // 几何必须与映射时一致，否则说明调用方拿的是别的槽位的头部。
    if (header->width != s_map_meta.width || header->height != s_map_meta.height ||
        header->frame_count != s_map_meta.frame_count) {
        return NULL;
    }

    uint32_t frame_bytes = app_anim_frame_bytes(header->width, header->height);
    if (frame_bytes == 0) return NULL;
    return s_map_frames + (uint32_t)index * frame_bytes;
}

esp_err_t app_assets_slot_verify(int slot, const app_anim_header_t *header)
{
    if (!app_assets_slot_valid(slot) || !header) return ESP_ERR_INVALID_ARG;
    if (app_anim_header_check(header) != APP_ANIM_OK) return ESP_ERR_INVALID_CRC;

    const uint8_t *frames = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = app_assets_map(slot, header, &frames, &handle);
    if (err != ESP_OK) return err;

    uint32_t crc = app_anim_crc32(frames, header->data_bytes);
    if (crc != header->data_crc) {
        ESP_LOGW(TAG, "槽位 %d 帧数据 CRC 不符: 0x%08lx != 0x%08lx", slot,
                 (unsigned long)crc, (unsigned long)header->data_crc);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 流式写入
// ---------------------------------------------------------------------------

static void writer_reset(app_assets_writer_t *writer)
{
    memset(writer, 0, sizeof(*writer));
    writer->active = false;
}

esp_err_t app_assets_write_begin(app_assets_writer_t *writer, int slot,
                                 const app_anim_meta_t *meta, const char *name,
                                 uint32_t payload_bytes)
{
    if (!writer) return ESP_ERR_INVALID_ARG;
    writer_reset(writer);

    if (!app_assets_slot_valid(slot) || !meta) return ESP_ERR_INVALID_ARG;

    app_anim_status_t status = app_anim_validate(meta->width, meta->height,
                                                 meta->frame_count, payload_bytes);
    if (status != APP_ANIM_OK) {
        ESP_LOGW(TAG, "上传参数被拒: %s", app_anim_status_text(status));
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t need = APP_ANIM_HEADER_SIZE + payload_bytes;
    if (need > SLOT_SIZE) return ESP_ERR_INVALID_SIZE;

    // 先擦头部扇区：这一步之后槽位立即表现为"空"，之后任何失败都不会留下半个动图。
    esp_err_t err = esp_partition_erase_range(s_part, slot_offset(slot), round_up_sector(need));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除槽位 %d 失败: %s", slot, esp_err_to_name(err));
        return err;
    }

    if (s_map_slot_offset == slot_offset(slot)) app_assets_unmap(s_map_handle);

    writer->slot = slot;
    writer->meta = *meta;
    writer->expected = payload_bytes;
    writer->written = 0;
    writer->crc = app_anim_crc32_begin();
    writer->active = true;

    if (name) {
        // 复用头部打包时同样的截断规则，保证名称与头部里的最终结果一致。
        app_anim_header_t tmp;
        app_anim_header_pack(&tmp, meta, name, 0, payload_bytes);
        app_anim_name_copy(&tmp, writer->name, sizeof(writer->name));
    } else {
        writer->name[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t app_assets_write_chunk(app_assets_writer_t *writer, const void *data, size_t len)
{
    if (!writer || !writer->active) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;
    if (!data) return ESP_ERR_INVALID_ARG;
    if (writer->written + len > writer->expected) {
        ESP_LOGW(TAG, "上传数据超出声明长度 %u", (unsigned)writer->expected);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t offset = slot_offset(writer->slot) + APP_ANIM_HEADER_SIZE + writer->written;
    esp_err_t err = esp_partition_write(s_part, offset, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入槽位 %d 偏移 %u 失败: %s", writer->slot, (unsigned)offset,
                 esp_err_to_name(err));
        return err;
    }

    writer->crc = app_anim_crc32_update(writer->crc, data, len);
    writer->written += (uint32_t)len;
    return ESP_OK;
}

esp_err_t app_assets_write_commit(app_assets_writer_t *writer)
{
    if (!writer || !writer->active) return ESP_ERR_INVALID_STATE;
    if (writer->written != writer->expected) {
        ESP_LOGW(TAG, "上传未收齐: %u / %u", (unsigned)writer->written,
                 (unsigned)writer->expected);
        return ESP_ERR_INVALID_SIZE;
    }

    app_anim_header_t header;
    app_anim_header_pack(&header, &writer->meta, writer->name,
                         app_anim_crc32_finish(writer->crc), writer->expected);
    if (app_anim_header_check(&header) != APP_ANIM_OK) return ESP_ERR_INVALID_ARG;

    // 头部最后写：它是"这段动图完整可用"的提交标记。
    esp_err_t err = esp_partition_write(s_part, slot_offset(writer->slot), &header,
                                        sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写槽位 %d 头部失败: %s", writer->slot, esp_err_to_name(err));
        return err;
    }

    int slot = writer->slot;
    writer_reset(writer);

    // 回读校验：闪存写错不会返回错误，只能靠读回来发现。
    app_anim_header_t readback;
    err = app_assets_slot_header(slot, &readback);
    if (err != ESP_OK) return err;
    if (readback.data_crc != header.data_crc || readback.data_bytes != header.data_bytes) {
        return ESP_ERR_INVALID_CRC;
    }
    return app_assets_slot_verify(slot, &readback);
}

void app_assets_write_abort(app_assets_writer_t *writer)
{
    if (!writer) return;
    if (writer->active) {
        // 擦掉头部，槽位回到"空"。帧数据不再被引用，无需一并擦除。
        esp_partition_erase_range(s_part, slot_offset(writer->slot), 0x1000);
    }
    writer_reset(writer);
}
