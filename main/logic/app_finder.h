// main/logic/app_finder.h —— 蓝牙查找器的纯逻辑：附近设备表、信号平滑、收藏（"我的设备"）。
//
// 不碰 NimBLE、不碰界面：把"收到一条广播"当成普通输入，把"现在屏幕上该显示哪几台、
// 信号算多近"当成普通输出，所以整条查找流程可以在主机上验证，不需要真的拿蓝牙耳机丢一次。
//
// 信号强度用 dBm（负值，越大越强）。用户不需要看懂 dBm，因此这里额外提供
// app_finder_closeness()：把 dBm 映射成 0..100 的"接近度"和一句人话档位。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 屏幕上最多同时列这么多台。再多也翻不完，且每台都要占 RAM。
#define APP_FINDER_MAX 16
// 设备名缓冲。BLE 广播里的名字是 UTF-8，超长截断（截断点可能切断多字节字符，
// 界面按字节显示顶多出现一个方块，不影响其它条目）。
#define APP_FINDER_NAME_LEN 24
// "我的设备"最多存这么多。存的是地址+名字，重启后仍能按名字找回。
#define APP_FINDER_SAVED_MAX 8
// 超过这个时间没再收到广播就从列表里去掉。BLE 广播间隔通常远小于 1 秒，
// 6 秒足够滤掉"已经走开/关机"的设备，又不至于让列表频繁闪烁。
#define APP_FINDER_TTL_MS 6000

// 接近度的映射区间：-40 dBm 视为贴脸（100），-100 dBm 视为快搜不到（0）。
#define APP_FINDER_RSSI_NEAR (-40)
#define APP_FINDER_RSSI_FAR  (-100)

// 设备类别：依据广播里可核实的公开标识（厂商 ID、标准服务 UUID、名称线索）做的
// 保守归类，只用来帮用户从一堆"未知设备"里快速缩小范围。这是一个启发式的"像什么"，
// 不是身份识别：手机 MAC 会随机化、多数设备根本不广播名字，因此绝不能据此声称
// 设备属于谁。拿不准时一律归入 UNKNOWN。
typedef enum {
    APP_FINDER_CAT_UNKNOWN = 0,   // 信息不足，无法归类
    APP_FINDER_CAT_PHONE,
    APP_FINDER_CAT_EARBUDS,
    APP_FINDER_CAT_WATCH,
    APP_FINDER_CAT_TRACKER,
    APP_FINDER_CAT_AUDIO,
    APP_FINDER_CAT_COMPUTER,
    APP_FINDER_CAT_OTHER,         // 能确定是人造蓝牙设备，但不属于以上任何一类
    APP_FINDER_CAT_COUNT
} app_finder_cat_t;

typedef struct {
    uint8_t  addr[6];
    char     name[APP_FINDER_NAME_LEN];
    bool     has_name;
    app_finder_cat_t category;   // 启发式类别（见 app_finder_classify）
    int      rssi;         // 平滑后的信号强度，用于显示
    int      raw_rssi;     // 最近一次原始值，便于判断"是不是在动"
    uint64_t last_ms;      // 最近一次收到广播的时刻
} app_finder_dev_t;

typedef struct {
    app_finder_dev_t devs[APP_FINDER_MAX];
    int              count;

    // 收藏的"我的设备"。只存地址与名字：具体信号由扫描实时填。
    uint8_t saved_addr[APP_FINDER_SAVED_MAX][6];
    char    saved_name[APP_FINDER_SAVED_MAX][APP_FINDER_NAME_LEN];
    int     saved_count;
} app_finder_t;

// 全部清零（含收藏）。载入 NVS 之前先调用。
void app_finder_init(app_finder_t *f);
// 只清附近设备表，保留收藏。每次重新开始扫描时调用。
void app_finder_clear_devices(app_finder_t *f);

// 收到一条广播。同一地址会更新平滑信号并把 last_ms 推到现在；新地址插入表中。
// 表满且是新地址时，淘汰 last_ms 最旧的一台。name 为 NULL 或 name_len<=0 表示本次
// 广播没带名字：保留已有的名字（名字常在后续广播里补发），从未有过名字则记为无名字。
// company_id 为广播里的厂商 ID（0 表示本次没带厂商数据）；uuid16/uuid16_count 为本次
// 广播里的 16 位服务 UUID 列表（可为 NULL / 0）。这些输入用来推断类别，规则同样允许
// 后续广播把类别从"未知"更新成已知（见 app_finder_classify）。
// 返回该设备在表中的下标，插入失败（表满且无法淘汰）返回 -1。
int app_finder_feed(app_finder_t *f, const uint8_t addr[6],
                    uint16_t company_id, const uint16_t *uuid16, int uuid16_count,
                    const char *name, int name_len, int rssi, uint64_t now_ms);

// 依据广播内容保守地推断设备类别。只使用可核实的公开标识；信息不足返回
// APP_FINDER_CAT_UNKNOWN。这是启发式归类而非身份识别，详见实现里的逐条注释。
app_finder_cat_t app_finder_classify(uint16_t company_id,
                                     const uint16_t *uuid16, int uuid16_count,
                                     const char *name, int name_len);

// 类别的中文短标签（"手机""耳机"…）。传入越界值返回"未知"。
const char *app_finder_category_text(app_finder_cat_t c);

// 统计当前设备表里各类别的数量。out 长度须为 APP_FINDER_CAT_COUNT，先整体清零。
void app_finder_category_counts(const app_finder_t *f, int out[APP_FINDER_CAT_COUNT]);

// 去掉超过 APP_FINDER_TTL_MS 未出现的设备。
void app_finder_prune(app_finder_t *f, uint64_t now_ms);

// 按信号从强到弱排序，便于界面直接按顺序显示。
void app_finder_sort(app_finder_t *f);

// 按地址查下标，找不到返回 -1。
int app_finder_find(const app_finder_t *f, const uint8_t addr[6]);

// 把信号强度映射成 0..100 的接近度。数值越大表示越靠近。
int app_finder_closeness(int rssi);

// 接近度对应的人话档位，用于界面文字（"就在附近" 等）。
const char *app_finder_level(int closeness);

// ---- 收藏 ----
// 保存一台设备为"我的设备"。已保存则只更新名字并返回原下标；已满返回 -1。
int  app_finder_save(app_finder_t *f, const uint8_t addr[6], const char *name);
// 取消收藏。返回是否真的删掉了一条。
bool app_finder_unsave(app_finder_t *f, const uint8_t addr[6]);
int  app_finder_saved_index(const app_finder_t *f, const uint8_t addr[6]);
bool app_finder_is_saved(const app_finder_t *f, const uint8_t addr[6]);
int  app_finder_saved_count(const app_finder_t *f);
// 读取第 index 条收藏。写入 addr（6 字节）与名字；name 缓冲不足时截断。
bool app_finder_saved_at(const app_finder_t *f, int index, uint8_t addr[6],
                         char *name, size_t name_cap);

// ---- 收藏持久化 ----
// 打包成固定格式的字节流，便于直接写进 NVS。返回写入长度，缓冲区不足返回 0。
size_t app_finder_saved_serialize(const app_finder_t *f, uint8_t *out, size_t cap);
// 从字节流恢复收藏。magic / 版本 / 长度任一不符返回 false，且不改动 f 的收藏。
bool   app_finder_saved_deserialize(app_finder_t *f, const uint8_t *in, size_t len);