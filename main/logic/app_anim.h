// main/logic/app_anim.h —— 名片动图的数据模型（不依赖 ESP-IDF 与 LVGL）。
//
// 为什么设备端不解析 GIF：本机无 PSRAM、单 DMA 缓冲，LVGL 的 GIF 解码器需要把
// 整个文件与逐帧画布放进内存，尺寸稍大就会挤爆 LVGL 堆。因此约定由手机浏览器
// 解码并缩放，设备只接收定宽定高的 RGB565 帧序列顺序播放——解码成本挪到了算力
// 充裕的一端，设备端只剩"按顺序换一张图"。
//
// 帧格式：RGB565，小端，每像素 2 字节。手机端写入的数值必须是
//   value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
// 并以小端字节序落到缓冲区，这样设备端可以把它当作 lv_color16_t 直接渲染。
//
// 本文件只做纯计算与校验，因此可以在主机上直接编译测试；真正读写闪存的是
// main/app_assets.c。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_ANIM_SLOT_MAX    6      // assets 分区划分成多少个槽位
#define APP_ANIM_MAX_FRAMES  24
#define APP_ANIM_MAX_SIDE    96     // 单边像素上限；96x96x24 帧已是 432KB
#define APP_ANIM_NAME_LEN    16
#define APP_ANIM_HEADER_SIZE 48
#define APP_ANIM_VERSION     1
#define APP_ANIM_MAGIC       0x4D4E4141u   // "AANM"

// 帧间隔（毫秒）。GIF 允许每帧不同延时，但设备端只需要"看起来在动"，因此手机端按
// 各帧延时的平均值折算成一个统一值，省掉一张每帧延时表。
#define APP_ANIM_FRAME_MS_DEFAULT 100
#define APP_ANIM_FRAME_MS_MIN     40
#define APP_ANIM_FRAME_MS_MAX     1000

typedef enum {
    APP_ANIM_OK = 0,
    APP_ANIM_ERR_EMPTY,      // 没有帧
    APP_ANIM_ERR_FRAMES,     // 帧数越界
    APP_ANIM_ERR_SIDE,       // 宽或高越界
    APP_ANIM_ERR_BYTES,      // 数据长度与"宽 x 高 x 帧数"不一致
    APP_ANIM_ERR_HEADER,     // 头部自相矛盾（magic/版本/CRC 不符）
} app_anim_status_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint16_t frame_ms;       // 0 表示用默认值
} app_anim_meta_t;

// 槽位头部。字段按 4 字节自然对齐排列，整体 48 字节，于是紧随其后的帧数据也落在
// 4 字节边界上。header_crc 放在 40 字节处，覆盖它前面的全部内容，而不像把 CRC 放
// 开头那样只覆盖几个字段。
typedef struct {
    uint32_t magic;         //  0
    uint32_t data_crc;      //  4 帧数据的 CRC-32
    uint32_t data_bytes;    //  8 帧数据总字节数
    uint16_t version;       // 12
    uint16_t width;         // 14
    uint16_t height;        // 16
    uint16_t frame_count;   // 18
    uint16_t frame_bytes;   // 20 单帧字节数，冗余字段，用于交叉校验
    uint16_t frame_ms;      // 22 每帧停留毫秒（0 表示用默认值）
    char     name[APP_ANIM_NAME_LEN];   // 24..39
    uint32_t header_crc;    // 40 覆盖 0..39
    uint8_t  pad[4];        // 44..47
} app_anim_header_t;

// 单帧字节数。
uint32_t app_anim_frame_bytes(int width, int height);
// 全部帧的字节数。
uint32_t app_anim_data_bytes(int width, int height, int frame_count);
// 校验一组上传参数；payload_bytes 是手机声明要发送的字节数。
app_anim_status_t app_anim_validate(int width, int height, int frame_count,
                                    uint32_t payload_bytes);
// 标准 IEEE CRC-32（多项式 0xEDB88320，初值/末值取反）。逐位实现，不建表：调用
// 频率低（一次上传一次、装动图时一次），换来的是无静态状态、无并发问题。
uint32_t app_anim_crc32(const void *data, size_t len);

// 增量版本，供流式上传使用：begin() 取初值，update() 逐段喂入，finish() 出结果。
// 一次算完等价于：
//   app_anim_crc32_finish(app_anim_crc32_update(app_anim_crc32_begin(), data, len))
uint32_t app_anim_crc32_begin(void);
uint32_t app_anim_crc32_update(uint32_t state, const void *data, size_t len);
uint32_t app_anim_crc32_finish(uint32_t state);

// 组装头部（含 header_crc）。name 可为 NULL。
void app_anim_header_pack(app_anim_header_t *header, const app_anim_meta_t *meta,
                          const char *name, uint32_t data_crc, uint32_t data_bytes);
// 校验头部自洽性；不校验帧数据 CRC（那需要读完整块数据，由调用方决定何时做）。
app_anim_status_t app_anim_header_check(const app_anim_header_t *header);
// 头部声明的帧间隔，已夹到 [MIN, MAX]；字段为 0（旧数据或未填）时取默认值。
uint32_t app_anim_frame_ms_get(const app_anim_header_t *header);
// 把任意输入夹到合法帧间隔。
uint16_t app_anim_frame_ms_clamp(int frame_ms);
// 取出槽位名并保证以 NUL 结尾（闪存里可能没有终止符，也可能不是合法 UTF-8 边界）。
void app_anim_name_copy(const app_anim_header_t *header, char *out, size_t out_size);
// 状态码的可读文本，用于界面与日志。
const char *app_anim_status_text(app_anim_status_t status);
