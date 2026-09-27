// tests/test_app_channel.c —— app_channel 的主机侧单元测试。
//
// 覆盖：复位、非法信道忽略、强度加权累加与封顶、最强值更新、整体拥挤度与推荐信道、
// 每信道明细（排序）、单信道热点列表（按强度排序 / 隐藏 SSID / 容量上限）、结论文案、
// 给普通用户的建议，以及本次新增的安全模式标签、全量热点排序（热点总览）、
// 邻频重叠边界、环境报告结论与安全态势统计（开放 / WEP / 隐藏）。
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
        assert(r.ch[i].weight == 0);
        assert(r.order[i] == i);            // 默认即信道号升序
    }
    assert(r.stored == 0);
    assert(r.ap_total == 0);
    assert(r.congestion == 0);
    assert(r.best_channel == 1);

    // ---- 非法信道直接忽略 ----
    app_channel_add_ap(&r, 0, -50, "x", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 14, -50, "x", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, -1, -50, "x", 1, NULL, APP_CHANNEL_SEC_OPEN);
    assert(r.ap_total == 0);
    assert(r.stored == 0);

    // ---- 单信道：按强度累加 ----
    app_channel_add_ap(&r, 6, -50, "Home", 4, NULL, APP_CHANNEL_SEC_WPA2);
    assert(r.ch[5].aps == 1);
    assert(r.ch[5].strongest == -50);
    assert(r.ch[5].score == weight(-50));   // 25
    assert(r.ch[5].weight == weight(-50));

    app_channel_add_ap(&r, 6, -40, "Neighbor", 8, NULL, APP_CHANNEL_SEC_OPEN);
    assert(r.ch[5].aps == 2);
    assert(r.ch[5].strongest == -40);       // 更强则更新
    assert(r.ch[5].score == weight(-50) + weight(-40));   // 25 + 30 = 55

    app_channel_add_ap(&r, 6, -110, NULL, 0, NULL, APP_CHANNEL_SEC_UNKNOWN);
    assert(r.ch[5].strongest == -40);
    assert(r.ch[5].score == weight(-50) + weight(-40) + weight(-110));
    assert(r.ap_total == 3);
    assert(r.stored == 3);

    // ---- finish：封顶与整体拥挤度 ----
    app_channel_finish(&r);
    assert(r.ch[5].score == 55);            // 未到 100，说明不再"两台就顶满"
    assert(r.ch[5].weight == 55);           // weight 不封顶，但与未超限的 score 相等
    assert(r.congestion == 55 / 3);         // (0 + 55 + 0) / 3 = 18
    assert(r.best_channel == 1);            // 1/11 都是 0，取第一个

    // ---- 推荐信道：三个不重叠信道里同频+邻频重叠最小的那条 ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 1, -40, "a", 1, NULL, APP_CHANNEL_SEC_OPEN);    // 权重 30
    app_channel_add_ap(&r, 6, -90, "b", 1, NULL, APP_CHANNEL_SEC_OPEN);    // 权重 5
    app_channel_add_ap(&r, 11, -70, "c", 1, NULL, APP_CHANNEL_SEC_OPEN);   // 权重 15
    app_channel_finish(&r);
    assert(app_channel_score(&r, 1) == 30);
    assert(app_channel_score(&r, 6) == 5);
    assert(app_channel_score(&r, 11) == 15);
    assert(r.congestion == (30 + 5 + 15) / 3);
    assert(r.best_channel == 6);

    // ---- 非 1/6/11 的信道不直接计入拥挤度，但会通过邻频重叠影响推荐 ----
    // 信道 3 上的强 AP 离信道 1 更近（隔 2 条），离信道 6 隔 3 条，离信道 11 很远。
    // 过去只看同频会推荐 1；现在重叠一算进来，1 被邻频压得最重，推荐改为 11。
    app_channel_reset(&r);
    app_channel_add_ap(&r, 3, -40, "x", 1, NULL, APP_CHANNEL_SEC_OPEN);    // 落在信道 3
    app_channel_finish(&r);
    assert(app_channel_score(&r, 3) == 30);
    assert(r.congestion == 0);              // 1/6/11 上都没有同频 AP
    assert(app_channel_overlap(&r, 1) == 30 * 3 / 5);   // 隔 2 条，权重 3/5
    assert(app_channel_overlap(&r, 6) == 30 * 2 / 5);   // 隔 3 条，权重 2/5
    assert(app_channel_overlap(&r, 11) == 0);           // 隔 8 条，不相干
    assert(r.best_channel == 11);

    // ---- 信道越界查询 ----
    assert(app_channel_score(&r, 0) == 0);
    assert(app_channel_score(&r, 14) == 0);

    // ---- 每信道排序：最空的排最前，同分按信道号升序 ----
    app_channel_reset(&r);
    for (int i = 0; i < 6; i++) {
        app_channel_add_ap(&r, 6, -40, "n", 1, NULL, APP_CHANNEL_SEC_OPEN);  // 6 -> 封顶 100
    }
    app_channel_add_ap(&r, 1, -95, "f", 1, NULL, APP_CHANNEL_SEC_OPEN);    // 2
    app_channel_add_ap(&r, 11, -95, "f", 1, NULL, APP_CHANNEL_SEC_OPEN);   // 2
    app_channel_finish(&r);
    assert(app_channel_score(&r, 6) == 100);
    // 信道 6 的 weight 不封顶，仍保留真实份量，供重叠计算使用。
    assert(r.ch[5].weight == 6 * weight(-40));
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
    app_channel_add_ap(&r, 6, -70, "Hidden", 0, b1, APP_CHANNEL_SEC_WPA2);  // ssid_len 0 => 隐藏
    app_channel_add_ap(&r, 6, -40, "NearAP", 6, b2, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 6, -55, "Home", 4, b1, APP_CHANNEL_SEC_WPA3);
    app_channel_add_ap(&r, 11, -60, "Other", 5, b2, APP_CHANNEL_SEC_WEP);
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
    // 安全模式随明细落库（由 net 层映射后传入）。
    assert(r.aps[idx[0]].sec == APP_CHANNEL_SEC_OPEN);
    assert(r.aps[idx[2]].sec == APP_CHANNEL_SEC_WPA2);

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

    // ---- 全量热点排序（热点总览）：跨信道按强度降序 ----
    int all[APP_CHANNEL_MAX_APS];
    int total = app_channel_aps_sorted(&r, all, APP_CHANNEL_MAX_APS);
    assert(total == 4);
    assert(r.aps[all[0]].rssi == -40);
    assert(strcmp(r.aps[all[0]].ssid, "NearAP") == 0);
    assert(r.aps[all[1]].rssi == -55);
    assert(r.aps[all[2]].rssi == -60);
    assert(r.aps[all[3]].rssi == -70);
    assert(app_channel_aps_sorted(&r, all, 2) == 2);       // max 限制
    assert(app_channel_aps_sorted(NULL, all, 4) == 0);     // 空指针
    assert(app_channel_aps_sorted(&r, NULL, 4) == 0);      // 空输出
    assert(app_channel_aps_sorted(&r, all, 0) == 0);       // max<=0

    // ---- 邻频重叠：±4 算，±5 起不算 ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 6, -40, "ap", 2, NULL, APP_CHANNEL_SEC_OPEN);   // 权重 30
    app_channel_finish(&r);
    assert(app_channel_overlap(&r, 6) == 30);                // 同频 5/5
    assert(app_channel_overlap(&r, 5) == 30 * 4 / 5);        // 隔 1 条
    assert(app_channel_overlap(&r, 4) == 30 * 3 / 5);        // 隔 2 条
    assert(app_channel_overlap(&r, 2) == 30 * 1 / 5);        // 隔 4 条：仍算重叠
    assert(app_channel_overlap(&r, 10) == 30 * 1 / 5);       // 另一侧的隔 4 条
    assert(app_channel_overlap(&r, 1) == 0);                 // 隔 5 条：不重叠
    assert(app_channel_overlap(&r, 11) == 0);                // 隔 5 条：不重叠
    assert(app_channel_overlap(&r, 0) == 0);                 // 越界
    assert(app_channel_overlap(&r, 14) == 0);                // 越界
    assert(app_channel_overlap(NULL, 6) == 0);               // 空指针

    // ---- 安全模式文本 ----
    assert(strcmp(app_channel_sec_text(APP_CHANNEL_SEC_OPEN), "开放") == 0);
    assert(strcmp(app_channel_sec_text(APP_CHANNEL_SEC_WEP), "WEP") == 0);
    assert(strcmp(app_channel_sec_text(APP_CHANNEL_SEC_WPA), "WPA") == 0);
    assert(strcmp(app_channel_sec_text(APP_CHANNEL_SEC_WPA2), "WPA2") == 0);
    assert(strcmp(app_channel_sec_text(APP_CHANNEL_SEC_WPA3), "WPA3") == 0);
    assert(strcmp(app_channel_sec_text(APP_CHANNEL_SEC_UNKNOWN), "未知") == 0);
    assert(strcmp(app_channel_sec_text((app_channel_sec_t)99), "未知") == 0);   // 越界兜底

    // ---- 频段文本：本机恒为 2.4G ----
    assert(strcmp(app_channel_band_text(), "2.4G") == 0);

    // ---- 明细容量上限：统计照算，明细截断 ----
    app_channel_reset(&r);
    for (int i = 0; i < APP_CHANNEL_MAX_APS + 10; i++) {
        char name[8];
        snprintf(name, sizeof(name), "ap%02d", i);
        app_channel_add_ap(&r, 1, -60, name, (int)strlen(name), NULL, APP_CHANNEL_SEC_OPEN);
    }
    app_channel_finish(&r);
    assert(r.stored == APP_CHANNEL_MAX_APS);
    assert(r.ap_total == APP_CHANNEL_MAX_APS + 10);
    assert(app_channel_aps_on(&r, 1, idx, APP_CHANNEL_MAX_APS) == APP_CHANNEL_MAX_APS);
    assert(app_channel_aps_sorted(&r, all, APP_CHANNEL_MAX_APS) == APP_CHANNEL_MAX_APS);
    // 明细被截断，但同频 weight 仍统计了全部 AP（重叠计算因此不会漏算）。
    assert(r.ch[0].weight == (APP_CHANNEL_MAX_APS + 10) * weight(-60));

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
    app_channel_add_ap(&r, 6, -100, "w", 1, NULL, APP_CHANNEL_SEC_OPEN);   // 单个很弱的 AP：拥挤度低
    app_channel_finish(&r);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "很通畅") != NULL);

    app_channel_reset(&r);
    for (int i = 0; i < 6; i++) {
        app_channel_add_ap(&r, 6, -40, "n", 1, NULL, APP_CHANNEL_SEC_OPEN); // 信道 6 挤爆
    }
    app_channel_add_ap(&r, 1, -95, "f", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 11, -95, "f", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_finish(&r);
    assert(r.congestion >= 20);
    app_channel_advice(&r, advice, sizeof(advice));
    assert(strstr(advice, "信道") != NULL);
    assert(strstr(advice, "1 信道") != NULL || strstr(advice, "11 信道") != NULL);

    // ---- 环境报告：点出邻频来源并给出建议信道 ----
    // 信道 3、9 各一台强 AP：推荐信道落在 1，其邻频干扰主要来自 3 信道。
    char env[128];
    app_channel_reset(&r);
    app_channel_add_ap(&r, 3, -40, "a", 1, NULL, APP_CHANNEL_SEC_WPA2);
    app_channel_add_ap(&r, 9, -40, "b", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_finish(&r);
    assert(r.best_channel == 1);
    app_channel_env_summary(&r, env, sizeof(env));
    assert(env[0] != '\0');
    assert(strstr(env, "邻频") != NULL);
    assert(strstr(env, "3 信道") != NULL);
    assert(strstr(env, "1 信道") != NULL);          // 含建议信道

    // 推荐信道两侧各有一条邻频来源时，两条都列出来（编号升序）。
    app_channel_reset(&r);
    app_channel_add_ap(&r, 3, -40, "a", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 5, -40, "b", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 12, -40, "c", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_finish(&r);
    assert(r.best_channel == 1);
    app_channel_env_summary(&r, env, sizeof(env));
    assert(strstr(env, "3/5 信道") != NULL);

    // 空报告：也要给一句可读结论，而不是空串。
    app_channel_reset(&r);
    app_channel_finish(&r);
    app_channel_env_summary(&r, env, sizeof(env));
    assert(env[0] != '\0');

    // 越界/空指针兜底。
    app_channel_env_summary(&r, NULL, sizeof(env));
    app_channel_env_summary(&r, env, 0);
    app_channel_env_summary(NULL, env, sizeof(env));
    assert(env[0] == '\0');

    // ---- 安全态势统计：开放 / WEP / 隐藏 各多少（只读，基于已保存明细） ----
    app_channel_reset(&r);
    app_channel_add_ap(&r, 1, -50, "open1", 5, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 1, -60, "open2", 5, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_add_ap(&r, 6, -55, "wep1", 4, NULL, APP_CHANNEL_SEC_WEP);
    app_channel_add_ap(&r, 6, -70, NULL, 0, NULL, APP_CHANNEL_SEC_WPA2);   // 隐藏 SSID
    app_channel_add_ap(&r, 11, -65, "wpa3", 4, NULL, APP_CHANNEL_SEC_WPA3);
    app_channel_finish(&r);
    int n_open = -1, n_wep = -1, n_hid = -1;
    app_channel_security_counts(&r, &n_open, &n_wep, &n_hid);
    assert(n_open == 2);
    assert(n_wep == 1);
    assert(n_hid == 1);

    // 任一输出指针可为 NULL，不应崩溃。
    app_channel_security_counts(&r, NULL, NULL, &n_hid);
    assert(n_hid == 1);
    // 空报告 / 空指针：输出全部清零。
    app_channel_security_counts(NULL, &n_open, &n_wep, &n_hid);
    assert(n_open == 0 && n_wep == 0 && n_hid == 0);

    // ---- 空指针安全 ----
    app_channel_reset(NULL);
    app_channel_add_ap(NULL, 6, -50, "x", 1, NULL, APP_CHANNEL_SEC_OPEN);
    app_channel_finish(NULL);
    assert(app_channel_score(NULL, 6) == 0);
    assert(app_channel_aps_on(NULL, 6, idx, 4) == 0);
    assert(app_channel_aps_sorted(NULL, idx, 4) == 0);
    assert(app_channel_overlap(NULL, 6) == 0);
    assert(app_channel_order_channel(NULL, 0) == 0);
    app_channel_advice(NULL, advice, sizeof(advice));
    assert(advice[0] == '\0');

    printf("test_app_channel: PASS\n");
    return 0;
}