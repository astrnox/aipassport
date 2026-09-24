// main/logic/app_qr.c —— 字节模式 QR 码编码器（纠错等级 M，版本 1..10）。
//
// 本层不依赖 ESP-IDF 与 LVGL，可在主机上直接编译测试。实现遵循 ISO/IEC 18004：
// 数据编码 → Reed-Solomon 纠错 → 分块交织 → 矩阵铺设 → 掩码评估。矩阵为行优先，
// 1 表示黑模块、0 表示白模块。

#include "app_qr.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// GF(256) 运算（本原多项式 0x11D）
// ---------------------------------------------------------------------------

// 伽罗华域乘法：把 b 逐位拆开，对 a 做 x 倍乘（溢出时约简 0x11D）。
static uint8_t gf_mul(uint8_t a, uint8_t b)
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

// 计算次数为 degree 的 RS 生成多项式，根为 α^0..α^(degree-1)。
static void rs_divisor(int degree, uint8_t *out)
{
    memset(out, 0, (size_t)degree);
    out[degree - 1] = 1; // 从单项式 x^0 开始
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            out[j] = gf_mul(out[j], root);
            if (j + 1 < degree) out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

// 计算数据码字除以生成多项式后的余数，即纠错码字。
static void rs_remainder(const uint8_t *data, int len, const uint8_t *gen,
                         int degree, uint8_t *out)
{
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ out[0];
        memmove(&out[0], &out[1], (size_t)(degree - 1));
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++) out[j] ^= gf_mul(gen[j], factor);
    }
}

// ---------------------------------------------------------------------------
// 版本参数（纠错等级 M）
// ---------------------------------------------------------------------------

typedef struct {
    int data;        // 数据码字总数
    int ec_per_block; // 每块纠错码字数
    int g1_blocks;   // 第 1 组块数
    int g1_data;     // 第 1 组每块数据码字数
    int g2_blocks;   // 第 2 组块数
    int g2_data;     // 第 2 组每块数据码字数
} qr_version_info;

// 下标 0 占位，1..10 对应版本。
static const qr_version_info VERSION_INFO[11] = {
    {0, 0, 0, 0, 0, 0},
    {16, 10, 1, 16, 0, 0},
    {28, 16, 1, 28, 0, 0},
    {44, 26, 1, 44, 0, 0},
    {64, 18, 2, 32, 0, 0},
    {86, 24, 2, 43, 0, 0},
    {108, 16, 4, 27, 0, 0},
    {124, 18, 4, 31, 0, 0},
    {154, 22, 2, 38, 2, 39},
    {182, 22, 3, 36, 2, 37},
    {216, 26, 4, 43, 1, 44},
};

// 版本 2..10 的对齐图形中心坐标。
static const uint8_t ALIGN_POS[11][3] = {
    {0, 0, 0}, {0, 0, 0}, {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0},
    {6, 34, 0}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
};
static const uint8_t ALIGN_COUNT[11] = {0, 0, 2, 2, 2, 2, 2, 3, 3, 3, 3};

// 字节模式下字符计数指示符的位数。
static int count_bits(int version)
{
    return version <= 9 ? 8 : 16;
}

// 该版本在字节模式下可容纳的字节数。
static int data_capacity_bytes(int version)
{
    int bits = VERSION_INFO[version].data * 8;
    return (bits - 4 - count_bits(version)) / 8;
}

// ---------------------------------------------------------------------------
// 比特流写入
// ---------------------------------------------------------------------------

// 按高位优先写入 value 的低 nbits 位；调用前缓冲需清零。
static void put_bits(uint8_t *buf, size_t *bit_pos, uint32_t value, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        size_t byte = *bit_pos >> 3;
        int bit = 7 - (int)(*bit_pos & 7);
        if ((value >> i) & 1u) buf[byte] |= (uint8_t)(1u << bit);
        (*bit_pos)++;
    }
}

// ---------------------------------------------------------------------------
// 矩阵铺设
// ---------------------------------------------------------------------------

