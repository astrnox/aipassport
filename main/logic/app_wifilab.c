// main/logic/app_wifilab.c —— Wi-Fi 实验纯逻辑层的实现（见 app_wifilab.h 的来源说明）。
//
// 本文件是 GhostESP wifi_manager.c 中若干原始 802.11 帧构造逻辑的忠实移植，只做"算字节"，
// 不发射、不碰 ESP-IDF。随机化一律走确定性 xorshift32（种子由 app_wifilab_set_seed 设定），
// 以便主机侧单测能复现每一字节。
#include "app_wifilab.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 确定性 PRNG
// ---------------------------------------------------------------------------

// 默认种子：与 BLE 实验错开，避免"同一数值被误当作未初始化"。
static uint32_t s_prng = 0x1A2B3C4Du;

void app_wifilab_set_seed(uint32_t seed)
{
    // 0 视为无效，保持当前状态（与 BLE 实验约定一致）。
    if (seed == 0) return;
    s_prng = seed;
}

uint32_t app_wifilab_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void app_wifilab_random_mac(uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)(app_wifilab_xorshift32(&s_prng) & 0xFF);
    }
    // 本地管理单播地址（首字节高两位 11），与 GhostESP generate_random_mac 一致。
    out[0] = (out[0] & 0xFE) | 0x02;
}

void app_wifilab_random_ssid(char *out, size_t len)
{
    // 字符集与 GhostESP generate_random_ssid 完全一致。
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t set_len = sizeof(charset) - 1;   // 不含结尾 NUL
    for (size_t i = 0; i < len; i++) {
        out[i] = charset[app_wifilab_xorshift32(&s_prng) % set_len];
    }
}

// ---------------------------------------------------------------------------
// Rickroll 歌词
// ---------------------------------------------------------------------------

// 与 GhostESP wifi_beacon_task 的 rickroll_lyrics 一致；其实际循环只用前 5 句（num_lines=5），
// 第 6 句虽在数组里但不会被广播，这里同样只暴露前 5 句，保持与发射行为一致。
static const char *const RICKROLL_LYRICS[5] = {
    "Never gonna give you up",
    "Never gonna let you down",
    "Never gonna run around and desert you",
    "Never gonna make you cry",
    "Never gonna say goodbye",
};

int app_wifilab_rickroll_count(void)
{
    return 5;
}

const char *app_wifilab_rickroll_line(int index)
{
    int n = app_wifilab_rickroll_count();
    int i = ((index % n) + n) % n;
    return RICKROLL_LYRICS[i];
}

// ---------------------------------------------------------------------------
// 解除认证 / 解除关联
// ---------------------------------------------------------------------------

// 方向无关的 26 字节去认证/去关联帧构造。fc0 区分 deauth(0xC0) 与 disassoc(0xA0)；
// dst/src/bssid3 三个地址按调用方意图填写（GhostESP 在 broadcast_deauth 里分别处理
// AP->STA 与 STA->AP 两个方向）。
static app_wifilab_err_t build_deauth_dir(uint8_t *buf, size_t cap,
                                         const uint8_t *dst, const uint8_t *src,
                                         const uint8_t *bssid3, uint8_t fc0,
                                         uint16_t reason, size_t *out_len)
{
    const size_t SZ = 26;
    if (!buf || !out_len || !dst || !src || !bssid3) {
        if (out_len) *out_len = SZ;
        return APP_WIFILAB_ERR_INVALID_ARG;
    }
    if (cap < SZ) {
        *out_len = SZ;
        return APP_WIFILAB_ERR_BUF_TOO_SMALL;
    }

    // 帧控制 + 持续时间（0xC0/0xA0, 0x00；0x3A, 0x01）。
    buf[0] = fc0;
    buf[1] = 0x00;
    buf[2] = 0x3A;
    buf[3] = 0x01;
    // 目的 / 源 / BSSID。
    memcpy(&buf[4], dst, 6);
    memcpy(&buf[10], src, 6);
    memcpy(&buf[16], bssid3, 6);
    // 序列号：低 12 位随机、左移 4（与 GhostESP 的 (esp_random() & 0xFFF) << 4 一致）。
    uint16_t seq = (uint16_t)((app_wifilab_xorshift32(&s_prng) & 0xFFF) << 4);
    buf[22] = (uint8_t)(seq & 0xFF);
    buf[23] = (uint8_t)((seq >> 8) & 0xFF);
    // 原因码（小端）。
    buf[24] = (uint8_t)(reason & 0xFF);
    buf[25] = (uint8_t)((reason >> 8) & 0xFF);

    *out_len = SZ;
    return APP_WIFILAB_OK;
}

app_wifilab_err_t app_wifilab_build_deauth(uint8_t *buf, size_t cap,
                                           const uint8_t bssid[6], const uint8_t sta[6],
                                           uint16_t reason, size_t *out_len)
{
    // AP -> 站点方向：目的=sta，源=ap_bssid，BSSID=ap_bssid。
    return build_deauth_dir(buf, cap, sta, bssid, bssid, 0xC0, reason, out_len);
}

