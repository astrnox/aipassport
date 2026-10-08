// main/logic/app_novel.h —— 离线小说阅读的纯逻辑（不依赖 ESP-IDF 与 LVGL）。
//
// 为什么单独一层：分页、章节识别、上传校验都是"给定一段正文算出结果"的纯计算，放在
// 这里就能在主机上穷举边界（半个汉字、跨段换行、超长英文串、没有章节标记的书），
// 不必依赖真机；真正读写闪存的是 main/app_novel.c，页面是 main/ui/ui_novel.c。
//
// 正文约定：UTF-8 纯文本。宽度按"半角单位"记账——半角字符 1 个单位、全角（CJK、
// 全角标点）2 个单位，制表符 4 个单位；这样分页只依赖一个整数预算，与具体字体
// 解耦，主机测试能完全确定地复现。'\r' 计 0 个单位（老式 CRLF 文本不清洗也不会打乱
// 排版），换行只认 '\n'。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_NOVEL_TITLE_LEN        32
#define APP_NOVEL_HEADER_SIZE      64
#define APP_NOVEL_VERSION          1
#define APP_NOVEL_MAGIC            0x314E5646u   // "FVN1"
#define APP_NOVEL_MAX_CHAPTERS     96
#define APP_NOVEL_CHAPTER_TITLE_LEN 32
#define APP_NOVEL_MAX_LINE         256

// 分区布局：头部(64B) -> 章节索引(最多 96 条，每条 36B = 3456B) -> 正文。
// 正文固定从 0x1000 开始：一来索引区有确定的余量（64 + 3456 < 4096），二来正文落在
// 扇区边界上，擦除与写入都按扇区对齐，不需要在容量计算里算边界。
#define APP_NOVEL_TEXT_OFFSET      0x1000u

// 一行宽度预算与一页行数的合理区间。界面按字号与区域算出实际值后传进来。
#define APP_NOVEL_MIN_UNITS_PER_LINE 4
#define APP_NOVEL_MAX_UNITS_PER_LINE 64
#define APP_NOVEL_MAX_LINES_PER_PAGE 32

typedef enum {
    APP_NOVEL_OK = 0,
    APP_NOVEL_ERR_HEADER,     // magic / 版本 / 长度自相矛盾
    APP_NOVEL_ERR_EMPTY,      // 正文为空
    APP_NOVEL_ERR_CAPACITY,   // 正文超过分区容量
} app_novel_status_t;

// 分区头部。字段按自然对齐排列，整体 64 字节，章节索引紧随其后。
// data_crc 由存储层用设备侧 CRC-32 计算与校验（本层不做 CRC，保持零依赖）。
typedef struct {
    uint32_t magic;          //  0
    uint32_t data_crc;       //  4 正文 CRC-32
    uint32_t data_bytes;     //  8 正文长度
    uint16_t version;        // 12
    uint16_t chapter_count;  // 14 章节索引条数（0 = 没有章节标记）
    char     title[APP_NOVEL_TITLE_LEN];   // 16..47
    uint8_t  pad[16];        // 48..63
} app_novel_header_t;

// 排版参数：一行多少单位宽、一页多少行。
typedef struct {
    int units_per_line;
    int lines_per_page;
} app_novel_layout_t;

// 一章的起点与标题。整条 36 字节，按顺序存放在头部之后的索引区里，设备端直接
// 从闪存映射读取，不占常驻内存。
typedef struct {
    uint32_t offset;                                   // 标题行首的字节偏移
    char     title[APP_NOVEL_CHAPTER_TITLE_LEN];        // UTF-8，已截断
} app_novel_chapter_t;

// 填头部（除 data_crc 之外的字段；data_crc 由调用方随后写入）。title 会按 UTF-8
// 安全截断到 APP_NOVEL_TITLE_LEN 以内；chapter_count 由扫描结果传入。
void app_novel_header_init(app_novel_header_t *header, const char *title,
                           uint32_t data_bytes, uint32_t data_crc, uint16_t chapter_count);

// 校验头部自洽性并把正文长度写回 *data_bytes（可为 NULL）。capacity 是分区能容纳的
// 正文上限。任何一项不成立都返回对应错误，界面据此区分"没上传过"与"数据损坏"。
app_novel_status_t app_novel_header_check(const app_novel_header_t *header, uint32_t capacity,
                                          uint32_t *data_bytes);

// 一个码点占多少个半角单位。'\r' 为 0；制表符按 4；其余全角 >= 0x1100 的宽字符按 2。
int app_novel_char_units(uint32_t cp);

// 从 off 开始那一页的结束偏移，也就是下一页的起点；off 越界时返回 len。
// 换行符被这一页"吃掉"，每一页都从字符边界开始，不会把汉字劈成两半。
size_t app_novel_page_next(const char *text, size_t len, size_t off,
                           const app_novel_layout_t *layout);

// off 所在页的起始偏移（用于往回翻页）。off 为 0 返回 0。
size_t app_novel_page_prev(const char *text, size_t len, size_t off,
                           const app_novel_layout_t *layout);

// 把任意偏移吸附到"包含它的那一页"的页首：恢复阅读进度、按百分比跳转都用它，
// 保证无论落到正文哪个字节，都从完整的一页开始显示。
size_t app_novel_page_floor(const char *text, size_t len, size_t off,
                            const app_novel_layout_t *layout);

// 解一个 UTF-8 码点并把 pos 推到下一个字符边界；非法序列返回 U+FFFD 且只前进一个
// 字节。界面按页拷贝正文、替换缺字都用它，避免在界面层再写一份解码。
uint32_t app_novel_utf8_next(const char *text, size_t len, size_t *pos);

// 找 from 之后（含 from）的第一处章节标题行：返回该行的字节偏移，并把标题写进
// title（UTF-8 截断到 title_cap 以内）。没有更多章节时返回 len。
// 上传时按这个接口逐条扫描写索引，因此不需要在内存里放整张章节表。
size_t app_novel_next_chapter(const char *text, size_t len, size_t from,
                              char *title, size_t title_cap);

// 上传期流式 UTF-8 校验：把一段字节喂进状态机，返回 false 表示出现了非法 UTF-8。
// state 必须从 0 开始，并在每次上传时复位；它记录"还差几个续字节"。
bool app_novel_utf8_feed(uint32_t *state, const uint8_t *data, size_t len);

// 上传结束时的收尾校验：state 必须回到 0（否则末尾是半个汉字）。
bool app_novel_utf8_finish(uint32_t state);