// 写入一个功能模块，并在功能图上标记（掩码不作用于功能模块）。
static void set_func(uint8_t *modules, uint8_t *is_func, int size, int x, int y, int dark)
{
    modules[y * size + x] = (uint8_t)(dark ? 1 : 0);
    is_func[y * size + x] = 1;
}

// 定位图形及其分隔符：以 (cx, cy) 为中心，切比雪夫距离 2、4 处为浅色。
static void draw_finder(uint8_t *modules, uint8_t *is_func, int size, int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= size || y < 0 || y >= size) continue;
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            set_func(modules, is_func, size, x, y, dist != 2 && dist != 4);
        }
    }
}

// 对齐图形：5x5，距离 1 处为浅色。
static void draw_align(uint8_t *modules, uint8_t *is_func, int size, int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            set_func(modules, is_func, size, cx + dx, cy + dy, dist != 1);
        }
    }
}

// 格式信息：2 位纠错等级（M=00）+ 3 位掩码，经 BCH(15,5) 编码并异或 0x5412。
static void draw_format_bits(uint8_t *modules, uint8_t *is_func, int size, int mask)
{
    int data = mask; // (0 << 3) | mask，纠错等级 M 的指示位为 00
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;

    // 第一份：左上角定位图形周围
    for (int i = 0; i <= 5; i++) set_func(modules, is_func, size, 8, i, (bits >> i) & 1);
    set_func(modules, is_func, size, 8, 7, (bits >> 6) & 1);
    set_func(modules, is_func, size, 8, 8, (bits >> 7) & 1);
    set_func(modules, is_func, size, 7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) set_func(modules, is_func, size, 14 - i, 8, (bits >> i) & 1);

    // 第二份：右上与左下
    for (int i = 0; i < 8; i++) set_func(modules, is_func, size, size - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) set_func(modules, is_func, size, 8, size - 15 + i, (bits >> i) & 1);
    set_func(modules, is_func, size, 8, size - 8, 1); // 固定暗模块
}

// 版本信息（版本 >= 7）：6 位版本号经 BCH(18,6) 编码，共 18 位。
static void draw_version_bits(uint8_t *modules, uint8_t *is_func, int size, int version)
{
    if (version < 7) return;
    int rem = version;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    int bits = (version << 12) | rem;
    for (int i = 0; i < 18; i++) {
        int bit = (bits >> i) & 1;
        int a = size - 11 + i % 3;
        int b = i / 3;
        set_func(modules, is_func, size, a, b, bit);
        set_func(modules, is_func, size, b, a, bit);
    }
}

// 铺设全部功能图形。
static void draw_function_patterns(uint8_t *modules, uint8_t *is_func, int size, int version)
{
    // 定时图形：行 6 与列 6 交替，偶数坐标处为黑。
    for (int i = 0; i < size; i++) {
        set_func(modules, is_func, size, 6, i, i % 2 == 0);
        set_func(modules, is_func, size, i, 6, i % 2 == 0);
    }

    // 三个定位图形（会覆盖部分定时图形，符合标准）。
    draw_finder(modules, is_func, size, 3, 3);
    draw_finder(modules, is_func, size, size - 4, 3);
    draw_finder(modules, is_func, size, 3, size - 4);

    // 对齐图形：跳过与定位图形重叠的三个角。
    int n = ALIGN_COUNT[version];
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
            draw_align(modules, is_func, size, ALIGN_POS[version][i], ALIGN_POS[version][j]);
        }
    }

    draw_format_bits(modules, is_func, size, 0); // 先占位，选掩码后再重画
    draw_version_bits(modules, is_func, size, version);
}

// 按标准之字形顺序放置码字；剩余位保持为 0（浅色）。
static void place_codewords(uint8_t *modules, const uint8_t *is_func, int size,
                            const uint8_t *codewords, int total_cw)
{
    int total_bits = total_cw * 8;
    int i = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5; // 跳过定时图形所在列
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (!is_func[y * size + x] && i < total_bits) {
                    modules[y * size + x] =
                        (uint8_t)((codewords[i >> 3] >> (7 - (i & 7))) & 1);
                    i++;
                }
            }
        }
    }
}