app_wifilab_err_t app_wifilab_build_disassoc(uint8_t *buf, size_t cap,
                                             const uint8_t bssid[6], const uint8_t sta[6],
                                             uint16_t reason, size_t *out_len)
{
    return build_deauth_dir(buf, cap, sta, bssid, bssid, 0xA0, reason, out_len);
}

app_wifilab_err_t app_wifilab_build_deauth_rev(uint8_t *buf, size_t cap,
                                              const uint8_t bssid[6], const uint8_t sta[6],
                                              uint16_t reason, size_t *out_len)
{
    // 站点 -> AP 方向：目的=ap_bssid，源=sta，BSSID=sta。
    return build_deauth_dir(buf, cap, bssid, sta, sta, 0xC0, reason, out_len);
}

app_wifilab_err_t app_wifilab_build_disassoc_rev(uint8_t *buf, size_t cap,
                                                const uint8_t bssid[6], const uint8_t sta[6],
                                                uint16_t reason, size_t *out_len)
{
    return build_deauth_dir(buf, cap, bssid, sta, sta, 0xA0, reason, out_len);
}

// ---------------------------------------------------------------------------
// EAPOL Logoff
// ---------------------------------------------------------------------------

app_wifilab_err_t app_wifilab_build_eapol_logoff(uint8_t *buf, size_t cap,
                                                 const uint8_t ap_bssid[6],
                                                 const uint8_t sta_mac[6], size_t *out_len)
{
    const size_t SZ = 36;
    if (!buf || !out_len || !ap_bssid || !sta_mac) {
        if (out_len) *out_len = SZ;
        return APP_WIFILAB_ERR_INVALID_ARG;
    }
    if (cap < SZ) {
        *out_len = SZ;
        return APP_WIFILAB_ERR_BUF_TOO_SMALL;
    }

    // 帧控制（Data, ToDS=1, FromDS=0）+ 持续时间 + 三个地址占位。
    buf[0] = 0x08; buf[1] = 0x01;
    buf[2] = 0x00; buf[3] = 0x00;
    memcpy(&buf[4], ap_bssid, 6);    // 目的 = AP
    memcpy(&buf[10], sta_mac, 6);     // 源 = 站点
    memcpy(&buf[16], ap_bssid, 6);    // BSSID = AP
    buf[22] = 0x00; buf[23] = 0x00;   // 序列控制（保持 0，与模板一致）
    // LLC/SNAP：AA AA 03 00 00 00 88 8E
    buf[24] = 0xAA; buf[25] = 0xAA; buf[26] = 0x03;
    buf[27] = 0x00; buf[28] = 0x00; buf[29] = 0x00;
    buf[30] = 0x88; buf[31] = 0x8E;
    // EAPOL 头：版本 1，类型 Logoff(2)，长度 0。
    buf[32] = 0x01; buf[33] = 0x02; buf[34] = 0x00; buf[35] = 0x00;

    *out_len = SZ;
    return APP_WIFILAB_OK;
}

// ---------------------------------------------------------------------------
// SAE Commit
// ---------------------------------------------------------------------------

app_wifilab_err_t app_wifilab_build_sae_commit(uint8_t *buf, size_t cap,
                                               const uint8_t target_bssid[6],
                                               const uint8_t src_mac[6],
                                               uint16_t frame_counter, size_t *out_len)
{
    const size_t SZ = 128;
    if (!buf || !out_len || !target_bssid || !src_mac) {
        if (out_len) *out_len = SZ;
        return APP_WIFILAB_ERR_INVALID_ARG;
    }
    if (cap < SZ) {
        *out_len = SZ;
        return APP_WIFILAB_ERR_BUF_TOO_SMALL;
    }

    // 帧控制（Authentication）+ 持续时间 + 三个地址占位。
    buf[0] = 0xB0; buf[1] = 0x00;
    buf[2] = 0x00; buf[3] = 0x00;
    memcpy(&buf[4], target_bssid, 6);   // 目的 = AP
    memcpy(&buf[10], src_mac, 6);       // 源 = 伪装客户端
    memcpy(&buf[16], target_bssid, 6);  // BSSID
    // 序列控制：高 12 位随机，低 4 位为 frame_counter（与 GhostESP 一致）。
    uint16_t seq = (uint16_t)((app_wifilab_xorshift32(&s_prng) & 0xFFF0)
                              | (frame_counter & 0x000F));
    buf[22] = (uint8_t)(seq & 0xFF);
    buf[23] = (uint8_t)((seq >> 8) & 0xFF);
    // 认证帧体：算法=SAE(3)，事务序号=Commit(1)，状态码=0，组 ID=19(P-256)。
    buf[24] = 0x03; buf[25] = 0x00;
    buf[26] = 0x01; buf[27] = 0x00;
    buf[28] = 0x00; buf[29] = 0x00;
    buf[30] = 0x13; buf[31] = 0x00;
    // scalar(32) + element(64) 全部随机化（与 GhostESP inject_sae_commit_frame 一致）。
    for (int i = 32; i < (int)SZ; i++) {
        buf[i] = (uint8_t)(app_wifilab_xorshift32(&s_prng) & 0xFF);
    }

    *out_len = SZ;
    return APP_WIFILAB_OK;
}

