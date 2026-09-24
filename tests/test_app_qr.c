// tests/test_app_qr.c —— app_qr 的往返解码测试。
//
// 除了检查定位/定时/暗模块等结构特征外，本文件实现了一个独立的 QR 解码器，
// 反向执行：读格式信息 → 撤销掩码 → 按之字形取出码字 → 解交织 → RS 校验 →
// 解析字节模式载荷。这是对编码器正确性最强的证明。

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logic/app_qr.h"

// ===========================================================================
// 独立解码器
// ===========================================================================

// GF(256) 乘法，本原多项式 0x11D。
static uint8_t t_gf_mul(uint8_t a, uint8_t b)
{
    uint8_t product = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) product ^= a;
        uint8_t carry = a & 0x80;
        a = (uint8_t)(a << 1);
        if (carry) a ^= 0x1D;
        b = (uint8_t)(b >> 1);
    }
    return product;
}

// 计算码字多项式在 α^i 处的取值（Horner），用于 RS 校验子。
static uint8_t t_syndrome(const uint8_t *block, int len, int i)
{
    uint8_t x = 1;
    for (int k = 0; k < i; k++) x = t_gf_mul(x, 0x02);
    uint8_t s = 0;
    for (int j = 0; j < len; j++) s = t_gf_mul(s, x) ^ block[j];
    return s;
}

static int t_read_bits(const uint8_t *buf, int *bit_pos, int n)
{
    int value = 0;
    for (int i = 0; i < n; i++) {
        int byte = *bit_pos >> 3;
        int bit = 7 - (*bit_pos & 7);
        value = (value << 1) | ((buf[byte] >> bit) & 1);
        (*bit_pos)++;
    }
    return value;
}

// 解码器侧使用的版本参数（与编码器独立，来源于 ISO/IEC 18004 表）。
static const int T_DATA[11] = {0, 16, 28, 44, 64, 86, 108, 124, 154, 182, 216};
static const int T_EC[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const int T_G1[11] = {0, 1, 1, 1, 2, 2, 4, 4, 2, 3, 4};
static const int T_G1DC[11] = {0, 16, 28, 44, 32, 43, 27, 31, 38, 36, 43};
static const int T_G2[11] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 1};
static const int T_G2DC[11] = {0, 0, 0, 0, 0, 0, 0, 0, 39, 37, 44};

static const int T_ALIGN[11][3] = {
    {0, 0, 0}, {0, 0, 0}, {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0},
    {6, 34, 0}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
};

static int t_align_count(int version)
{
    if (version == 1) return 0;
    return version < 7 ? 2 : 3;
}

static void t_mark_finder(uint8_t *f, int size, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= size || y < 0 || y >= size) continue;
            f[y * size + x] = 1;
        }
    }
}

static void t_mark_align(uint8_t *f, int size, int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) f[(cy + dy) * size + (cx + dx)] = 1;
    }
}

// 重建功能模块图（与编码器使用同一套标准规则，但独立实现）。
static void t_build_func(uint8_t *f, int size, int version)
{
    memset(f, 0, (size_t)size * (size_t)size);

    for (int i = 0; i < size; i++) {
        f[i * size + 6] = 1;
        f[6 * size + i] = 1;
    }
    t_mark_finder(f, size, 3, 3);
    t_mark_finder(f, size, size - 4, 3);
    t_mark_finder(f, size, 3, size - 4);

    int n = t_align_count(version);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
            t_mark_align(f, size, T_ALIGN[version][i], T_ALIGN[version][j]);
        }
    }

    // 格式信息区与固定暗模块
    for (int i = 0; i <= 5; i++) f[i * size + 8] = 1;
    f[7 * size + 8] = 1;
    f[8 * size + 8] = 1;
    f[8 * size + 7] = 1;
    for (int i = 9; i < 15; i++) f[8 * size + (14 - i)] = 1;
    for (int i = 0; i < 8; i++) f[8 * size + (size - 1 - i)] = 1;
    for (int i = 8; i < 15; i++) f[(size - 15 + i) * size + 8] = 1;
    f[(size - 8) * size + 8] = 1;

    // 版本信息区
    if (version >= 7) {
        for (int i = 0; i < 18; i++) {
            int a = size - 11 + i % 3;
            int b = i / 3;
            f[b * size + a] = 1;
            f[a * size + b] = 1;
        }
    }
}

