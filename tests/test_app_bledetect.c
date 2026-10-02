// tests/test_app_bledetect.c —— app_bledetect 的主机侧单元测试。
//
// 覆盖：正常环境、AirTag / Find My、Apple 连续广播轰炸（两种已知前缀）、滑动窗口衰减、
// 样例列表与结论文案。输入完全确定性（地址与时间戳由测试给定），不依赖任何 ESP-IDF / LVGL，
// 只依赖标准库。由 tools/validate.sh --static 编译执行。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_bledetect.h"

// 构造一条观察：addr 为 6 字节；mfg 指向厂商数据（含 2 字节小端厂商 ID 在前）；svc16 为服务
// UUID 列表。其余标识按参数填入。now_ms 由调用方控制，用于复现滑动窗口行为。
static app_bledetect_ad_t make_ad(const uint8_t addr[6], uint16_t company_id,
                                  const uint8_t *mfg, uint8_t mfg_len,
                                  const uint16_t *svc16, int svc16_count,
                                  uint64_t now_ms)
{
    app_bledetect_ad_t ad;
    memset(&ad, 0, sizeof(ad));
    ad.addr = addr;
    ad.company_id = company_id;
    ad.mfg_data = mfg;
    ad.mfg_data_len = mfg_len;
    ad.svc16 = svc16;
    ad.svc16_count = svc16_count;
    ad.now_ms = now_ms;
    return ad;
}

// 生成第 i 台设备的 6 字节地址（保证互不相同，便于测试"不同广播源"计数）。
static void addr_of(int i, uint8_t out[6])
{
    out[0] = 0xAA; out[1] = 0xBB; out[2] = 0xCC;
    out[3] = 0; out[4] = 0; out[5] = (uint8_t)(i & 0xFF);
}

