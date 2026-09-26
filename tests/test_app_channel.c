// tests/test_app_channel.c —— app_channel 的主机侧单元测试。
//
// 覆盖：复位、非法信道忽略、强度加权累加与封顶、最强值更新、整体拥挤度与推荐信道、
// 结论文案、以及给普通用户的那句建议。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_channel.h"

// 每信道权重 = rssi + 100，夹在 0..60。
static int weight(int rssi)
{
    int w = rssi + 100;
    if (w < 0) w = 0;
    if (w > 60) w = 60;
    return w;
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
    }
    assert(r.ap_total == 0);
    assert(r.congestion == 0);
    assert(r.best_channel == 1);

    // ---- 非法信道直接忽略 ----
    app_channel_add_ap(&r, 0, -50);
    app_channel_add_ap(&r, 14, -50);
    app_channel_add_ap(&r, -1, -50);
    assert(r.ap_total == 0);

    // ---- 单信道：按强度累加 ----
    app_channel_add_ap(&r, 6, -50);
    assert(r.ch[5].aps == 1);
    assert(r.ch[5].strongest == -50);
    assert(r.ch[5].score == weight(-50));   // 50

    app_channel_add_ap(&r, 6, -40);
    assert(r.ch[5].aps == 2);
    assert(r.ch[5].strongest == -40);       // 更强则更新
    assert(r.ch[5].score == weight(-50) + weight(-40));   // 50 + 60 = 110

    app_channel_add_ap(&r, 6, -110);        // 弱于现有最强，不更新 strongest
    assert(r.ch[5].strongest == -40);
    assert(r.ch[5].score == weight(-50) + weight(-40) + weight(-110));
    assert(r.ap_total == 3);

    // ---- finish：封顶与整体拥挤度 ----
    app_channel_finish(&r);
    assert(r.ch[5].score == 100);           // 110 封顶到 100
    assert(r.congestion == 100 / 3);        // (0 + 100 + 0) / 3 = 33
    assert(r.best_channel == 1);            // 1/11 都是 0，取第一个

    // ---- 推荐信道：三个不重叠信道里最空的那条 ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 1, -40);         // 权重 60
    app_channel_add_ap(&r, 6, -90);         // 权重 10
    app_channel_add_ap(&r, 11, -70);        // 权重 30
    app_channel_finish(&r);
    assert(app_channel_score(&r, 1) == 60);
    assert(app_channel_score(&r, 6) == 10);
    assert(app_channel_score(&r, 11) == 30);
    assert(r.congestion == (60 + 10 + 30) / 3);
    assert(r.best_channel == 6);

    // ---- 非 1/6/11 的信道照样统计，但不参与推荐 ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 3, -40);         // 落在信道 3
    app_channel_finish(&r);
    assert(app_channel_score(&r, 3) == 60);
    assert(r.congestion == 0);              // 1/6/11 上都没有 AP
    assert(r.best_channel == 1);

    // ---- 信道越界查询 ----
    assert(app_channel_score(&r, 0) == 0);
    assert(app_channel_score(&r, 14) == 0);

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
    app_channel_add_ap(&r, 6, -100);        // 单个很弱的 AP：拥挤度低
    app_channel_finish(&r);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "很通畅") != NULL);

    app_channel_reset(&r);
    for (int i = 0; i < 6; i++) app_channel_add_ap(&r, 6, -40);   // 信道 6 挤爆
    app_channel_add_ap(&r, 1, -95);
    app_channel_add_ap(&r, 11, -95);
    app_channel_finish(&r);
    assert(r.congestion >= 20);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "信道") != NULL);
    assert(strstr(advice, "1 信道") != NULL || strstr(advice, "11 信道") != NULL);

    // ---- 空指针安全 ----
    app_channel_reset(NULL);
    app_channel_add_ap(NULL, 6, -50);
    app_channel_finish(NULL);
    assert(app_channel_score(NULL, 6) == 0);
    app_channel_advice(NULL, advice, sizeof(advice));
    assert(advice[0] == '\0');

    printf("test_app_channel: PASS\n");
    return 0;
}