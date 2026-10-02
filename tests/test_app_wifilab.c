// tests/test_app_wifilab.c —— app_wifilab 的主机侧单元测试。
//
// 覆盖：四种模式（解除认证 / EAPOL 下线 / SAE 提交 / 信标）的精确字节布局与长度、
// 容量不足返回所需长度、非法入参、确定性 PRNG（xorshift32）与随机 SSID/MAC 的可复现性，
// 以及 Rickroll 歌词集合。
//
// 字节布局全部对照 GhostESP 的 wifi_manager.c（Deauthentication / EAPOL Logoff / SAE flood /
// Random beacon spam）。注意 beacon 的结尾多带 2 字节 0x00 是 GhostESP 源码在 supported_rates
// 处按 12 而非 10 计导致的真实行为，这里按原样复刻。
//
// 本测试只依赖标准库与 logic/app_wifilab.h，不碰任何 ESP-IDF / LVGL 符号；它在
// tools/validate.sh 的 --static 流程里被正式编译执行。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_wifilab.h"

// 与 app_wifilab.c 内部一致的字符集，用于在测试里复现随机 SSID。
static const char CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static void expect_bytes(const uint8_t *buf, size_t len,
                         const uint8_t *expect, size_t expect_len)
{
    assert(len == expect_len);
    assert(memcmp(buf, expect, expect_len) == 0);
}

