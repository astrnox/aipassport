// main/logic/app_finder.c —— 蓝牙查找器纯逻辑实现（详见 app_finder.h）。
#include "app_finder.h"

#include <string.h>

// 收藏字节流的头部：magic 用来拒绝随机/损坏数据，版本号用于以后改格式时安全丢弃旧数据。
#define FINDER_SAVED_MAGIC   0xA7
#define FINDER_SAVED_VERSION 1

// 平滑系数：新值占 1/4。BLE 的 RSSI 抖动可达十几 dB，直接显示会让"接近度"条乱跳；
// 取 1/4 既能跟上走动速度，又能把单次异常值压下去。
#define FINDER_SMOOTH_SHIFT 2

static void copy_name(char *dst, size_t cap, const char *name, int name_len)
{
    if (cap == 0) return;
    dst[0] = '\0';
    if (!name || name_len <= 0) return;

    size_t n = (size_t)name_len;
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, name, n);
    dst[n] = '\0';
}

void app_finder_init(app_finder_t *f)
{
    if (!f) return;
    memset(f, 0, sizeof(*f));
}

void app_finder_clear_devices(app_finder_t *f)
{
    if (!f) return;
    memset(f->devs, 0, sizeof(f->devs));
    f->count = 0;
}

int app_finder_find(const app_finder_t *f, const uint8_t addr[6])
{
    if (!f || !addr) return -1;
    for (int i = 0; i < f->count; i++) {
        if (memcmp(f->devs[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

// 表满时挑一台最久没出现的淘汰。返回其下标；表为空返回 -1。
static int find_oldest(const app_finder_t *f)
{
    int oldest = -1;
    for (int i = 0; i < f->count; i++) {
        if (oldest < 0 || f->devs[i].last_ms < f->devs[oldest].last_ms) oldest = i;
    }
    return oldest;
}

int app_finder_feed(app_finder_t *f, const uint8_t addr[6],
                    const char *name, int name_len, int rssi, uint64_t now_ms)
{
    if (!f || !addr) return -1;

    int idx = app_finder_find(f, addr);
    if (idx < 0) {
        if (f->count >= APP_FINDER_MAX) {
            // 表满：淘汰最旧的一台。淘汰而不是丢弃新设备，否则用户走到新设备旁边
            // 反而看不到它，只能等旧设备自己超时——那是明显的反直觉。
            idx = find_oldest(f);
            if (idx < 0) return -1;
            memset(&f->devs[idx], 0, sizeof(f->devs[idx]));
        } else {
            idx = f->count++;
            memset(&f->devs[idx], 0, sizeof(f->devs[idx]));
        }
        memcpy(f->devs[idx].addr, addr, 6);
        f->devs[idx].rssi = rssi;
    } else {
        // 指数平滑：新值占 1/4，其余沿用旧值。
        int prev = f->devs[idx].rssi;
        f->devs[idx].rssi = prev + ((rssi - prev) >> FINDER_SMOOTH_SHIFT);
    }

    f->devs[idx].raw_rssi = rssi;
    f->devs[idx].last_ms = now_ms;

    // 广播可能分几次把名字发全：只要这次带了名字就覆盖，没带就保留旧的。
    if (name && name_len > 0) {
        copy_name(f->devs[idx].name, sizeof(f->devs[idx].name), name, name_len);
        f->devs[idx].has_name = true;
    }
    return idx;
}

void app_finder_prune(app_finder_t *f, uint64_t now_ms)
{
    if (!f) return;
    int w = 0;
    for (int i = 0; i < f->count; i++) {
        if (now_ms > f->devs[i].last_ms &&
            now_ms - f->devs[i].last_ms > APP_FINDER_TTL_MS) {
            continue;   // 超时：丢弃
        }
        if (w != i) f->devs[w] = f->devs[i];
        w++;
    }
    f->count = w;
}

void app_finder_sort(app_finder_t *f)
{
    if (!f) return;
    // 插入排序：条数上限 16，简单且不引入 qsort 的比较函数开销。
    for (int i = 1; i < f->count; i++) {
        app_finder_dev_t key = f->devs[i];
        int j = i - 1;
        while (j >= 0 && f->devs[j].rssi < key.rssi) {
            f->devs[j + 1] = f->devs[j];
            j--;
        }
        f->devs[j + 1] = key;
    }
}

int app_finder_closeness(int rssi)
{
    if (rssi >= APP_FINDER_RSSI_NEAR) return 100;
    if (rssi <= APP_FINDER_RSSI_FAR) return 0;
    int range = APP_FINDER_RSSI_NEAR - APP_FINDER_RSSI_FAR;   // 60
    return (rssi - APP_FINDER_RSSI_FAR) * 100 / range;
}

const char *app_finder_level(int closeness)
{
    if (closeness >= 80) return "就在附近";
    if (closeness >= 55) return "很近";
    if (closeness >= 30) return "不太远";
    if (closeness >= 12) return "有点远";
    return "信号很弱";
}

// ---------------------------------------------------------------------------
// 收藏
// ---------------------------------------------------------------------------

int app_finder_saved_index(const app_finder_t *f, const uint8_t addr[6])
{
    if (!f || !addr) return -1;
    for (int i = 0; i < f->saved_count; i++) {
        if (memcmp(f->saved_addr[i], addr, 6) == 0) return i;
    }
    return -1;
}

bool app_finder_is_saved(const app_finder_t *f, const uint8_t addr[6])
{
    return app_finder_saved_index(f, addr) >= 0;
}

int app_finder_saved_count(const app_finder_t *f)
{
    return f ? f->saved_count : 0;
}

int app_finder_save(app_finder_t *f, const uint8_t addr[6], const char *name)
{
    if (!f || !addr) return -1;

    int idx = app_finder_saved_index(f, addr);
    if (idx < 0) {
        if (f->saved_count >= APP_FINDER_SAVED_MAX) return -1;
        idx = f->saved_count++;
        memcpy(f->saved_addr[idx], addr, 6);
        f->saved_name[idx][0] = '\0';
    }
    // 名字取显式传入的；没有则沿用旧名字，避免"再次保存"把名字清空。
    if (name && name[0]) {
        copy_name(f->saved_name[idx], sizeof(f->saved_name[idx]), name, (int)strlen(name));
    }
    return idx;
}

bool app_finder_unsave(app_finder_t *f, const uint8_t addr[6])
{
    int idx = app_finder_saved_index(f, addr);
    if (idx < 0) return false;

    for (int i = idx; i + 1 < f->saved_count; i++) {
        memcpy(f->saved_addr[i], f->saved_addr[i + 1], 6);
        memcpy(f->saved_name[i], f->saved_name[i + 1], APP_FINDER_NAME_LEN);
    }
    f->saved_count--;
    memset(f->saved_addr[f->saved_count], 0, 6);
    f->saved_name[f->saved_count][0] = '\0';
    return true;
}

bool app_finder_saved_at(const app_finder_t *f, int index, uint8_t addr[6],
                         char *name, size_t name_cap)
{
    if (!f || index < 0 || index >= f->saved_count) return false;
    if (addr) memcpy(addr, f->saved_addr[index], 6);
    if (name && name_cap > 0) {
        strncpy(name, f->saved_name[index], name_cap - 1);
        name[name_cap - 1] = '\0';
    }
    return true;
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------
// 格式：magic(1) + version(1) + count(1) + 每条 [addr(6) + name(24)]。
// 定长条目让解析不需要长度前缀，也不依赖结构体布局（结构体有对齐填充，直接 memcpy
// 会在换编译器/换架构后读出垃圾）。

#define FINDER_SAVED_ENTRY_LEN (6 + APP_FINDER_NAME_LEN)
#define FINDER_SAVED_HEADER_LEN 3

size_t app_finder_saved_serialize(const app_finder_t *f, uint8_t *out, size_t cap)
{
    if (!f || !out) return 0;

    int count = f->saved_count;
    if (count < 0) count = 0;
    if (count > APP_FINDER_SAVED_MAX) count = APP_FINDER_SAVED_MAX;

    size_t need = FINDER_SAVED_HEADER_LEN + (size_t)count * FINDER_SAVED_ENTRY_LEN;
    if (cap < need) return 0;

    out[0] = FINDER_SAVED_MAGIC;
    out[1] = FINDER_SAVED_VERSION;
    out[2] = (uint8_t)count;

    size_t o = FINDER_SAVED_HEADER_LEN;
    for (int i = 0; i < count; i++) {
        memcpy(out + o, f->saved_addr[i], 6);
        memset(out + o + 6, 0, APP_FINDER_NAME_LEN);
        strncpy((char *)(out + o + 6), f->saved_name[i], APP_FINDER_NAME_LEN - 1);
        o += FINDER_SAVED_ENTRY_LEN;
    }
    return need;
}

bool app_finder_saved_deserialize(app_finder_t *f, const uint8_t *in, size_t len)
{
    if (!f || !in) return false;
    if (len < FINDER_SAVED_HEADER_LEN) return false;
    if (in[0] != FINDER_SAVED_MAGIC || in[1] != FINDER_SAVED_VERSION) return false;

    int count = in[2];
    if (count > APP_FINDER_SAVED_MAX) return false;
    if (len < FINDER_SAVED_HEADER_LEN + (size_t)count * FINDER_SAVED_ENTRY_LEN) return false;

    // 先解析到临时数组，全部校验通过再覆盖：损坏的数据不该把已有收藏清成一半。
    uint8_t addrs[APP_FINDER_SAVED_MAX][6];
    char names[APP_FINDER_SAVED_MAX][APP_FINDER_NAME_LEN];

    size_t o = FINDER_SAVED_HEADER_LEN;
    for (int i = 0; i < count; i++) {
        memcpy(addrs[i], in + o, 6);
        memcpy(names[i], in + o + 6, APP_FINDER_NAME_LEN);
        names[i][APP_FINDER_NAME_LEN - 1] = '\0';
        o += FINDER_SAVED_ENTRY_LEN;
    }

    for (int i = 0; i < count; i++) {
        memcpy(f->saved_addr[i], addrs[i], 6);
        memcpy(f->saved_name[i], names[i], APP_FINDER_NAME_LEN);
    }
    for (int i = count; i < APP_FINDER_SAVED_MAX; i++) {
        memset(f->saved_addr[i], 0, 6);
        f->saved_name[i][0] = '\0';
    }
    f->saved_count = count;
    return true;
}