// 掩码条件（x 为列，y 为行）。
static int mask_bit(int mask, int x, int y)
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

// 对非功能模块应用掩码（异或，自反，可用于撤销）。
static void apply_mask(uint8_t *modules, const uint8_t *is_func, int size, int mask)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (!is_func[y * size + x] && mask_bit(mask, x, y))
                modules[y * size + x] ^= 1;
        }
    }
}

// ---------------------------------------------------------------------------
// 掩码惩罚分（ISO/IEC 18004 第 4 条规则）
// ---------------------------------------------------------------------------

// 把最近 7 段连续同色长度（hist[0] 最新）平移并写入。
static void penalty_add_history(int run, int hist[7], int size)
{
    if (hist[0] == 0) run += size; // 首次调用：前面补上静区
    memmove(&hist[1], &hist[0], 6 * sizeof(hist[0]));
    hist[0] = run;
}

// 在长度历史中统计 1:1:3:1:1 的类定位图形（两侧各有 >= 4 倍宽的浅色）。
static int penalty_count_patterns(const int hist[7])
{
    int n = hist[1];
    int core = n > 0 && hist[2] == n && hist[3] == n * 3 && hist[4] == n && hist[5] == n;
    return (core && hist[0] >= n * 4 && hist[6] >= n ? 1 : 0) +
           (core && hist[6] >= n * 4 && hist[0] >= n ? 1 : 0);
}

// 收尾：补上末尾静区并统计图案。
static int penalty_terminate(int color, int run, int hist[7], int size)
{
    if (color) {
        penalty_add_history(run, hist, size);
        run = 0;
    }
    run += size;
    penalty_add_history(run, hist, size);
    return penalty_count_patterns(hist);
}