int main(void)
{
    uint8_t buf[256];
    size_t  len = 0;
    app_wifilab_err_t rc;

    // ---- 解除认证：AP -> 站点方向，序列号由确定性 PRNG 推导 ----
    uint8_t bssid[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t sta[6]   = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    app_wifilab_set_seed(0x12345678u);
    uint32_t st = 0x12345678u;
    uint32_t r = app_wifilab_xorshift32(&st);
    uint16_t exp_seq = (uint16_t)((r & 0xFFF) << 4);
    rc = app_wifilab_build_deauth(buf, sizeof(buf), bssid, sta, 7, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 26);
    {
        uint8_t expect[26];
        expect[0] = 0xC0; expect[1] = 0x00; expect[2] = 0x3A; expect[3] = 0x01;
        memcpy(&expect[4], sta, 6);
        memcpy(&expect[10], bssid, 6);
        memcpy(&expect[16], bssid, 6);
        expect[22] = (uint8_t)(exp_seq & 0xFF);
        expect[23] = (uint8_t)((exp_seq >> 8) & 0xFF);
        expect[24] = 0x07; expect[25] = 0x00;   // reason = 7
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- 解除关联：仅帧控制首字节不同（0xA0）----
    rc = app_wifilab_build_disassoc(buf, sizeof(buf), bssid, sta, 7, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 26);
    assert(buf[0] == 0xA0 && buf[1] == 0x00);
    assert(memcmp(&buf[4], sta, 6) == 0);
    assert(memcmp(&buf[10], bssid, 6) == 0);
    assert(buf[24] == 0x07 && buf[25] == 0x00);

    // ---- 解除认证"反向"（站点 -> AP）：目的=ap_bssid，源/ BSSID = sta ----
    rc = app_wifilab_build_deauth_rev(buf, sizeof(buf), bssid, sta, 7, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 26);
    assert(memcmp(&buf[4], bssid, 6) == 0);
    assert(memcmp(&buf[10], sta, 6) == 0);
    assert(memcmp(&buf[16], sta, 6) == 0);

    // ---- EAPOL Logoff：固定 36 字节，无随机化 ----
    rc = app_wifilab_build_eapol_logoff(buf, sizeof(buf), bssid, sta, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 36);
    {
        uint8_t expect[36];
        expect[0] = 0x08; expect[1] = 0x01; expect[2] = 0x00; expect[3] = 0x00;
        memcpy(&expect[4], bssid, 6);   // 目的 = AP
        memcpy(&expect[10], sta, 6);     // 源 = 站点
        memcpy(&expect[16], bssid, 6);   // BSSID = AP
        expect[22] = 0x00; expect[23] = 0x00;   // 序列控制
        // LLC/SNAP
        expect[24] = 0xAA; expect[25] = 0xAA; expect[26] = 0x03;
        expect[27] = 0x00; expect[28] = 0x00; expect[29] = 0x00;
        expect[30] = 0x88; expect[31] = 0x8E;
        // EAPOL 头：版本 1，类型 Logoff(2)，长度 0
        expect[32] = 0x01; expect[33] = 0x02; expect[34] = 0x00; expect[35] = 0x00;
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- SAE Commit：固定 128 字节，scalar+element 随机化，序列号含 frame_counter ----
    app_wifilab_set_seed(0x9E3779B9u);
    uint32_t sts = 0x9E3779B9u;
    uint32_t rs = app_wifilab_xorshift32(&sts);
    uint16_t exp_sae_seq = (uint16_t)((rs & 0xFFF0) | (0 & 0x000F));   // frame_counter=0
    rc = app_wifilab_build_sae_commit(buf, sizeof(buf), bssid, sta, 0, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 128);
    assert(buf[0] == 0xB0 && buf[1] == 0x00);   // 认证帧控制
    assert(memcmp(&buf[4], bssid, 6) == 0);      // 目的 = AP
    assert(memcmp(&buf[10], sta, 6) == 0);       // 源 = 伪装客户端
    assert(memcmp(&buf[16], bssid, 6) == 0);     // BSSID
    assert(buf[22] == (uint8_t)(exp_sae_seq & 0xFF));
    assert(buf[23] == (uint8_t)((exp_sae_seq >> 8) & 0xFF));
    assert(buf[24] == 0x03 && buf[25] == 0x00);  // 算法 = SAE
    assert(buf[26] == 0x01 && buf[27] == 0x00);  // 事务序号 = Commit
    assert(buf[28] == 0x00 && buf[29] == 0x00);  // 状态码 = 0
    assert(buf[30] == 0x13 && buf[31] == 0x00);  // 组 ID = 19
    // 随机区域确定性：同 seed 必得同一整包
    uint8_t buf2[128];
    app_wifilab_set_seed(0x9E3779B9u);
    rc = app_wifilab_build_sae_commit(buf2, sizeof(buf2), bssid, sta, 0, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(memcmp(buf, buf2, 128) == 0);

    // ---- 信标（随机 SSID + 随机 MAC，信道 6）：长度 74，固定布局 + 末尾 2 字节 0x00 ----
    app_wifilab_set_seed(0x12345678u);
    uint32_t tb = 0x12345678u;
    char exp_ssid[8];
    for (int i = 0; i < 8; i++) {
        exp_ssid[i] = CHARSET[app_wifilab_xorshift32(&tb) % (sizeof(CHARSET) - 1)];
    }
    uint8_t exp_mac[6];
    for (int i = 0; i < 6; i++) {
        exp_mac[i] = (uint8_t)(app_wifilab_xorshift32(&tb) & 0xFF);
    }
    exp_mac[0] = (uint8_t)((exp_mac[0] & 0xFE) | 0x02);
    rc = app_wifilab_build_beacon(buf, sizeof(buf), APP_WIFILAB_BEACON_RANDOM,
                                  NULL, 0, NULL, 6, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 74);   // 38 + 8 + 12 + 3 + 13
    {
        uint8_t expect[74];
        memset(expect, 0, sizeof(expect));
        expect[0] = 0x80; expect[1] = 0x00; expect[2] = 0x00; expect[3] = 0x00;
        memset(&expect[4], 0xFF, 6);                 // 目的：广播
        memcpy(&expect[10], exp_mac, 6);              // 源
        memcpy(&expect[16], exp_mac, 6);              // BSSID
        expect[22] = 0xC0; expect[23] = 0x6C;         // 序列控制
        // 24..31 时间戳：全 0（清零得到）
        expect[32] = 0x64; expect[33] = 0x00;         // Beacon interval
        expect[34] = 0x11; expect[35] = 0x04;         // Capability
        expect[36] = 0x00;                            // SSID tag
        expect[37] = 8;                               // SSID length
        memcpy(&expect[38], exp_ssid, 8);             // SSID
        // p = 46
        expect[46] = 0x01; expect[47] = 0x08;         // Supported rates tag/len
        expect[48] = 0x82; expect[49] = 0x84; expect[50] = 0x8B;
        expect[51] = 0x96; expect[52] = 0x24; expect[53] = 0x30;
        expect[54] = 0x48; expect[55] = 0x6C;
        expect[56] = 0x03; expect[57] = 0x01; expect[58] = 0x06;  // DS: channel 6
        expect[59] = 0xFF; expect[60] = 0x0D;                     // HE tag/len
        expect[61] = 0x50; expect[62] = 0x6F; expect[63] = 0x9A;  // Wi-Fi Alliance OUI
        expect[64] = 0x00; expect[65] = 0x08; expect[66] = 0x00;
        expect[67] = 0x00; expect[68] = 0x40; expect[69] = 0x00;
        expect[70] = 0x00; expect[71] = 0x01;
        // 72..73 为 GhostESP 多带的 2 字节 0x00
        expect_bytes(buf, len, expect, sizeof(expect));
    }

    // ---- 信标（Rickroll 歌词）：SSID 应为传入的歌词，长度随之变化 ----
    const char *lyric = app_wifilab_rickroll_line(0);
    size_t llen = strlen(lyric);
    rc = app_wifilab_build_beacon(buf, sizeof(buf), APP_WIFILAB_BEACON_RICKROLL,
                                  lyric, llen, NULL, 11, &len);
    assert(rc == APP_WIFILAB_OK);
    assert(len == 38 + llen + 12 + 3 + 13);
    assert(buf[37] == (uint8_t)llen);
    assert(memcmp(&buf[38], lyric, llen) == 0);
    assert(buf[50 + llen] == 11);   // DS 信道字节在 38(头)+llen(SSID)+12(速率)+2 处

    // ---- 容量不足：返回所需长度 ----
    uint8_t small[20];
    rc = app_wifilab_build_deauth(small, sizeof(small), bssid, sta, 7, &len);
    assert(rc == APP_WIFILAB_ERR_BUF_TOO_SMALL);
    assert(len == 26);
    rc = app_wifilab_build_eapol_logoff(small, sizeof(small), bssid, sta, &len);
    assert(rc == APP_WIFILAB_ERR_BUF_TOO_SMALL);
    assert(len == 36);
    rc = app_wifilab_build_sae_commit(small, sizeof(small), bssid, sta, 0, &len);
    assert(rc == APP_WIFILAB_ERR_BUF_TOO_SMALL);
    assert(len == 128);
    rc = app_wifilab_build_beacon(small, sizeof(small), APP_WIFILAB_BEACON_RANDOM, NULL, 0, NULL, 6, &len);
    assert(rc == APP_WIFILAB_ERR_BUF_TOO_SMALL);
    assert(len == 38 + 8 + 12 + 3 + 13);

    // ---- 非法入参 ----
    rc = app_wifilab_build_deauth(NULL, 26, bssid, sta, 7, &len);
    assert(rc == APP_WIFILAB_ERR_INVALID_ARG);
    rc = app_wifilab_build_eapol_logoff(buf, sizeof(buf), NULL, sta, &len);
    assert(rc == APP_WIFILAB_ERR_INVALID_ARG);
    rc = app_wifilab_build_sae_commit(buf, sizeof(buf), bssid, NULL, 0, &len);
    assert(rc == APP_WIFILAB_ERR_INVALID_ARG);

    // ---- 确定性 PRNG：同 seed → 同序列；且序列有变化 ----
    uint32_t a = 0x12345678u, b = 0x12345678u;
    uint32_t ra = app_wifilab_xorshift32(&a);
    uint32_t rb = app_wifilab_xorshift32(&b);
    assert(ra == rb);
    uint32_t ra2 = app_wifilab_xorshift32(&a);
    assert(ra2 != ra);
    // 不同 seed 一般不同
    uint32_t c = 0xABCDEF01u;
    uint32_t rc2 = app_wifilab_xorshift32(&c);
    assert(rc2 != ra);

    // ---- Rickroll 歌词集合 ----
    assert(app_wifilab_rickroll_count() == 5);
    assert(strcmp(app_wifilab_rickroll_line(0), "Never gonna give you up") == 0);
    assert(strcmp(app_wifilab_rickroll_line(4), "Never gonna say goodbye") == 0);
    // 取模回卷
    assert(strcmp(app_wifilab_rickroll_line(5), "Never gonna give you up") == 0);

    printf("test_app_wifilab: PASS\n");
    return 0;
}
