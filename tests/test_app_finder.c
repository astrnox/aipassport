// tests/test_app_finder.c —— app_finder 的主机侧单元测试。
//
// 覆盖：设备表的插入/更新/淘汰/超时清理、排序、信号平滑、接近度与人话档位、
// 收藏的增删查、以及收藏字节流的序列化往返与损坏拒绝。
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
    int i0 = app_finder_feed(&f, a, "AirPods", 7, -60, 1000);
    assert(i0 == 0);
    assert(f.count == 1);
    assert(f.devs[0].has_name == true);
    assert(strcmp(f.devs[0].name, "AirPods") == 0);
    assert(f.devs[0].rssi == -60);
    assert(f.devs[0].raw_rssi == -60);
    assert(f.devs[0].last_ms == 1000);

    // ---- 同址更新：平滑信号 + 推进时间 ----
    int i1 = app_finder_feed(&f, a, NULL, 0, -40, 1500);
    assert(i1 == 0);
    assert(f.count == 1);                          // 不重复插入
    assert(f.devs[0].rssi == -55);                 // -60 + (20>>2)
    assert(f.devs[0].raw_rssi == -40);
    assert(f.devs[0].last_ms == 1500);
    assert(strcmp(f.devs[0].name, "AirPods") == 0);   // 本次没带名字，保留旧的

    // ---- 从未带过名字的设备 ----
    addr_of(b, 0x20);
    app_finder_feed(&f, b, NULL, 0, -70, 2000);
    assert(app_finder_find(&f, b) == 1);
    assert(f.devs[1].has_name == false);
    assert(f.devs[1].name[0] == '\0');
    // 之后补发名字
    app_finder_feed(&f, b, "MX Master", 9, -70, 2100);
    assert(f.devs[1].has_name == true);
    assert(strcmp(f.devs[1].name, "MX Master") == 0);

    // ---- 超长名字截断且以 NUL 结尾 ----
    uint8_t c[6];
    addr_of(c, 0x30);
    app_finder_feed(&f, c, "0123456789012345678901234567890", 31, -50, 2200);
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
    app_finder_feed(&f, a, "A", 1, -50, 1000);
    app_finder_feed(&f, b, "B", 1, -60, 9000);
    app_finder_prune(&f, 9000 + APP_FINDER_TTL_MS + 1);   // a 早已超时，b 刚超
    assert(f.count == 0);
    app_finder_init(&f);
    app_finder_feed(&f, a, "A", 1, -50, 1000);
    app_finder_feed(&f, b, "B", 1, -60, 9000);
    app_finder_prune(&f, 9000);                           // b 恰好在有效期内
    assert(f.count == 1);
    assert(app_finder_find(&f, b) == 0);

    // ---- 排序：信号从强到弱 ----
    app_finder_init(&f);
    uint8_t d1[6], d2[6], d3[6];
    addr_of(d1, 1);
    addr_of(d2, 2);
    addr_of(d3, 3);
    app_finder_feed(&f, d1, "weak", 4, -90, 10);
    app_finder_feed(&f, d2, "strong", 6, -40, 10);
    app_finder_feed(&f, d3, "mid", 3, -65, 10);
    app_finder_sort(&f);
    assert(f.devs[0].rssi == -40);
    assert(f.devs[1].rssi == -65);
    assert(f.devs[2].rssi == -90);

    // ---- 表满时淘汰最久未出现的 ----
    app_finder_init(&f);
    uint8_t e[6];
    for (int i = 0; i < APP_FINDER_MAX; i++) {
        addr_of(e, (uint8_t)(0x40 + i));
        app_finder_feed(&f, e, NULL, 0, -50, (uint64_t)(100 + i));
    }
    assert(f.count == APP_FINDER_MAX);
    uint8_t oldest[6];
    addr_of(oldest, 0x40);                 // 最早插入、last_ms 最小
    assert(app_finder_find(&f, oldest) == 0);
    addr_of(e, 0x99);
    int idx = app_finder_feed(&f, e, "new", 3, -45, 9999);
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

    printf("test_app_finder: PASS\n");
    return 0;
}