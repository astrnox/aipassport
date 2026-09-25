// main/logic/app_badge.h —— 电子工牌的数据模型（与硬件无关）。
//
// 本层不依赖 ESP-IDF 与 LVGL，可在主机上直接编译测试。配置页负责编辑文本，界面层
// 负责渲染，二者共用这里的截断与校验逻辑。
//
// 一张工牌可以带多个二维码：校园场景里"身份码"和"支付码"常常要并排使用，退出一个
// 再进另一个很别扭。上限取 3——再多就超出一屏可扫的尺寸，实际体验反而更差。
#pragma once

#include "app_anim.h"

#include <stdbool.h>
#include <stddef.h>

#define APP_BADGE_MAX          5
#define APP_BADGE_MAX_LINES    4
#define APP_BADGE_QR_MAX       3
#define APP_BADGE_QR_TEXT_LEN  128   // 与 APP_QR_MAX_BYTES(120) 对齐，留终止符余量
#define APP_BADGE_QR_LABEL_LEN 12    // 二维码短标签，如"身份码"

typedef struct {
    char nickname[24];                      // UTF-8，最多 8 个汉字
    char lines[APP_BADGE_MAX_LINES][28];    // 每行最多 12 个汉字；空行不显示
    char qr_label[APP_BADGE_QR_MAX][APP_BADGE_QR_LABEL_LEN];
    char qr_text[APP_BADGE_QR_MAX][APP_BADGE_QR_TEXT_LEN];
    int  qr_count;                          // 0..APP_BADGE_QR_MAX
    int  anim_slot;                         // 动图槽位；-1 表示不带动图
    bool used;
} app_badge_t;

typedef struct {
    app_badge_t items[APP_BADGE_MAX];
    int count;
    int selected;
} app_badge_list_t;

void app_badge_list_init(app_badge_list_t *l);
int  app_badge_add(app_badge_list_t *l);              // 新索引；满返回 -1
bool app_badge_remove(app_badge_list_t *l, int index);
void app_badge_cycle(app_badge_list_t *l, int delta); // 循环切换 selected
bool app_badge_set_text(app_badge_t *b, const char *nickname,
                        const char *const *lines, int line_count);

// ---- 二维码 ----
// 追加一个二维码。text 为空、非法 UTF-8、或已满时返回 -1。
// label 可以为 NULL（显示时回退成"二维码 N"）。
int  app_badge_qr_add(app_badge_t *b, const char *label, const char *text);
bool app_badge_qr_remove(app_badge_t *b, int index);
bool app_badge_qr_set(app_badge_t *b, int index, const char *label, const char *text);
// 有效二维码条数（按 qr_count 计，不扫描内容）。
int  app_badge_qr_count(const app_badge_t *b);
// 第 index 个二维码内容；越界或为空返回 NULL。
const char *app_badge_qr_text(const app_badge_t *b, int index);
// 第 index 个二维码的显示名；越界返回 NULL，label 为空时返回 "二维码 N"。
const char *app_badge_qr_label(const app_badge_t *b, int index, char *scratch,
                               size_t scratch_cap);

// ---- 动图 ----
// slot 取 -1（清除）或 0..APP_ANIM_SLOT_MAX-1。越界返回 false。
bool app_badge_set_anim(app_badge_t *b, int slot);
