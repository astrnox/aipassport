// main/logic/app_anim.c —— 名片动图数据模型的实现（与硬件无关）。
#include "app_anim.h"
#include "app_text.h"

#include <string.h>

uint32_t app_anim_frame_bytes(int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    return (uint32_t)width * (uint32_t)height * 2u;
}

uint32_t app_anim_data_bytes(int width, int height, int frame_count)
{
    if (frame_count <= 0) return 0;
    return app_anim_frame_bytes(width, height) * (uint32_t)frame_count;
}

app_anim_status_t app_anim_validate(int width, int height, int frame_count,
                                    uint32_t payload_bytes)
{
    if (frame_count <= 0) return APP_ANIM_ERR_EMPTY;
    if (frame_count > APP_ANIM_MAX_FRAMES) return APP_ANIM_ERR_FRAMES;
    if (width <= 0 || height <= 0) return APP_ANIM_ERR_SIDE;
    if (width > APP_ANIM_MAX_SIDE || height > APP_ANIM_MAX_SIDE) return APP_ANIM_ERR_SIDE;

    uint32_t expected = app_anim_data_bytes(width, height, frame_count);
    if (payload_bytes != expected) return APP_ANIM_ERR_BYTES;
    return APP_ANIM_OK;
}

uint32_t app_anim_crc32_begin(void)
{
    return 0xFFFFFFFFu;
}

uint32_t app_anim_crc32_update(uint32_t state, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (!bytes) return state;
    for (size_t i = 0; i < len; i++) {
        state ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            // 反射多项式 0xEDB88320；逐位代替查表，省掉 1KB 常量表。
            uint32_t mask = (uint32_t)(-(int32_t)(state & 1u));
            state = (state >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return state;
}

uint32_t app_anim_crc32_finish(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

uint32_t app_anim_crc32(const void *data, size_t len)
{
    if (!data && len) return 0;
    return app_anim_crc32_finish(app_anim_crc32_update(app_anim_crc32_begin(), data, len));
}

void app_anim_header_pack(app_anim_header_t *header, const app_anim_meta_t *meta,
                          const char *name, uint32_t data_crc, uint32_t data_bytes)
{
    if (!header) return;

    memset(header, 0, sizeof(*header));
    header->magic = APP_ANIM_MAGIC;
    header->data_crc = data_crc;
    header->data_bytes = data_bytes;
    header->version = APP_ANIM_VERSION;

    if (meta) {
        header->width = meta->width;
        header->height = meta->height;
        header->frame_count = meta->frame_count;
        header->frame_bytes = (uint16_t)app_anim_frame_bytes(meta->width, meta->height);
        header->frame_ms = app_anim_frame_ms_clamp(meta->frame_ms);
    }

    if (name) {
        // 名字是给人看的，截到 NUL 前，并保证不在汉字中间断开。
        app_utf8_copy_prefix(name, APP_ANIM_NAME_LEN / 3, header->name,
                             sizeof(header->name));
    }

    header->header_crc = app_anim_crc32(header, offsetof(app_anim_header_t, header_crc));
}

app_anim_status_t app_anim_header_check(const app_anim_header_t *header)
{
    if (!header) return APP_ANIM_ERR_HEADER;
    if (header->magic != APP_ANIM_MAGIC) return APP_ANIM_ERR_HEADER;
    if (header->version != APP_ANIM_VERSION) return APP_ANIM_ERR_HEADER;

    uint32_t expect_crc = app_anim_crc32(header, offsetof(app_anim_header_t, header_crc));
    if (header->header_crc != expect_crc) return APP_ANIM_ERR_HEADER;

    if (header->frame_count == 0) return APP_ANIM_ERR_EMPTY;
    if (header->frame_count > APP_ANIM_MAX_FRAMES) return APP_ANIM_ERR_FRAMES;
    if (header->width == 0 || header->height == 0) return APP_ANIM_ERR_SIDE;
    if (header->width > APP_ANIM_MAX_SIDE || header->height > APP_ANIM_MAX_SIDE) {
        return APP_ANIM_ERR_SIDE;
    }

    // 三个长度字段互为冗余，任何一个对不上都说明写入被中断或数据被改写。
    uint32_t frame_bytes = app_anim_frame_bytes(header->width, header->height);
    if (frame_bytes == 0 || frame_bytes > 0xFFFFu) return APP_ANIM_ERR_BYTES;
    if (header->frame_bytes != (uint16_t)frame_bytes) return APP_ANIM_ERR_BYTES;
    if (header->data_bytes != frame_bytes * (uint32_t)header->frame_count) {
        return APP_ANIM_ERR_BYTES;
    }
    return APP_ANIM_OK;
}

uint16_t app_anim_frame_ms_clamp(int frame_ms)
{
    if (frame_ms <= 0) return APP_ANIM_FRAME_MS_DEFAULT;
    if (frame_ms < APP_ANIM_FRAME_MS_MIN) return APP_ANIM_FRAME_MS_MIN;
    if (frame_ms > APP_ANIM_FRAME_MS_MAX) return APP_ANIM_FRAME_MS_MAX;
    return (uint16_t)frame_ms;
}

uint32_t app_anim_frame_ms_get(const app_anim_header_t *header)
{
    if (!header || header->frame_ms == 0) return APP_ANIM_FRAME_MS_DEFAULT;
    return app_anim_frame_ms_clamp(header->frame_ms);
}

void app_anim_name_copy(const app_anim_header_t *header, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!header) return;

    size_t len = 0;
    while (len < APP_ANIM_NAME_LEN && header->name[len] != '\0') len++;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, header->name, len);
    out[len] = '\0';

    // 闪存里可能残留半个多字节字符；回退到最后一个完整字符边界。
    while (len > 0 && !app_utf8_valid(out)) {
        len--;
        out[len] = '\0';
    }
}

const char *app_anim_status_text(app_anim_status_t status)
{
    switch (status) {
    case APP_ANIM_OK:          return "可用";
    case APP_ANIM_ERR_EMPTY:   return "空槽位";
    case APP_ANIM_ERR_FRAMES:  return "帧数超限";
    case APP_ANIM_ERR_SIDE:    return "尺寸超限";
    case APP_ANIM_ERR_BYTES:   return "数据长度不符";
    case APP_ANIM_ERR_HEADER:  return "头部损坏";
    default:                   return "未知错误";
    }
}
