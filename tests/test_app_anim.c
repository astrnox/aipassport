// tests/test_app_anim.c —— 名片动图数据模型的主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_anim.h"
#include "logic/app_text.h"

static void test_frame_bytes(void)
{
    assert(app_anim_frame_bytes(0, 10) == 0);
    assert(app_anim_frame_bytes(-1, 10) == 0);
    assert(app_anim_frame_bytes(10, 0) == 0);
    assert(app_anim_frame_bytes(1, 1) == 2);
    assert(app_anim_frame_bytes(64, 64) == 8192);
    assert(app_anim_data_bytes(64, 64, 0) == 0);
    assert(app_anim_data_bytes(64, 64, 12) == 8192u * 12u);
}

static void test_validate(void)
{
    assert(app_anim_validate(64, 64, 0, 0) == APP_ANIM_ERR_EMPTY);
    assert(app_anim_validate(64, 64, -3, 0) == APP_ANIM_ERR_EMPTY);
    assert(app_anim_validate(64, 64, APP_ANIM_MAX_FRAMES + 1, 1) == APP_ANIM_ERR_FRAMES);

    assert(app_anim_validate(0, 64, 4, 0) == APP_ANIM_ERR_SIDE);
    assert(app_anim_validate(64, 0, 4, 0) == APP_ANIM_ERR_SIDE);
    assert(app_anim_validate(-8, 64, 4, 0) == APP_ANIM_ERR_SIDE);
    assert(app_anim_validate(APP_ANIM_MAX_SIDE + 1, 64, 4, 0) == APP_ANIM_ERR_SIDE);
    assert(app_anim_validate(64, APP_ANIM_MAX_SIDE + 1, 4, 0) == APP_ANIM_ERR_SIDE);

    // 边界值本身合法。
    assert(app_anim_validate(APP_ANIM_MAX_SIDE, APP_ANIM_MAX_SIDE,
                             APP_ANIM_MAX_FRAMES,
                             app_anim_data_bytes(APP_ANIM_MAX_SIDE, APP_ANIM_MAX_SIDE,
                                                 APP_ANIM_MAX_FRAMES)) == APP_ANIM_OK);
    assert(app_anim_validate(1, 1, 1, 2) == APP_ANIM_OK);

    // 长度对不上（少一字节、多一字节都要拒绝）。
    uint32_t bytes = app_anim_data_bytes(48, 48, 6);
    assert(app_anim_validate(48, 48, 6, bytes) == APP_ANIM_OK);
    assert(app_anim_validate(48, 48, 6, bytes - 1) == APP_ANIM_ERR_BYTES);
    assert(app_anim_validate(48, 48, 6, bytes + 1) == APP_ANIM_ERR_BYTES);
    assert(app_anim_validate(48, 48, 6, 0) == APP_ANIM_ERR_BYTES);
}

static void test_crc32(void)
{
    // 标准向量：空输入为 0，"123456789" 为 0xCBF43926。
    assert(app_anim_crc32(NULL, 0) == 0);
    assert(app_anim_crc32("", 0) == 0);
    assert(app_anim_crc32("123456789", 9) == 0xCBF43926u);
    assert(app_anim_crc32("123456789", 4) != app_anim_crc32("123456789", 9));

    // 单字节翻转必须改变结果。
    uint8_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };
    assert(app_anim_crc32(a, sizeof(a)) != app_anim_crc32(b, sizeof(b)));

    // 增量版本必须与一次算完完全一致，包括任意切分点。
    uint8_t payload[257];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 31 + 7);
    uint32_t whole = app_anim_crc32(payload, sizeof(payload));

    for (size_t cut = 0; cut <= sizeof(payload); cut += 17) {
        uint32_t state = app_anim_crc32_begin();
        state = app_anim_crc32_update(state, payload, cut);
        state = app_anim_crc32_update(state, payload + cut, sizeof(payload) - cut);
        assert(app_anim_crc32_finish(state) == whole);
    }

    // 长度为 0 的分段不改变状态。
    uint32_t state = app_anim_crc32_begin();
    assert(app_anim_crc32_update(state, NULL, 0) == state);
    assert(app_anim_crc32_update(state, payload, 0) == state);
}

static void test_header_pack_check(void)
{
    app_anim_meta_t meta = { .width = 64, .height = 64, .frame_count = 12 };
    uint32_t data_bytes = app_anim_data_bytes(64, 64, 12);

    app_anim_header_t h;
    app_anim_header_pack(&h, &meta, "小猫", 0xDEADBEEFu, data_bytes);

    assert(h.magic == APP_ANIM_MAGIC);
    assert(h.version == APP_ANIM_VERSION);
    assert(h.width == 64 && h.height == 64 && h.frame_count == 12);
    assert(h.frame_bytes == 8192);
    assert(h.data_bytes == data_bytes);
    assert(h.data_crc == 0xDEADBEEFu);
    assert(h.header_crc != 0);

    assert(app_anim_header_check(&h) == APP_ANIM_OK);

    char name[APP_ANIM_NAME_LEN];
    app_anim_name_copy(&h, name, sizeof(name));
    assert(strcmp(name, "小猫") == 0);

    // 每个字段被改写都必须被检出。
    app_anim_header_t bad;

    bad = h; bad.magic = 0;
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_HEADER);

    bad = h; bad.version = APP_ANIM_VERSION + 1;
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_HEADER);

    bad = h; bad.header_crc ^= 1u;
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_HEADER);

    // 重新计算头部 CRC 后，字段级矛盾仍要被长度交叉校验抓到。
    bad = h; bad.frame_bytes = 8190;
    bad.header_crc = app_anim_crc32(&bad, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_BYTES);

    bad = h; bad.data_bytes = data_bytes - 2;
    bad.header_crc = app_anim_crc32(&bad, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_BYTES);

    bad = h; bad.frame_count = 0;
    bad.header_crc = app_anim_crc32(&bad, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_EMPTY);

    bad = h; bad.frame_count = APP_ANIM_MAX_FRAMES + 1;
    bad.data_bytes = app_anim_frame_bytes(64, 64) * (APP_ANIM_MAX_FRAMES + 1);
    bad.header_crc = app_anim_crc32(&bad, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_FRAMES);

    bad = h; bad.width = APP_ANIM_MAX_SIDE + 2;
    bad.header_crc = app_anim_crc32(&bad, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_SIDE);

    bad = h; bad.height = 0;
    bad.header_crc = app_anim_crc32(&bad, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&bad) == APP_ANIM_ERR_SIDE);

    // 全部 0 的闪存（擦除态）必须是"未写入"，不能当成合法槽位。
    app_anim_header_t blank;
    memset(&blank, 0, sizeof(blank));
    assert(app_anim_header_check(&blank) == APP_ANIM_ERR_HEADER);
    assert(app_anim_header_check(NULL) == APP_ANIM_ERR_HEADER);
}

