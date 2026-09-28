// tests/test_app_finder.c —— app_finder 的主机侧单元测试。
//
// 覆盖：设备表的插入/更新/淘汰/超时清理、排序、信号平滑、接近度与人话档位、
// 设备类别的启发式归类与按类别计数、收藏的增删查、以及收藏字节流的序列化往返与损坏拒绝。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_finder.h"

static void addr_of(uint8_t a[6], uint8_t seed)
{
    for (int i = 0; i < 6; i++) a[i] = (uint8_t)(seed + i);
}

int main(void)
{
    app_finder_t f;
    uint8_t a[6], b[6];

    // ---- 初始化 ----
    app_finder_init(&f);
    assert(f.count == 0);
    assert(f.saved_count == 0);
    assert(app_finder_find(&f, a) == -1);

    // ---- 新设备插入并带名字 ----
    addr_of(a, 0x10);
    int i0 = app_finder_feed(&f, a, 0, 0, NULL, 0, "AirPods", 7, -60, 1000);
    assert(i0 == 0);
    assert(f.count == 1);
    assert(f.devs[0].has_name == true);
    assert(strcmp(f.devs[0].name, "AirPods") == 0);
    assert(f.devs[0].rssi == -60);
    assert(f.devs[0].raw_rssi == -60);
    assert(f.devs[0].last_ms == 1000);
    assert(f.devs[0].category == APP_FINDER_CAT_EARBUDS);   // 名字线索命中

    // ---- 同址更新：平滑信号 + 推进时间 ----
    int i1 = app_finder_feed(&f, a, 0, 0, NULL, 0, NULL, 0, -40, 1500);
    assert(i1 == 0);
    assert(f.count == 1);                          // 不重复插入
    assert(f.devs[0].rssi == -55);                 // -60 + (20>>2)
    assert(f.devs[0].raw_rssi == -40);
    assert(f.devs[0].last_ms == 1500);
    assert(strcmp(f.devs[0].name, "AirPods") == 0);   // 本次没带名字，保留旧的
    assert(f.devs[0].category == APP_FINDER_CAT_EARBUDS);   // 本次无线索，类别不降级

    // ---- 从未带过名字的设备 ----
    addr_of(b, 0x20);
    app_finder_feed(&f, b, 0, 0, NULL, 0, NULL, 0, -70, 2000);
    assert(app_finder_find(&f, b) == 1);
    assert(f.devs[1].has_name == false);
    assert(f.devs[1].name[0] == '\0');
    assert(f.devs[1].category == APP_FINDER_CAT_UNKNOWN);
    // 之后补发名字
    app_finder_feed(&f, b, 0, 0, NULL, 0, "MX Master", 9, -70, 2100);
    assert(f.devs[1].has_name == true);
    assert(strcmp(f.devs[1].name, "MX Master") == 0);

    // ---- 超长名字截断且以 NUL 结尾 ----
    uint8_t c[6];
    addr_of(c, 0x30);
    app_finder_feed(&f, c, 0, 0, NULL, 0, "0123456789012345678901234567890", 31, -50, 2200);
    int ic = app_finder_find(&f, c);
    assert(ic >= 0);
    assert(strlen(f.devs[ic].name) == APP_FINDER_NAME_LEN - 1);

    // ---- 清设备但保留收藏 ----
    app_finder_save(&f, a, "AirPods");
    app_finder_clear_devices(&f);
    assert(f.count == 0);
    assert(f.saved_count == 1);

    // ---- 超时清理 ----
    app_finder_init(&f);
    addr_of(a, 0x10);
    addr_of(b, 0x20);
    app_finder_feed(&f, a, 0, 0, NULL, 0, "A", 1, -50, 1000);
    app_finder_feed(&f, b, 0, 0, NULL, 0, "B", 1, -60, 9000);
    app_finder_prune(&f, 9000 + APP_FINDER_TTL_MS + 1);   // a 早已超时，b 刚超
    assert(f.count == 0);
    app_finder_init(&f);
    app_finder_feed(&f, a, 0, 0, NULL, 0, "A", 1, -50, 1000);
    app_finder_feed(&f, b, 0, 0, NULL, 0, "B", 1, -60, 9000);
    app_finder_prune(&f, 9000);                           // b 恰好在有效期内
    assert(f.count == 1);
    assert(app_finder_find(&f, b) == 0);

    // ---- 排序：信号从强到弱 ----
    app_finder_init(&f);
    uint8_t d1[6], d2[6], d3[6];
    addr_of(d1, 1);
    addr_of(d2, 2);
    addr_of(d3, 3);
    app_finder_feed(&f, d1, 0, 0, NULL, 0, "weak", 4, -90, 10);
    app_finder_feed(&f, d2, 0, 0, NULL, 0, "strong", 6, -40, 10);
    app_finder_feed(&f, d3, 0, 0, NULL, 0, "mid", 3, -65, 10);
    app_finder_sort(&f);
    assert(f.devs[0].rssi == -40);
    assert(f.devs[1].rssi == -65);
    assert(f.devs[2].rssi == -90);

    // ---- 表满时淘汰最久未出现的 ----
    app_finder_init(&f);
    uint8_t e[6];
    for (int i = 0; i < APP_FINDER_MAX; i++) {
        addr_of(e, (uint8_t)(0x40 + i));
        app_finder_feed(&f, e, 0, 0, NULL, 0, NULL, 0, -50, (uint64_t)(100 + i));
    }
    assert(f.count == APP_FINDER_MAX);
    uint8_t oldest[6];
    addr_of(oldest, 0x40);                 // 最早插入、last_ms 最小
    assert(app_finder_find(&f, oldest) == 0);
    addr_of(e, 0x99);
    int idx = app_finder_feed(&f, e, 0, 0, NULL, 0, "new", 3, -45, 9999);
    assert(idx >= 0);
    assert(f.count == APP_FINDER_MAX);
    assert(app_finder_find(&f, oldest) == -1);   // 被淘汰
    assert(app_finder_find(&f, e) >= 0);

    // ---- 接近度与人话档位 ----
    assert(app_finder_closeness(-40) == 100);    // 贴上
    assert(app_finder_closeness(-100) == 0);     // 快没了
    assert(app_finder_closeness(-70) == 50);     // 区间中点
    assert(app_finder_closeness(0) == 100);      // 强于上限也封顶
    assert(app_finder_closeness(-120) == 0);     // 弱于下限也封底
    assert(strcmp(app_finder_level(100), "就在附近") == 0);
    assert(strcmp(app_finder_level(80), "就在附近") == 0);
    assert(strcmp(app_finder_level(79), "很近") == 0);
    assert(strcmp(app_finder_level(55), "很近") == 0);
    assert(strcmp(app_finder_level(54), "不太远") == 0);
    assert(strcmp(app_finder_level(30), "不太远") == 0);
    assert(strcmp(app_finder_level(29), "有点远") == 0);
    assert(strcmp(app_finder_level(12), "有点远") == 0);
    assert(strcmp(app_finder_level(11), "信号很弱") == 0);
    assert(strcmp(app_finder_level(0), "信号很弱") == 0);

    // ---- 设备类别：空输入与未知 ----
    assert(app_finder_classify(0, 0, NULL, 0, NULL, 0) == APP_FINDER_CAT_UNKNOWN);
    assert(app_finder_classify(0, 0, NULL, 0, "", 0) == APP_FINDER_CAT_UNKNOWN);
    assert(app_finder_classify(0xFFFF, 0, NULL, 0, NULL, 0) == APP_FINDER_CAT_UNKNOWN);   // 未登记厂商

    // ---- 设备类别：厂商 ID 命中 ----
    assert(app_finder_classify(0x004C, 0, NULL, 0, NULL, 0) == APP_FINDER_CAT_PHONE);     // Apple
    assert(app_finder_classify(0x0075, 0, NULL, 0, NULL, 0) == APP_FINDER_CAT_PHONE);     // Samsung
    assert(app_finder_classify(0x00E0, 0, NULL, 0, NULL, 0) == APP_FINDER_CAT_PHONE);     // Google
    assert(app_finder_classify(0x0006, 0, NULL, 0, NULL, 0) == APP_FINDER_CAT_COMPUTER);  // Microsoft

    // ---- 设备类别：标准服务 UUID ----
    const uint16_t u_fastpair[] = {0xFE2C};
    const uint16_t u_heart[]    = {0x180F, 0x180D};   // 含电池 + 心率，心率应胜出
    const uint16_t u_hid[]      = {0x1812};
    const uint16_t u_batt[]     = {0x180F};           // 仅电池：过于通用，不定性
    const uint16_t u_tile[]     = {0xFEED};           // Tile 防丢器专用服务
    const uint16_t u_smarttag[] = {0xFD5A};           // 三星 SmartTag 专用服务
    assert(app_finder_classify(0, 0, u_fastpair, 1, NULL, 0) == APP_FINDER_CAT_EARBUDS);
    assert(app_finder_classify(0, 0, u_heart, 2, NULL, 0) == APP_FINDER_CAT_WATCH);
    assert(app_finder_classify(0, 0, u_hid, 1, NULL, 0) == APP_FINDER_CAT_OTHER);
    assert(app_finder_classify(0, 0, u_batt, 1, NULL, 0) == APP_FINDER_CAT_UNKNOWN);
    // 防丢器专用 UUID：命中即归为追踪器（这是本轮的追踪器探测线索之一）。
    assert(app_finder_classify(0, 0, u_tile, 1, NULL, 0) == APP_FINDER_CAT_TRACKER);
    assert(app_finder_classify(0, 0, u_smarttag, 1, NULL, 0) == APP_FINDER_CAT_TRACKER);
    // 厂商 ID 与 UUID 同时出现：更具体的 UUID 应优先（Apple 厂商 + Fast Pair → 耳机）。
    assert(app_finder_classify(0x004C, 0, u_fastpair, 1, NULL, 0) == APP_FINDER_CAT_EARBUDS);

    // ---- 设备类别：Apple 厂商数据的类型字节细分 ----
    // 0x12 表示设备正在用 Find My 网络广播：AirTag 与第三方 Find My 防丢器都这么做。
    assert(app_finder_classify(0x004C, 0x12, NULL, 0, NULL, 0) == APP_FINDER_CAT_TRACKER);
    // 0x07 是 AirPods 一类的音频配件。
    assert(app_finder_classify(0x004C, 0x07, NULL, 0, NULL, 0) == APP_FINDER_CAT_EARBUDS);
    // 其余 Apple 广播仍按概率归为手机。
    assert(app_finder_classify(0x004C, 0x10, NULL, 0, NULL, 0) == APP_FINDER_CAT_PHONE);

    // ---- 设备类别：名称线索（大小写无关、中英混排） ----
    assert(app_finder_classify(0, 0, NULL, 0, "AirPods Pro", 11) == APP_FINDER_CAT_EARBUDS);
    assert(app_finder_classify(0, 0, NULL, 0, "WH-1000XM4 headphone", 20) == APP_FINDER_CAT_EARBUDS);
    assert(app_finder_classify(0, 0, NULL, 0, "JBL Flip 6", 10) == APP_FINDER_CAT_AUDIO);
    assert(app_finder_classify(0, 0, NULL, 0, "Galaxy Watch5", 13) == APP_FINDER_CAT_WATCH);
    assert(app_finder_classify(0, 0, NULL, 0, "SmartTag2", 9) == APP_FINDER_CAT_TRACKER);
    assert(app_finder_classify(0, 0, NULL, 0, "MacBook Pro", 11) == APP_FINDER_CAT_COMPUTER);
    assert(app_finder_classify(0, 0, NULL, 0, "我的耳机", 12) == APP_FINDER_CAT_EARBUDS);
    // 名称线索优先于厂商 ID：Apple 厂商但名字是音箱 → 音频。
    assert(app_finder_classify(0x004C, 0, NULL, 0, "JBL Flip", 8) == APP_FINDER_CAT_AUDIO);

    // ---- 类别标签 ----
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_UNKNOWN), "未知") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_PHONE), "手机") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_EARBUDS), "耳机") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_WATCH), "手表") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_TRACKER), "追踪器") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_AUDIO), "音频") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_COMPUTER), "电脑") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_OTHER), "其它") == 0);
    assert(strcmp(app_finder_category_text(APP_FINDER_CAT_COUNT), "未知") == 0);   // 越界兜底

    // ---- feed 记录类别，并在后续广播中更新 ----
    app_finder_init(&f);
    addr_of(a, 0x10);
    app_finder_feed(&f, a, 0x004C, 0, NULL, 0, NULL, 0, -50, 1000);   // 仅厂商 → 手机
    assert(f.devs[0].category == APP_FINDER_CAT_PHONE);
    app_finder_feed(&f, a, 0, 0, u_fastpair, 1, NULL, 0, -50, 1100);  // 后续广播带 Fast Pair → 耳机
    assert(f.devs[0].category == APP_FINDER_CAT_EARBUDS);
    app_finder_feed(&f, a, 0, 0, NULL, 0, NULL, 0, -50, 1200);        // 再后来无线索，不降级
    assert(f.devs[0].category == APP_FINDER_CAT_EARBUDS);

    // ---- 按类别计数 ----
    app_finder_init(&f);
    int counts[APP_FINDER_CAT_COUNT];
    app_finder_category_counts(&f, counts);                       // 空表：全 0
    for (int i = 0; i < APP_FINDER_CAT_COUNT; i++) assert(counts[i] == 0);

    addr_of(a, 0x10);
    app_finder_feed(&f, a, 0x004C, 0, NULL, 0, NULL, 0, -50, 1000);           // 手机
    addr_of(b, 0x20);
    app_finder_feed(&f, b, 0, 0, u_fastpair, 1, NULL, 0, -55, 1000);          // 耳机
    uint8_t e2[6];
    addr_of(e2, 0x30);
    app_finder_feed(&f, e2, 0, 0, u_heart, 2, NULL, 0, -60, 1000);            // 手表
    uint8_t e3[6];
    addr_of(e3, 0x40);
    app_finder_feed(&f, e3, 0, 0, u_hid, 1, NULL, 0, -65, 1000);              // 其它
    uint8_t e4[6];
    addr_of(e4, 0x50);
    app_finder_feed(&f, e4, 0, 0, NULL, 0, NULL, 0, -70, 1000);               // 未知
    uint8_t e5[6];
    addr_of(e5, 0x60);
    app_finder_feed(&f, e5, 0, 0, NULL, 0, NULL, 0, -70, 1000);               // 未知

    app_finder_category_counts(&f, counts);
    assert(counts[APP_FINDER_CAT_PHONE] == 1);
    assert(counts[APP_FINDER_CAT_EARBUDS] == 1);
    assert(counts[APP_FINDER_CAT_WATCH] == 1);
    assert(counts[APP_FINDER_CAT_OTHER] == 1);
    assert(counts[APP_FINDER_CAT_UNKNOWN] == 2);
    assert(counts[APP_FINDER_CAT_TRACKER] == 0);
    assert(counts[APP_FINDER_CAT_AUDIO] == 0);
    assert(counts[APP_FINDER_CAT_COMPUTER] == 0);

    // NULL 入参不应崩溃，且把输出清零。
    app_finder_category_counts(NULL, counts);
    for (int i = 0; i < APP_FINDER_CAT_COUNT; i++) assert(counts[i] == 0);

    // ---- 收藏增删查 ----
    app_finder_init(&f);
    addr_of(a, 0x10);
    addr_of(b, 0x20);
    assert(app_finder_is_saved(&f, a) == false);
    assert(app_finder_save(&f, a, "耳机") == 0);
    assert(app_finder_save(&f, b, "鼠标") == 1);
    assert(app_finder_saved_count(&f) == 2);
    assert(app_finder_is_saved(&f, a) == true);
    assert(app_finder_saved_index(&f, b) == 1);
    assert(app_finder_save(&f, a, "耳机2") == 0);     // 重复保存返回原下标
    assert(app_finder_saved_count(&f) == 2);
    char nm[APP_FINDER_NAME_LEN];
    uint8_t got[6];
    assert(app_finder_saved_at(&f, 0, got, nm, sizeof(nm)) == true);
    assert(memcmp(got, a, 6) == 0);
    assert(strcmp(nm, "耳机2") == 0);
    assert(app_finder_saved_at(&f, 2, got, nm, sizeof(nm)) == false);
    assert(app_finder_save(&f, a, NULL) == 0);        // 无名字时保留旧名
    app_finder_saved_at(&f, 0, NULL, nm, sizeof(nm));
    assert(strcmp(nm, "耳机2") == 0);

    assert(app_finder_unsave(&f, a) == true);
    assert(app_finder_saved_count(&f) == 1);
    assert(app_finder_is_saved(&f, a) == false);
    assert(app_finder_unsave(&f, a) == false);        // 再删无效
    assert(app_finder_saved_index(&f, b) == 0);       // 删除后前移

    // ---- 收藏装满 ----
    app_finder_init(&f);
    for (int i = 0; i < APP_FINDER_SAVED_MAX; i++) {
        addr_of(e, (uint8_t)(0x50 + i));
        assert(app_finder_save(&f, e, NULL) == i);
    }
    addr_of(e, 0xEE);
    assert(app_finder_save(&f, e, NULL) == -1);       // 已满

    // ---- 序列化往返 ----
    app_finder_init(&f);
    addr_of(a, 0x10);
    addr_of(b, 0x20);
    app_finder_save(&f, a, "耳机");
    app_finder_save(&f, b, "鼠标");
    uint8_t buf[256];
    size_t n = app_finder_saved_serialize(&f, buf, sizeof(buf));
    assert(n > 0);
    assert(app_finder_saved_serialize(&f, buf, 3) == 0);   // 缓冲不足

    app_finder_t f2;
    app_finder_init(&f2);
    assert(app_finder_saved_deserialize(&f2, buf, n) == true);
    assert(f2.saved_count == 2);
    assert(app_finder_saved_at(&f2, 0, NULL, nm, sizeof(nm)));
    assert(strcmp(nm, "耳机") == 0);
    assert(app_finder_saved_index(&f2, b) == 1);

    // ---- 损坏数据被拒绝，且不破坏已有收藏 ----
    uint8_t bad[256];
    memcpy(bad, buf, n);
    bad[0] ^= 0xFF;                                        // 坏 magic
    assert(app_finder_saved_deserialize(&f2, bad, n) == false);
    assert(f2.saved_count == 2);
    memcpy(bad, buf, n);
    bad[1] = 99;                                           // 坏版本
    assert(app_finder_saved_deserialize(&f2, bad, n) == false);
    assert(f2.saved_count == 2);
    assert(app_finder_saved_deserialize(&f2, buf, 2) == false);   // 短于头部

    // ---- 趋势判定：直接构造设备结构，覆盖样本数门槛与阈值边界 ----
    app_finder_dev_t dv;
    memset(&dv, 0, sizeof(dv));
    assert(app_finder_trend(NULL) == APP_FINDER_TREND_UNKNOWN);

    dv.samples = APP_FINDER_TREND_MIN_SAMPLES;
    dv.rssi = -50;
    dv.rssi_slow = -50;                                     // 快慢相等 → 没动
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_STEADY);

    dv.rssi = -40;                                          // 快值比基线高 10 dB → 靠近
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_APPROACHING);
    dv.rssi = -44;                                          // 恰好高 6 dB，达到阈值
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_APPROACHING);
    dv.rssi = -45;                                          // 只高 5 dB，不达阈值
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_STEADY);
    dv.rssi = -56;                                          // 低 6 dB → 远离
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_RECEDING);
    dv.rssi = -60;                                          // 低 10 dB → 远离
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_RECEDING);

    dv.samples = APP_FINDER_TREND_MIN_SAMPLES - 1;          // 样本不足：不下判断
    dv.rssi = -40;
    assert(app_finder_trend(&dv) == APP_FINDER_TREND_UNKNOWN);

    // ---- 趋势文案 ----
    assert(strcmp(app_finder_trend_text(APP_FINDER_TREND_APPROACHING), "在靠近") == 0);
    assert(strcmp(app_finder_trend_text(APP_FINDER_TREND_RECEDING), "在远离") == 0);
    assert(strcmp(app_finder_trend_text(APP_FINDER_TREND_STEADY), "信号平稳") == 0);
    assert(strcmp(app_finder_trend_text(APP_FINDER_TREND_UNKNOWN), "刚发现") == 0);
    assert(strcmp(app_finder_trend_text((app_finder_trend_t)99), "刚发现") == 0);   // 越界兜底

    // ---- 趋势端到端：慢基线滞后于快值，因此"突然变强"能被判成靠近 ----
    app_finder_init(&f);
    addr_of(a, 0x10);
    for (int k = 0; k < 5; k++) {
        app_finder_feed(&f, a, 0, 0, NULL, 0, NULL, 0, -60, (uint64_t)(1000 + k * 100));
    }
    int ti = app_finder_find(&f, a);
    assert(ti >= 0);
    assert(app_finder_trend(&f.devs[ti]) == APP_FINDER_TREND_STEADY);   // 一直是 -60，没动
    // 一步跳到 -20：快值 -60+(40>>2)=-50，慢基线 -60+(40>>5)=-59，相差 9 dB → 靠近。
    app_finder_feed(&f, a, 0, 0, NULL, 0, NULL, 0, -20, 2000);
    assert(app_finder_trend(&f.devs[ti]) == APP_FINDER_TREND_APPROACHING);

    printf("test_app_finder: PASS\n");
    return 0;
}