// ---------------------------------------------------------------------------
// 信标帧
// ---------------------------------------------------------------------------

app_wifilab_err_t app_wifilab_build_beacon(uint8_t *buf, size_t cap,
                                          app_wifilab_beacon_t sub,
                                          const char *ssid, size_t ssid_len,
                                          const uint8_t bssid[6], int channel,
                                          size_t *out_len)
{
    // 解析 SSID：RANDOM 且未提供时使用随机 SSID；其余情况用调用方传入的（Rickroll/AP列表）。
    char local_ssid[APP_WIFILAB_RANDOM_SSID_LEN + 1];
    const char *use_ssid = ssid;
    size_t use_len = ssid_len;
    if (sub == APP_WIFILAB_BEACON_RANDOM && (!ssid || ssid_len == 0)) {
        app_wifilab_random_ssid(local_ssid, APP_WIFILAB_RANDOM_SSID_LEN);
        use_ssid = local_ssid;
        use_len = APP_WIFILAB_RANDOM_SSID_LEN;
    }
    if (use_len > 32) use_len = 32;

    // 解析 MAC：未提供时随机化。
    uint8_t local_mac[6];
    const uint8_t *use_bssid = bssid;
    if (!use_bssid) {
        app_wifilab_random_mac(local_mac);
        use_bssid = local_mac;
    }

    // 长度：复刻 GhostESP 的 38 + ssid_len + 12 + 3 + 13（其源码把 supported_rates 按 12
    // 而非 10 计，会多带 2 字节 0x00；这里同样复刻该行为）。
    const size_t SZ = 38 + use_len + 12 + 3 + 13;
    if (!buf || !out_len) {
        if (out_len) *out_len = SZ;
        return APP_WIFILAB_ERR_INVALID_ARG;
    }
    if (cap < SZ) {
        *out_len = SZ;
        return APP_WIFILAB_ERR_BUF_TOO_SMALL;
    }

    // 先整包清零：覆盖帧头未显式写入的字节（时间戳 8 字节）以及末尾 2 字节 0x00。
    memset(buf, 0, SZ);

    // 帧控制 + 持续时间 + 目的（广播）+ 源 + BSSID。
    buf[0] = 0x80; buf[1] = 0x00;   // Beacon
    buf[2] = 0x00; buf[3] = 0x00;   // Duration
    memset(&buf[4], 0xFF, 6);       // 目的：广播
    memcpy(&buf[10], use_bssid, 6); // 源
    memcpy(&buf[16], use_bssid, 6); // BSSID
    buf[22] = 0xC0; buf[23] = 0x6C; // 序列控制（固定，与模板一致）
    buf[32] = 0x64; buf[33] = 0x00; // Beacon interval = 100 TU
    buf[34] = 0x11; buf[35] = 0x04; // Capability info (ESS)
    // 0x36 处的 SSID 元素标识符（tag=0x00）已由清零得到；0x37 为长度。
    buf[37] = (uint8_t)use_len;
    if (use_len > 0) {
        memcpy(&buf[38], use_ssid, use_len);
    }

    size_t p = 38 + use_len;
    // Supported Rates IE：tag=0x01, len=0x08, 8 个速率。
    buf[p + 0] = 0x01; buf[p + 1] = 0x08;
    buf[p + 2] = 0x82; buf[p + 3] = 0x84; buf[p + 4] = 0x8B; buf[p + 5] = 0x96;
    buf[p + 6] = 0x24; buf[p + 7] = 0x30; buf[p + 8] = 0x48; buf[p + 9] = 0x6C;
    // DS Parameter Set IE：tag=0x03, len=0x01, 当前信道。
    buf[p + 10] = 0x03; buf[p + 11] = 0x01;
    buf[p + 12] = (uint8_t)(channel & 0xFF);
    // HE Capabilities（伪装 Wi-Fi 6）：tag=0xFF, len=0x0D, Wi-Fi Alliance OUI 00:50:6F(9A)。
    buf[p + 13] = 0xFF; buf[p + 14] = 0x0D;
    buf[p + 15] = 0x50; buf[p + 16] = 0x6F; buf[p + 17] = 0x9A;
    buf[p + 18] = 0x00; buf[p + 19] = 0x08; buf[p + 20] = 0x00;
    buf[p + 21] = 0x00; buf[p + 22] = 0x40; buf[p + 23] = 0x00;
    buf[p + 24] = 0x00; buf[p + 25] = 0x01;
    // 末尾 2 字节（38 + use_len + 12 + 3 + 13 处）保持 0x00，复刻 GhostESP 的多带字节。

    *out_len = SZ;
    return APP_WIFILAB_OK;
}