static int penalty_score(const uint8_t *modules, int size)
{
    int result = 0;

    // 规则 1 与规则 3：逐行、逐列扫描连续同色段与类定位图形。
    for (int y = 0; y < size; y++) {
        int run_color = 0, run_len = 0;
        int hist[7] = {0};
        for (int x = 0; x < size; x++) {
            int color = modules[y * size + x] != 0;
            if (color == run_color) {
                run_len++;
                if (run_len == 5) result += 3;
                else if (run_len > 5) result++;
            } else {
                penalty_add_history(run_len, hist, size);
                if (!run_color) result += penalty_count_patterns(hist) * 40;
                run_color = color;
                run_len = 1;
            }
        }
        result += penalty_terminate(run_color, run_len, hist, size) * 40;
    }
    for (int x = 0; x < size; x++) {
        int run_color = 0, run_len = 0;
        int hist[7] = {0};
        for (int y = 0; y < size; y++) {
            int color = modules[y * size + x] != 0;
            if (color == run_color) {
                run_len++;
                if (run_len == 5) result += 3;
                else if (run_len > 5) result++;
            } else {
                penalty_add_history(run_len, hist, size);
                if (!run_color) result += penalty_count_patterns(hist) * 40;
                run_color = color;
                run_len = 1;
            }
        }
        result += penalty_terminate(run_color, run_len, hist, size) * 40;
    }

    // 规则 2：2x2 同色块。
    for (int y = 0; y < size - 1; y++) {
        for (int x = 0; x < size - 1; x++) {
            int c = modules[y * size + x] != 0;
            if (c == (modules[y * size + x + 1] != 0) &&
                c == (modules[(y + 1) * size + x] != 0) &&
                c == (modules[(y + 1) * size + x + 1] != 0))
                result += 3;
        }
    }

    // 规则 4：黑白比例偏离 50% 的程度，每 5% 计 10 分。
    int dark = 0;
    for (int i = 0; i < size * size; i++) {
        if (modules[i]) dark++;
    }
    int total = size * size;
    int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * 10;

    return result;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

int app_qr_size_for(size_t text_len)
{
    for (int v = 1; v <= 10; v++) {
        if ((int)text_len <= data_capacity_bytes(v)) return 17 + 4 * v;
    }
    return -1;
}

bool app_qr_encode(const char *text, uint8_t *modules, size_t modules_cap, int *size_out)
{
    if (!text || !modules || !size_out) return false;

    size_t len = strlen(text);
    if (len > APP_QR_MAX_BYTES) return false;

    int version = 0;
    for (int v = 1; v <= 10; v++) {
        if ((int)len <= data_capacity_bytes(v)) {
            version = v;
            break;
        }
    }
    if (version == 0) return false;

    int size = 17 + 4 * version;
    if (modules_cap < (size_t)size * (size_t)size) return false;

    const qr_version_info *vi = &VERSION_INFO[version];
    int total_data = vi->data;
    int ec_len = vi->ec_per_block;
    int blocks = vi->g1_blocks + vi->g2_blocks;

    // 1. 数据编码：模式 + 字符计数 + 原始字节 + 终止符 + 补齐 + 填充字节。
    uint8_t data_cw[216];
    memset(data_cw, 0, sizeof(data_cw));
    size_t bit_pos = 0;
    put_bits(data_cw, &bit_pos, 0x4, 4); // 字节模式指示符 0100
    put_bits(data_cw, &bit_pos, (uint32_t)len, count_bits(version));
    for (size_t i = 0; i < len; i++) put_bits(data_cw, &bit_pos, (uint8_t)text[i], 8);

    int cap_bits = total_data * 8;
    int term = cap_bits - (int)bit_pos;
    if (term > 4) term = 4;
    if (term > 0) put_bits(data_cw, &bit_pos, 0, term);
    while (bit_pos % 8 != 0) put_bits(data_cw, &bit_pos, 0, 1);
    uint8_t pad = 0xEC;
    while ((int)bit_pos < cap_bits) {
        put_bits(data_cw, &bit_pos, pad, 8);
        pad = (pad == 0xEC) ? 0x11 : 0xEC;
    }

    // 2. 分块计算纠错码字。
    uint8_t block_data[10][44];
    uint8_t block_ec[10][26];
    uint8_t gen[26];
    rs_divisor(ec_len, gen);

    int offset = 0;
    for (int b = 0; b < blocks; b++) {
        int dl = (b < vi->g1_blocks) ? vi->g1_data : vi->g2_data;
        memcpy(block_data[b], data_cw + offset, (size_t)dl);
        offset += dl;
        rs_remainder(block_data[b], dl, gen, ec_len, block_ec[b]);
    }

    // 3. 交织：先逐列交错各块的数据码字，再逐列交错纠错码字。
    uint8_t codewords[346];
    int k = 0;
    int max_dl = (vi->g2_blocks > 0) ? vi->g2_data : vi->g1_data;
    for (int i = 0; i < max_dl; i++) {
        for (int b = 0; b < blocks; b++) {
            int dl = (b < vi->g1_blocks) ? vi->g1_data : vi->g2_data;
            if (i < dl) codewords[k++] = block_data[b][i];
        }
    }
    for (int i = 0; i < ec_len; i++) {
        for (int b = 0; b < blocks; b++) codewords[k++] = block_ec[b][i];
    }
    int total_cw = k;

    // 4. 铺设矩阵。
    uint8_t is_func[APP_QR_MAX_SIZE * APP_QR_MAX_SIZE];
    memset(modules, 0, (size_t)size * (size_t)size);
    memset(is_func, 0, sizeof(is_func));

    draw_function_patterns(modules, is_func, size, version);
    place_codewords(modules, is_func, size, codewords, total_cw);

    // 5. 评估全部 8 个掩码，取惩罚分最低者。
    int best_mask = 0;
    int best_penalty = -1;
    for (int mask = 0; mask < 8; mask++) {
        apply_mask(modules, is_func, size, mask);
        draw_format_bits(modules, is_func, size, mask);
        int penalty = penalty_score(modules, size);
        if (best_penalty < 0 || penalty < best_penalty) {
            best_penalty = penalty;
            best_mask = mask;
        }
        apply_mask(modules, is_func, size, mask); // 撤销
    }
    apply_mask(modules, is_func, size, best_mask);
    draw_format_bits(modules, is_func, size, best_mask);

    *size_out = size;
    return true;
}
