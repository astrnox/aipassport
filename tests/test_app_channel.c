// tests/test_app_channel.c —— app_channel 的主机侧单元测试。
//
// 覆盖：复位、非法信道忽略、强度加权累加与封顶、最强值更新、整体拥挤度与推荐信道、
// 每信道明细（排序）、单信道热点列表（按强度排序 / 隐藏 SSID / 容量上限）、结论文案，
// 以及给普通用户的那句建议。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_channel.h"

// 每信道权重 = (rssi + 100) 夹在 0..60 后折半，即 0..30。
static int weight(int rssi)
{
    int w = rssi + 100;
    if (w < 0) w = 0;
    if (w > 60) w = 60;
    return w / 2;
}

int main(void)
{
    app_channel_report_t r;

    // ---- 复位 ----
    app_channel_reset(&r);
    for (int i = 0; i < APP_CHANNEL_COUNT; i++) {
        assert(r.ch[i].aps == 0);
        assert(r.ch[i].strongest == APP_CHANNEL_NO_RSSI);
        assert(r.ch[i].score == 0);
        assert(r.order[i] == i);            // 默认即信道号升序
    }
    assert(r.stored == 0);
    assert(r.ap_total == 0);
    assert(r.congestion == 0);
    assert(r.best_channel == 1);

    // ---- 非法信道直接忽略 ----
    app_channel_add_ap(&r, 0, -50, "x", 1, NULL);
    app_channel_add_ap(&r, 14, -50, "x", 1, NULL);
    app_channel_add_ap(&r, -1, -50, "x", 1, NULL);
    assert(r.ap_total == 0);
    assert(r.stored == 0);

    // ---- 单信道：按强度累加 ----
    app_channel_add_ap(&r, 6, -50, "Home", 4, NULL);
    assert(r.ch[5].aps == 1);
    assert(r.ch[5].strongest == -50);
    assert(r.ch[5].score == weight(-50));   // 25

    app_channel_add_ap(&r, 6, -40, "Neighbor", 8, NULL);
    assert(r.ch[5].aps == 2);
    assert(r.ch[5].strongest == -40);       // 更强则更新
    assert(r.ch[5].score == weight(-50) + weight(-40));   // 25 + 30 = 55

    app_channel_add_ap(&r, 6, -110, NULL, 0, NULL);        // 弱于现有最强，不更新 strongest
    assert(r.ch[5].strongest == -40);
    assert(r.ch[5].score == weight(-50) + weight(-40) + weight(-110));
    assert(r.ap_total == 3);
    assert(r.stored == 3);

    // ---- finish：封顶与整体拥挤度 ----
    app_channel_finish(&r);
    assert(r.ch[5].score == 55);            // 未到 100，说明不再"两台就顶满"
    assert(r.congestion == 55 / 3);         // (0 + 55 + 0) / 3 = 18
    assert(r.best_channel == 1);            // 1/11 都是 0，取第一个

    // ---- 推荐信道：三个不重叠信道里最空的那条 ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 1, -40, "a", 1, NULL);    // 权重 30
    app_channel_add_ap(&r, 6, -90, "b", 1, NULL);    // 权重 5
    app_channel_add_ap(&r, 11, -70, "c", 1, NULL);   // 权重 15
    app_channel_finish(&r);
    assert(app_channel_score(&r, 1) == 30);
    assert(app_channel_score(&r, 6) == 5);
    assert(app_channel_score(&r, 11) == 15);
    assert(r.congestion == (30 + 5 + 15) / 3);
    assert(r.best_channel == 6);

    // ---- 非 1/6/11 的信道照样统计，但不参与推荐 ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 3, -40, "x", 1, NULL);    // 落在信道 3
    app_channel_finish(&r);
    assert(app_channel_score(&r, 3) == 30);
    assert(r.congestion == 0);              // 1/6/11 上都没有 AP
    assert(r.best_channel == 1);

    // ---- 信道越界查询 ----
    assert(app_channel_score(&r, 0) == 0);
    assert(app_channel_score(&r, 14) == 0);

    // ---- 每信道排序：最空的排最前，同分按信道号升序 ----
    app_channel_reset(&r);
    for (int i = 0; i < 6; i++) app_channel_add_ap(&r, 6, -40, "n", 1, NULL);  // 6 -> 封顶 100
    app_channel_add_ap(&r, 1, -95, "f", 1, NULL);    // 2
    app_channel_add_ap(&r, 11, -95, "f", 1, NULL);   // 2
    app_channel_finish(&r);
    assert(app_channel_score(&r, 6) == 100);
    // 信道 2 是最小的空信道，应排第一；信道 6 最挤，排最后。
    assert(app_channel_order_channel(&r, 0) == 2);
    assert(app_channel_order_channel(&r, APP_CHANNEL_COUNT - 1) == 6);
    assert(app_channel_order_channel(&r, -1) == 0);
    assert(app_channel_order_channel(&r, APP_CHANNEL_COUNT) == 0);
    // 排序结果必须涵盖 1..13 每个信道一次。
    bool seen[APP_CHANNEL_COUNT] = { false };
    for (int i = 0; i < APP_CHANNEL_COUNT; i++) {
        int ch = app_channel_order_channel(&r, i);
        assert(ch >= 1 && ch <= 13);
        assert(!seen[ch - 1]);
        seen[ch - 1] = true;
    }

    // ---- 单信道热点列表：按强度从强到弱，隐藏 SSID 标记正确 ----
    app_channel_reset(&r);
    const uint8_t b1[6] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t b2[6] = { 7, 8, 9, 10, 11, 12 };
    app_channel_add_ap(&r, 6, -70, "Hidden", 0, b1);    // ssid_len 0 => 隐藏
    app_channel_add_ap(&r, 6, -40, "NearAP", 6, b2);
    app_channel_add_ap(&r, 6, -55, "Home", 4, b1);
    app_channel_add_ap(&r, 11, -60, "Other", 5, b2);
    app_channel_finish(&r);

    int idx[APP_CHANNEL_MAX_APS];
    int n = app_channel_aps_on(&r, 6, idx, APP_CHANNEL_MAX_APS);
    assert(n == 3);
    assert(r.aps[idx[0]].rssi == -40);          // 最强在前
    assert(strcmp(r.aps[idx[0]].ssid, "NearAP") == 0);
    assert(r.aps[idx[1]].rssi == -55);
    assert(r.aps[idx[2]].rssi == -70);
    assert(r.aps[idx[2]].hidden);               // 隐藏 SSID
    assert(strcmp(r.aps[idx[2]].ssid, "") == 0);
    assert(memcmp(r.aps[idx[0]].bssid, b2, 6) == 0);

    // 只取指定信道。
    n = app_channel_aps_on(&r, 11, idx, APP_CHANNEL_MAX_APS);
    assert(n == 1);
    assert(r.aps[idx[0]].channel == 11);

    // 空信道返回 0。
    assert(app_channel_aps_on(&r, 3, idx, APP_CHANNEL_MAX_APS) == 0);
    // 非法通道返回 0。
    assert(app_channel_aps_on(&r, 0, idx, APP_CHANNEL_MAX_APS) == 0);

    // max 限制生效。
    assert(app_channel_aps_on(&r, 6, idx, 2) == 2);

    // ---- 明细容量上限：统计照算，明细截断 ----
    app_channel_reset(&r);
    for (int i = 0; i < APP_CHANNEL_MAX_APS + 10; i++) {
        char name[8];
        snprintf(name, sizeof(name), "ap%02d", i);
        app_channel_add_ap(&r, 1, -60, name, (int)strlen(name), NULL);
    }
    app_channel_finish(&r);
    assert(r.stored == APP_CHANNEL_MAX_APS);
    assert(r.ap_total == APP_CHANNEL_MAX_APS + 10);
    assert(app_channel_aps_on(&r, 1, idx, APP_CHANNEL_MAX_APS) == APP_CHANNEL_MAX_APS);

    // ---- 结论文案分档 ----
    assert(strcmp(app_channel_verdict(0), "很通畅") == 0);
    assert(strcmp(app_channel_verdict(19), "很通畅") == 0);
    assert(strcmp(app_channel_verdict(20), "还行") == 0);
    assert(strcmp(app_channel_verdict(44), "还行") == 0);
    assert(strcmp(app_channel_verdict(45), "比较挤") == 0);
    assert(strcmp(app_channel_verdict(69), "比较挤") == 0);
    assert(strcmp(app_channel_verdict(70), "非常拥挤") == 0);
    assert(strcmp(app_channel_verdict(100), "非常拥挤") == 0);

    // ---- 给普通用户的建议 ----
    char advice[96];
    app_channel_reset(&r);
    app_channel_finish(&r);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "没扫到") != NULL);

    app_channel_reset(&r);
    app_channel_add_ap(&r, 6, -100, "w", 1, NULL);   // 单个很弱的 AP：拥挤度低
    app_channel_finish(&r);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "很通畅") != NULL);

    app_channel_reset(&r);
    for (int i = 0; i < 6; i++) app_channel_add_ap(&r, 6, -40, "n", 1, NULL);  // 信道 6 挤爆
    app_channel_add_ap(&r, 1, -95, "f", 1, NULL);
    app_channel_add_ap(&r, 11, -95, "f", 1, NULL);
    app_channel_finish(&r);
    assert(r.congestion >= 20);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "信道") != NULL);
    assert(strstr(advice, "1 信道") != NULL || strstr(advice, "11 信道") != NULL);

    // ---- 空指针安全 ----
    app_channel_reset(NULL);
    app_channel_add_ap(NULL, 6, -50, "x", 1, NULL);
    app_channel_finish(NULL);
    assert(app_channel_score(NULL, 6) == 0);
    assert(app_channel_aps_on(NULL, 6, idx, 4) == 0);
    assert(app_channel_order_channel(NULL, 0) == 0);
    app_channel_advice(NULL, advice, sizeof(advice));
    assert(advice[0] == '\0');

    printf("test_app_channel: PASS\n");
    return 0;
}
