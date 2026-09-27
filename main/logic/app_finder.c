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

// ---------------------------------------------------------------------------
// 设备类别（启发式）
// ---------------------------------------------------------------------------
// 这里用到的一切标识都是公开、可核实的：
//  - 厂商 ID（Company Identifier）来自 Bluetooth SIG 的公开分配表；
//  - 16 位服务 UUID 是 Bluetooth SIG 定义的标准服务；
//  - 名称线索是设备自己在广播里填的字符串，任何人都可以随便写。
//
// 之所以把每条规则都写得这么保守：广播内容只是"设备愿意公开的一点点信息"，
// 手机还会随机化 MAC、大多数外设根本不广播名字。任何一条线索命中都只能说明
// "它像这一类"，不足以认定它到底是什么、属于谁。需要多个条件互相印证时，
// 就保持 UNKNOWN，绝不为了填满列表而瞎猜。
//
// 厂商 ID（小端 16 位，来自广播的厂商自定义数据前两字节）。
#define APP_FINDER_CO_APPLE    0x004C
#define APP_FINDER_CO_MICROSOFT 0x0006
#define APP_FINDER_CO_SAMSUNG  0x0075
#define APP_FINDER_CO_GOOGLE   0x00E0

// 标准服务 UUID。
#define APP_FINDER_UUID_FASTPAIR 0xFE2C   // Google Fast Pair：耳机/音箱等音频配件
#define APP_FINDER_UUID_HID      0x1812   // Human Interface Device：键鼠/手柄等外设
#define APP_FINDER_UUID_HEART    0x180D   // Heart Rate：心率带/手表
#define APP_FINDER_UUID_BATTERY  0x180F   // Battery：过于通用，单独出现时不做定性