// 读取第一份格式信息，返回 5 位（2 位纠错 + 3 位掩码）。
static int t_read_format(const uint8_t *m, int size)
{
    int bits = 0;
    for (int i = 0; i <= 5; i++) bits |= (m[i * size + 8] & 1) << i;
    bits |= (m[7 * size + 8] & 1) << 6;
    bits |= (m[8 * size + 8] & 1) << 7;
    bits |= (m[8 * size + 7] & 1) << 8;
    for (int i = 9; i < 15; i++) bits |= (m[8 * size + (14 - i)] & 1) << i;
    bits ^= 0x5412;
    return (bits >> 10) & 0x1F;
}

static int t_mask_bit(int mask, int x, int y)
{
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return (x * y) % 2 + (x * y) % 3 == 0;
        case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
        default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
}

// 按之字形取出码字并撤销掩码，返回完整码字数。
static int t_extract(const uint8_t *m, const uint8_t *f, int size, int mask, uint8_t *cw)
{
    int bit_count = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (f[y * size + x]) continue;
                int bit = m[y * size + x] & 1;
                if (t_mask_bit(mask, x, y)) bit ^= 1;
                int byte = bit_count >> 3;
                int pos = 7 - (bit_count & 7);
                if ((bit_count & 7) == 0) cw[byte] = 0;
                cw[byte] |= (uint8_t)(bit << pos);
                bit_count++;
            }
        }
    }
    return bit_count / 8;
}

// 完整解码：返回载荷字节数，并写入 out。
static int t_decode(const uint8_t *m, int size, char *out, size_t out_cap)
{
    int version = (size - 17) / 4;
    assert(version >= 1 && version <= 10);

    uint8_t f[APP_QR_MAX_SIZE * APP_QR_MAX_SIZE];
    t_build_func(f, size, version);

    int fmt = t_read_format(m, size);
    int mask = fmt & 7;
    assert(((fmt >> 3) & 3) == 0); // 纠错等级必须是 M

    uint8_t cw[400];
    int cw_count = t_extract(m, f, size, mask, cw);

    int blocks = T_G1[version] + T_G2[version];
    assert(cw_count == T_DATA[version] + blocks * T_EC[version]);

    // 解交织
    uint8_t block_data[10][44];
    uint8_t block_ec[10][26];
    int k = 0;
    int max_dl = T_G2[version] > 0 ? T_G2DC[version] : T_G1DC[version];
    for (int i = 0; i < max_dl; i++) {
        for (int b = 0; b < blocks; b++) {
            int dl = b < T_G1[version] ? T_G1DC[version] : T_G2DC[version];
            if (i < dl) block_data[b][i] = cw[k++];
        }
    }
    for (int i = 0; i < T_EC[version]; i++) {
        for (int b = 0; b < blocks; b++) block_ec[b][i] = cw[k++];
    }
    assert(k == cw_count);

    // 每块做 RS 校验：全部校验子必须为零
    for (int b = 0; b < blocks; b++) {
        int dl = b < T_G1[version] ? T_G1DC[version] : T_G2DC[version];
        uint8_t full[70];
        memcpy(full, block_data[b], (size_t)dl);
        memcpy(full + dl, block_ec[b], (size_t)T_EC[version]);
        for (int i = 0; i < T_EC[version]; i++)
            assert(t_syndrome(full, dl + T_EC[version], i) == 0);
    }

    // 拼接各块数据码字
    uint8_t payload[216];
    int off = 0;
    for (int b = 0; b < blocks; b++) {
        int dl = b < T_G1[version] ? T_G1DC[version] : T_G2DC[version];
        memcpy(payload + off, block_data[b], (size_t)dl);
        off += dl;
    }
    assert(off == T_DATA[version]);

    // 解析字节模式载荷
    int bit_pos = 0;
    int mode = t_read_bits(payload, &bit_pos, 4);
    assert(mode == 0x4);
    int count = t_read_bits(payload, &bit_pos, version <= 9 ? 8 : 16);
    assert((size_t)count + 1 <= out_cap);
    for (int i = 0; i < count; i++) out[i] = (char)t_read_bits(payload, &bit_pos, 8);
    out[count] = '\0';
    return count;
}