static void test_header_layout(void)
{
    // 帧数据必须落在 4 字节边界上，且头部长度与文档一致。
    assert(sizeof(app_anim_header_t) == APP_ANIM_HEADER_SIZE);
    assert(offsetof(app_anim_header_t, header_crc) == 40);
    assert(offsetof(app_anim_header_t, name) == 24);
    assert(offsetof(app_anim_header_t, name) + APP_ANIM_NAME_LEN == 40);
}

static void test_name_copy(void)
{
    char out[APP_ANIM_NAME_LEN];

    // 没有终止符的头部：按定长拷贝并补 NUL（out 只有 16 字节，留一位给终止符）。
    app_anim_header_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.name, "ABCDEFGHIJKLMNOP", APP_ANIM_NAME_LEN);
    app_anim_name_copy(&h, out, sizeof(out));
    assert(strcmp(out, "ABCDEFGHIJKLMNO") == 0);
    assert(strlen(out) == APP_ANIM_NAME_LEN - 1);

    // 被截断的汉字（半个 3 字节字符）必须回退到完整字符边界。
    memset(&h, 0, sizeof(h));
    memcpy(h.name, "\xE5\xB0\x8F\xE7\x8C\xAB", 6);          // "小猫"
    memcpy(h.name + 6, "\xE7\x8C", 2);                       // 半个"猫"
    app_anim_name_copy(&h, out, sizeof(out));
    assert(app_utf8_valid(out));
    assert(strcmp(out, "小猫") == 0);

    // 小缓冲不越界。
    char small[4];
    memset(&h, 0, sizeof(h));
    memcpy(h.name, "\xE5\xB0\x8F\xE7\x8C\xAB", 6);
    app_anim_name_copy(&h, small, sizeof(small));
    assert(app_utf8_valid(small));

    app_anim_name_copy(NULL, out, sizeof(out));
    assert(out[0] == '\0');
    app_anim_name_copy(&h, NULL, sizeof(out));
    app_anim_name_copy(&h, out, 0);
}

static void test_frame_ms(void)
{
    // 0 / 负数按默认值处理，越界值被夹住。
    assert(app_anim_frame_ms_clamp(0) == APP_ANIM_FRAME_MS_DEFAULT);
    assert(app_anim_frame_ms_clamp(-5) == APP_ANIM_FRAME_MS_DEFAULT);
    assert(app_anim_frame_ms_clamp(1) == APP_ANIM_FRAME_MS_MIN);
    assert(app_anim_frame_ms_clamp(APP_ANIM_FRAME_MS_MIN) == APP_ANIM_FRAME_MS_MIN);
    assert(app_anim_frame_ms_clamp(80) == 80);
    assert(app_anim_frame_ms_clamp(APP_ANIM_FRAME_MS_MAX) == APP_ANIM_FRAME_MS_MAX);
    assert(app_anim_frame_ms_clamp(60000) == APP_ANIM_FRAME_MS_MAX);

    app_anim_meta_t meta = { 32, 32, 4, 80 };
    app_anim_header_t h;
    app_anim_header_pack(&h, &meta, "x", 0, app_anim_data_bytes(32, 32, 4));
    assert(h.frame_ms == 80);
    assert(app_anim_frame_ms_get(&h) == 80);
    assert(app_anim_header_check(&h) == APP_ANIM_OK);

    // 帧间隔只影响播放速度，不参与头部合法性判定，因此改它仍算合法。
    h.frame_ms = 0;
    h.header_crc = app_anim_crc32(&h, offsetof(app_anim_header_t, header_crc));
    assert(app_anim_header_check(&h) == APP_ANIM_OK);
    assert(app_anim_frame_ms_get(&h) == APP_ANIM_FRAME_MS_DEFAULT);

    assert(app_anim_frame_ms_get(NULL) == APP_ANIM_FRAME_MS_DEFAULT);
}

static void test_status_text(void)
{
    assert(strcmp(app_anim_status_text(APP_ANIM_OK), "可用") == 0);
    assert(app_anim_status_text((app_anim_status_t)99) != NULL);
}

int main(void)
{
    test_frame_bytes();
    test_validate();
    test_crc32();
    test_header_pack_check();
    test_header_layout();
    test_name_copy();
    test_frame_ms();
    test_status_text();
    puts("test_app_anim: PASS");
    return 0;
}