int main(void)
{
    uint8_t addr[6];

    // ---- 1) 正常：几台非 Apple 的普通广播源，不应报轰炸也不应报 AirTag ----
    app_bledetect_reset();
    for (int i = 0; i < 3; i++) {
        addr_of(i, addr);
        // 厂商 ID 随便写一个非 Apple 的值，且厂商数据不匹配任何已知前缀。
        uint8_t mfg[] = { 0x06, 0x00, 0x01, 0x02 };
        app_bledetect_ad_t ad = make_ad(addr, 0x0006, mfg, sizeof(mfg), NULL, 0,
                                        (uint64_t)(i * 100));
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.verdict == APP_BLEDETECT_NORMAL);
        assert(r.total_advertisers == 3);
        assert(r.apple_continuity_spam == 0);
        assert(r.airtag == 0);
        assert(r.other == 3);
    }

    // ---- 2) AirTag / Find My：Apple 厂商 ID + Find My 服务数据 12 19 ----
    app_bledetect_reset();
    addr_of(0, addr);
    {
        uint8_t mfg[] = { 0x4C, 0x00, 0x12, 0x19 };
        app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0, 0);
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.verdict == APP_BLEDETECT_AIRTAG);
        assert(r.airtag == 1);
        assert(r.apple_continuity_spam == 0);
        assert(r.other == 0);
    }

    // ---- 2b) AirTag / Find My：广播了 Find My 网络服务 UUID 0xFD5A（无厂商数据也能认）----
    app_bledetect_reset();
    addr_of(1, addr);
    {
        uint16_t svc[] = { 0xFD5A };
        // 厂商数据给空，仅靠服务 UUID 识别。
        app_bledetect_ad_t ad = make_ad(addr, 0x0000, NULL, 0, svc, 1, 0);
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.verdict == APP_BLEDETECT_AIRTAG);
        assert(r.airtag == 1);
    }

    // ---- 2c) AirTag：07 19 配对模式（同样是 Apple 前缀）----
    app_bledetect_reset();
    addr_of(2, addr);
    {
        uint8_t mfg[] = { 0x4C, 0x00, 0x07, 0x19 };
        app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0, 0);
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.verdict == APP_BLEDETECT_AIRTAG);
        assert(r.airtag == 1);
    }

    // ---- 3) 苹果连续广播轰炸（前缀 A：4C 00 07 19 07）—— 10 台不同源，超过阈值 ----
    app_bledetect_reset();
    for (int i = 0; i < 10; i++) {
        addr_of(i, addr);
        uint8_t mfg[] = { 0x4C, 0x00, 0x07, 0x19, 0x07, 0x00, 0x00 };
        app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0,
                                        (uint64_t)(i * 100));
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.apple_continuity_spam == 10);
        assert(r.verdict == APP_BLEDETECT_SPAM);
        // 样例列表应至少给出一条，且类别标记为"疑似轰炸"。
        assert(r.example_count >= 1);
        assert(r.example_count <= APP_BLEDETECT_EG_MAX);
        int spam_eg = 0;
        for (int i = 0; i < r.example_count; i++) {
            if (r.eg_kind[i] == APP_BLEDETECT_KIND_SPAM) spam_eg++;
        }
        assert(spam_eg >= 1);
    }

    // ---- 3b) 苹果连续广播轰炸（前缀 B：4C 00 04 04 2a 00）同样命中 ----
    app_bledetect_reset();
    for (int i = 0; i < 8; i++) {
        addr_of(i, addr);
        uint8_t mfg[] = { 0x4C, 0x00, 0x04, 0x04, 0x2A, 0x00 };
        app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0,
                                        (uint64_t)(i * 100));
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.apple_continuity_spam == 8);
        assert(r.verdict == APP_BLEDETECT_SPAM);
    }

    // ---- 3c) 数量不足阈值不应误报：仅 3 台前缀 A 源，计数对但结论仍是正常 ----
    app_bledetect_reset();
    for (int i = 0; i < 3; i++) {
        addr_of(i, addr);
        uint8_t mfg[] = { 0x4C, 0x00, 0x07, 0x19, 0x07 };
        app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0,
                                        (uint64_t)(i * 100));
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.apple_continuity_spam == 3);
        assert(r.verdict == APP_BLEDETECT_NORMAL);
    }

    // ---- 4) 滑动窗口衰减：把上面的 10 台炸源喂在 t=0 附近，再把时间推过窗口，应回落 ----
    app_bledetect_reset();
    for (int i = 0; i < 10; i++) {
        addr_of(i, addr);
        uint8_t mfg[] = { 0x4C, 0x00, 0x07, 0x19, 0x07 };
        app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0,
                                        (uint64_t)(i * 100));
        app_bledetect_feed(&ad);
    }
    // 喂一条"现在"的普通广播（t=6000ms），把窗口前沿推过 5000ms，旧炸源应被淘汰。
    addr_of(99, addr);
    {
        uint8_t mfg[] = { 0x06, 0x00, 0x01 };
        app_bledetect_ad_t ad = make_ad(addr, 0x0006, mfg, sizeof(mfg), NULL, 0, 6000);
        app_bledetect_feed(&ad);
    }
    {
        app_bledetect_report_t r;
        app_bledetect_report(&r);
        assert(r.apple_continuity_spam == 0);   // 旧炸源已过期
        assert(r.verdict == APP_BLEDETECT_NORMAL);
        assert(r.other == 1);                    // 仅剩 t=6000 的那台
    }

    // ---- 5) 结论与类别文案 ----
    assert(strcmp(app_bledetect_verdict_text(APP_BLEDETECT_NORMAL), "正常") == 0);
    assert(strcmp(app_bledetect_verdict_text(APP_BLEDETECT_SPAM),
                  "检测到疑似 BLE 广播轰炸") == 0);
    assert(strcmp(app_bledetect_verdict_text(APP_BLEDETECT_AIRTAG),
                  "发现 AirTag / Find My 设备") == 0);
    assert(strcmp(app_bledetect_kind_text(APP_BLEDETECT_KIND_AIRTAG), "AirTag") == 0);
    assert(strcmp(app_bledetect_kind_text(APP_BLEDETECT_KIND_SPAM), "疑似轰炸") == 0);
    assert(strcmp(app_bledetect_kind_text(APP_BLEDETECT_KIND_OTHER), "其它") == 0);

    // ---- 6) 确定性：相同输入两次得到相同结论 ----
    {
        app_bledetect_reset();
        for (int i = 0; i < 10; i++) {
            addr_of(i, addr);
            uint8_t mfg[] = { 0x4C, 0x00, 0x07, 0x19, 0x07 };
            app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0,
                                            (uint64_t)(i * 50));
            app_bledetect_feed(&ad);
        }
        app_bledetect_report_t r1;
        app_bledetect_report(&r1);

        app_bledetect_reset();
        for (int i = 0; i < 10; i++) {
            addr_of(i, addr);
            uint8_t mfg[] = { 0x4C, 0x00, 0x07, 0x19, 0x07 };
            app_bledetect_ad_t ad = make_ad(addr, 0x004C, mfg, sizeof(mfg), NULL, 0,
                                            (uint64_t)(i * 50));
            app_bledetect_feed(&ad);
        }
        app_bledetect_report_t r2;
        app_bledetect_report(&r2);

        assert(r1.verdict == r2.verdict);
        assert(r1.apple_continuity_spam == r2.apple_continuity_spam);
    }

    printf("app_bledetect: all asserts passed\n");
    return 0;
}