// ===========================================================================
// 结构检查
// ===========================================================================

static int mod_at(const uint8_t *m, int size, int x, int y)
{
    return m[y * size + x];
}

// 检查以 (ox, oy) 为左上角的 7x7 定位图形：外环黑、次环白、3x3 核心黑。
static void check_finder(const uint8_t *m, int size, int ox, int oy)
{
    for (int j = 0; j < 7; j++) {
        for (int i = 0; i < 7; i++) {
            int dx = abs(i - 3), dy = abs(j - 3);
            int dist = dx > dy ? dx : dy;
            int expected = dist != 2;
            assert(mod_at(m, size, ox + i, oy + j) == expected);
        }
    }
}

static void check_structure(const uint8_t *m, int size)
{
    check_finder(m, size, 0, 0);
    check_finder(m, size, size - 7, 0);
    check_finder(m, size, 0, size - 7);

    // 定时图形：列 6 与行 6 自偶数坐标起为黑
    assert(mod_at(m, size, 6, 8) == 1);
    assert(mod_at(m, size, 6, 9) == 0);
    assert(mod_at(m, size, 8, 6) == 1);
    assert(mod_at(m, size, 9, 6) == 0);

    // 固定暗模块
    assert(mod_at(m, size, 8, size - 8) == 1);
}

// 编码 → 结构检查 → 独立解码 → 与原文比较。
static void roundtrip(const char *text)
{
    uint8_t modules[APP_QR_MAX_SIZE * APP_QR_MAX_SIZE];
    int size = 0;
    bool ok = app_qr_encode(text, modules, sizeof(modules), &size);
    assert(ok);
    assert(size >= APP_QR_MIN_SIZE && size <= APP_QR_MAX_SIZE);
    assert(size == app_qr_size_for(strlen(text)));

    check_structure(modules, size);

    char decoded[256];
    int n = t_decode(modules, size, decoded, sizeof(decoded));
    assert(n == (int)strlen(text));
    assert(strcmp(decoded, text) == 0);
}

int main(void)
{
    // 尺寸查询
    assert(app_qr_size_for(0) == 21);
    assert(app_qr_size_for(14) == 21);
    assert(app_qr_size_for(15) == 25);
    assert(app_qr_size_for(APP_QR_MAX_BYTES) > 0);
    assert(app_qr_size_for(APP_QR_MAX_BYTES) <= APP_QR_MAX_SIZE);
    // 版本 10-M 的字节容量为 213，超出即无解。
    assert(app_qr_size_for(213) == 57);
    assert(app_qr_size_for(214) == -1);

    // 往返解码：短串、含空格、URL、接近上限的长串
    roundtrip("");
    roundtrip("A");
    roundtrip("HELLO WORLD");
    roundtrip("https://folotoy.com/passport?id=12345");

    char long_text[APP_QR_MAX_BYTES + 1];
    memset(long_text, 'A', APP_QR_MAX_BYTES);
    long_text[APP_QR_MAX_BYTES] = '\0';
    roundtrip(long_text);

    // 参数非法与超长输入
    uint8_t modules[APP_QR_MAX_SIZE * APP_QR_MAX_SIZE];
    int size = 0;

    char too_long[201];
    memset(too_long, 'B', 200);
    too_long[200] = '\0';
    assert(app_qr_encode(too_long, modules, sizeof(modules), &size) == false);

    assert(app_qr_encode("HELLO", NULL, sizeof(modules), &size) == false);
    assert(app_qr_encode(NULL, modules, sizeof(modules), &size) == false);
    assert(app_qr_encode("HELLO", modules, 10, &size) == false);
    assert(app_qr_encode("HELLO", modules, sizeof(modules), NULL) == false);

    puts("test_app_qr: PASS");
    return 0;
}
