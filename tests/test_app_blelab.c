// tests/test_app_blelab.c —— app_blelab 的主机侧单元测试。
//
// 覆盖：四种模式的精确字节布局、容量不足时返回所需长度、非法入参、苹果设备表、
// iBeacon / Swift Pair 的 setter，以及确定性 PRNG（xorshift32）与随机地址的高两位。
//
// 注意：本机开发环境（按任务设定）往往没有 C 编译器，无法本地跑；它会在
// tools/validate.sh 的 --static 流程里被正式编译执行。测试用例只依赖标准库，
// 不碰任何 ESP-IDF / LVGL 符号。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_blelab.h"

static void expect_bytes(const uint8_t *buf, size_t len,
                        const uint8_t *expect, size_t expect_len)
{
    assert(len == expect_len);
    assert(memcmp(buf, expect, expect_len) == 0);
}

int main(void)
{
    uint8_t buf[64];
    size_t  len = 0;
    app_blelab_err_t rc;

    // ---- 苹果音频：Airpods（model 0x02），31 字节，尾部补 0 ----
    rc = app_blelab_build_packet(APP_BLELAB_MODE_APPLE_AUDIO, 0, buf, sizeof(buf), &len);
    assert(rc == APP_BLELAB_OK);
    assert(len == 31);
    {
        uint8_t expect[31] = {
            0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02,
            0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45,
            0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- 苹果设置：AppleTV Setup（model 0x01），23 字节 ----
    rc = app_blelab_build_packet(APP_BLELAB_MODE_APPLE_SETUP, 0, buf, sizeof(buf), &len);
    assert(rc == APP_BLELAB_OK);
    assert(len == 23);
    {
        uint8_t expect[23] = {
            0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00,
            0x00, 0x00, 0x0f, 0x05, 0xc1, 0x01,
            0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        };
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- 苹果设备表数量与个别条目 ----
    assert(app_blelab_apple_count(APP_BLELAB_MODE_APPLE_AUDIO) == 22);
    assert(app_blelab_apple_count(APP_BLELAB_MODE_APPLE_SETUP) == 13);
    const char *nm = NULL;
    uint8_t mid = 0;
    assert(app_blelab_apple_dev(APP_BLELAB_MODE_APPLE_AUDIO, 0, &nm, &mid) == true);
    assert(strcmp(nm, "Airpods") == 0 && mid == 0x02);
    // 最后一个 setup 设备：model 0xc0
    assert(app_blelab_apple_dev(APP_BLELAB_MODE_APPLE_SETUP, 12, &nm, &mid) == true);
    assert(strcmp(nm, "AppleTV Wireless Audio Sync") == 0 && mid == 0xc0);
    // apple_dev 是越界返回 false 的取值函数：audio 表共 22 台（下标 0..21），index 22 越界。
    // 取模回卷发生在 build_packet 内部（index % count），不在 apple_dev。
    assert(app_blelab_apple_dev(APP_BLELAB_MODE_APPLE_AUDIO, 22, &nm, &mid) == false);

    // ---- iBeacon：UUID 全 0xAA，major 0x1234，minor 0x5678，tx -59（0xC5）----
    uint8_t uuid[16];
    memset(uuid, 0xAA, sizeof(uuid));
    app_blelab_set_ibeacon(uuid, 0x1234, 0x5678, -59);
    rc = app_blelab_build_packet(APP_BLELAB_MODE_IBEACON, 0, buf, sizeof(buf), &len);
    assert(rc == APP_BLELAB_OK);
    assert(len == 27);
    {
        uint8_t expect[27] = {
            0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15,
            0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
            0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
            0x12, 0x34, 0x56, 0x78, (uint8_t)(-59),
        };
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- Swift Pair：默认 CoD 0x000400，无名字 ----
    app_blelab_set_swift_name(NULL);   // 清空名字
    app_blelab_set_swift_cod(0x000400);
    rc = app_blelab_build_packet(APP_BLELAB_MODE_SWIFT_PAIR, 0, buf, sizeof(buf), &len);
    assert(rc == APP_BLELAB_OK);
    assert(len == 10);
    {
        uint8_t expect[10] = {0x09, 0xFF, 0x06, 0x00, 0x03, 0x02, 0x80, 0x00, 0x04, 0x00};
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- Swift Pair：自定义 CoD 0x123456 + 名字 "Test" ----
    app_blelab_set_swift_cod(0x123456);
    app_blelab_set_swift_name("Test");
    rc = app_blelab_build_packet(APP_BLELAB_MODE_SWIFT_PAIR, 0, buf, sizeof(buf), &len);
    assert(rc == APP_BLELAB_OK);
    assert(len == 16);   // 10 + 2 + 4
    {
        uint8_t expect[16] = {
            0x09, 0xFF, 0x06, 0x00, 0x03, 0x02, 0x80,
            0x56, 0x34, 0x12,            // CoD 小端
            0x05, 0x09, 'T', 'e', 's', 't',
        };
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- 容量不足：返回所需长度 ----
    uint8_t small[5];
    rc = app_blelab_build_packet(APP_BLELAB_MODE_APPLE_AUDIO, 0, small, sizeof(small), &len);
    assert(rc == APP_BLELAB_ERR_BUF_TOO_SMALL);
    assert(len == 31);   // 需要 31 字节
    // Swift Pair 含名字时所需长度也会变大
    rc = app_blelab_build_packet(APP_BLELAB_MODE_SWIFT_PAIR, 0, small, sizeof(small), &len);
    assert(rc == APP_BLELAB_ERR_BUF_TOO_SMALL);
    assert(len == 16);

    // ---- 非法入参 ----
    rc = app_blelab_build_packet(APP_BLELAB_MODE_APPLE_AUDIO, 0, NULL, 31, &len);
    assert(rc == APP_BLELAB_ERR_INVALID_ARG);
    rc = app_blelab_build_packet(APP_BLELAB_MODE_APPLE_AUDIO, 0, buf, sizeof(buf), NULL);
    assert(rc == APP_BLELAB_ERR_INVALID_ARG);
    rc = app_blelab_build_packet(APP_BLELAB_MODE_COUNT, 0, buf, sizeof(buf), &len);
    assert(rc == APP_BLELAB_ERR_INVALID_MODE);

    // ---- 确定性 PRNG：同 seed → 同序列 ----
    uint32_t s1 = 0x12345678u, s2 = 0x12345678u;
    uint32_t r1a = app_blelab_xorshift32(&s1);
    uint32_t r1b = app_blelab_xorshift32(&s1);
    uint32_t r2a = app_blelab_xorshift32(&s2);
    uint32_t r2b = app_blelab_xorshift32(&s2);
    assert(r1a == r2a);
    assert(r1b == r2b);
    assert(r1a != r1b);   // 序列应当有变化

    // ---- 随机地址：首字节高两位为 11 ----
    uint32_t s3 = 0x9E3779B9u;
    uint8_t addr[6];
    app_blelab_random_addr(&s3, addr);
    assert((addr[0] & 0xF0) == 0xF0);
    uint32_t s4 = 0x9E3779B9u;
    uint8_t addr2[6];
    app_blelab_random_addr(&s4, addr2);
    assert(memcmp(addr, addr2, 6) == 0);   // 同 seed → 同地址

    printf("test_app_blelab: PASS\n");
    return 0;
}