static int ascii_lower(int c)
{
    // 只折叠 ASCII 字母：名称线索可能是中英混排，UTF-8 多字节原样比较。
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

// 大小写无关的子串匹配（对 UTF-8 字节做原样匹配，因此中文关键词也能用）。
static bool name_has(const char *name, int name_len, const char *kw)
{
    if (!name || name_len <= 0 || !kw || !kw[0]) return false;
    int klen = (int)strlen(kw);
    if (klen > name_len) return false;
    for (int i = 0; i + klen <= name_len; i++) {
        int j = 0;
        while (j < klen && ascii_lower((unsigned char)name[i + j]) ==
                              ascii_lower((unsigned char)kw[j])) {
            j++;
        }
        if (j == klen) return true;
    }
    return false;
}

static bool name_has_any(const char *name, int name_len, const char *const *kws, int n)
{
    for (int i = 0; i < n; i++) {
        if (name_has(name, name_len, kws[i])) return true;
    }
    return false;
}

static bool uuid16_has(const uint16_t *uuid16, int count, uint16_t want)
{
    if (!uuid16) return false;
    for (int i = 0; i < count; i++) {
        if (uuid16[i] == want) return true;
    }
    return false;
}

app_finder_cat_t app_finder_classify(uint16_t company_id,
                                     const uint16_t *uuid16, int uuid16_count,
                                     const char *name, int name_len)
{
    // 规则一（最优先）：名称线索。名字是设备主动自报的，通常最具体；但它同样可以被
    // 随意伪造，所以只当作"像什么"的线索，不当作身份。
    // 耳机类：各家耳机名里几乎都会出现这些词。
    static const char *const kw_earbuds[] = {
        "airpod", "buds", "earbud", "headphone", "headset", "earfree",
        "耳机", "耳麦", "耳塞",
    };
    if (name_has_any(name, name_len, kw_earbuds,
                     (int)(sizeof(kw_earbuds) / sizeof(kw_earbuds[0])))) {
        return APP_FINDER_CAT_EARBUDS;
    }
    // 音箱/音频类（不带"耳"的字样，通常是外放设备）。
    static const char *const kw_audio[] = {
        "speaker", "soundbar", "sound link", "soundlink", "jbl", "bose",
        "音箱", "音响",
    };
    if (name_has_any(name, name_len, kw_audio,
                     (int)(sizeof(kw_audio) / sizeof(kw_audio[0])))) {
        return APP_FINDER_CAT_AUDIO;
    }
    // 手表/手环类。
    static const char *const kw_watch[] = {
        "watch", "band", "fitbit", "garmin",
        "手表", "手环",
    };
    if (name_has_any(name, name_len, kw_watch,
                     (int)(sizeof(kw_watch) / sizeof(kw_watch[0])))) {
        return APP_FINDER_CAT_WATCH;
    }
    // 防丢追踪器类。
    static const char *const kw_tracker[] = {
        "airtag", "smarttag", "tile", "tracker", "findmy", "lost",
        "追踪", "防丢",
    };
    if (name_has_any(name, name_len, kw_tracker,
                     (int)(sizeof(kw_tracker) / sizeof(kw_tracker[0])))) {
        return APP_FINDER_CAT_TRACKER;
    }
    // 电脑类：只有明确出现"电脑"字样的才归电脑，避免"mac"误伤其它名字。
    static const char *const kw_computer[] = {
        "macbook", "laptop", "notebook", "surface", "thinkpad", "desktop",
        "电脑", "笔记本",
    };
    if (name_has_any(name, name_len, kw_computer,
                     (int)(sizeof(kw_computer) / sizeof(kw_computer[0])))) {
        return APP_FINDER_CAT_COMPUTER;
    }

    // 规则二：标准服务 UUID。这些 UUID 的含义由 Bluetooth SIG 规定，比名字可信，
    // 但设备同样可以广播不属于自己的服务 UUID，所以仍是"像什么"而非身份。
    // Fast Pair 是 Google 为音频配件设计的快速配对协议，命中即偏向耳机/音频。
    if (uuid16_has(uuid16, uuid16_count, APP_FINDER_UUID_FASTPAIR)) {
        return APP_FINDER_CAT_EARBUDS;
    }
    // 心率服务基本只出现在心率带/运动手表上。
    if (uuid16_has(uuid16, uuid16_count, APP_FINDER_UUID_HEART)) {
        return APP_FINDER_CAT_WATCH;
    }
    // 电池服务（0x180F）几乎人人都有，单独出现说明不了任何类别，因此不参与判断——
    // 这也是"保守"的体现：宁可留 UNKNOWN，也不用一条无用线索硬凑一个答案。

    // 规则三：厂商 ID。厂商 ID 只说明"谁注册的广播格式"，同一家厂商既有手机也有
    // 耳机、手表、追踪器，所以这条线索最弱，放在最后。
    switch (company_id) {
    case APP_FINDER_CO_APPLE:
        // Apple 的自定义广播大量来自 iPhone，但也可能是 AirPods/手表/AirTag；
        // 在没有更具体线索时归为手机是概率上的多数，仍只作类别参考。
        return APP_FINDER_CAT_PHONE;
    case APP_FINDER_CO_SAMSUNG:
    case APP_FINDER_CO_GOOGLE:
        return APP_FINDER_CAT_PHONE;
    case APP_FINDER_CO_MICROSOFT:
        // 微软的自定义广播更常出现在 Windows 电脑/配件上。
        return APP_FINDER_CAT_COMPUTER;
    default:
        break;
    }

    // 规则四：HID 服务说明它是键鼠/手柄类外设。这类设备可能与电脑配对，但设备本身
    // 是外设而非电脑，因此归入"其它"，不冒充"电脑"。
    if (uuid16_has(uuid16, uuid16_count, APP_FINDER_UUID_HID)) {
        return APP_FINDER_CAT_OTHER;
    }

    // 信息不足：诚实地说不知道。
    return APP_FINDER_CAT_UNKNOWN;
}

const char *app_finder_category_text(app_finder_cat_t c)
{
    switch (c) {
    case APP_FINDER_CAT_PHONE:    return "手机";
    case APP_FINDER_CAT_EARBUDS:  return "耳机";
    case APP_FINDER_CAT_WATCH:    return "手表";
    case APP_FINDER_CAT_TRACKER:  return "追踪器";
    case APP_FINDER_CAT_AUDIO:    return "音频";
    case APP_FINDER_CAT_COMPUTER: return "电脑";
    case APP_FINDER_CAT_OTHER:    return "其它";
    case APP_FINDER_CAT_UNKNOWN:
    case APP_FINDER_CAT_COUNT:
    default:                      return "未知";
    }
}

void app_finder_category_counts(const app_finder_t *f, int out[APP_FINDER_CAT_COUNT])
{
    if (!out) return;
    for (int i = 0; i < APP_FINDER_CAT_COUNT; i++) out[i] = 0;
    if (!f) return;
    for (int i = 0; i < f->count; i++) {
        app_finder_cat_t c = f->devs[i].category;
        // 结构体可能被外部按旧格式/脏数据填充，越界值一律计入 UNKNOWN，绝不越界写。
        if (c < 0 || c >= APP_FINDER_CAT_COUNT) c = APP_FINDER_CAT_UNKNOWN;
        out[c]++;
    }
}

int app_finder_feed(app_finder_t *f, const uint8_t addr[6],
                    uint16_t company_id, const uint16_t *uuid16, int uuid16_count,
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

    // 类别用本次广播的线索重新推断。广播是分片发送的：名字、厂商数据、服务 UUID
    // 往往不在同一条报文里，因此"这次推断不出来"不代表之前的判断失效——只在推断
    // 到已知类别时才覆盖，避免列表里的类别在相邻两次刷新间忽有忽无地闪烁。
    app_finder_cat_t cat = app_finder_classify(company_id, uuid16, uuid16_count,
                                               name, name_len);
    if (cat != APP_FINDER_CAT_UNKNOWN) f->devs[idx].category = cat;

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