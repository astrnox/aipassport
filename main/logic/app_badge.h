// main/logic/app_badge.h —— 电子工牌的数据模型（与硬件无关）。
//
// 本层不依赖 ESP-IDF 与 LVGL，可在主机上直接编译测试。配置页负责编辑文本，
// 界面层负责渲染，二者共用这里的截断与校验逻辑。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_BADGE_MAX        5
#define APP_BADGE_MAX_LINES  4

typedef struct {
    char nickname[24];                      // UTF-8，最多 8 个汉字
    char lines[APP_BADGE_MAX_LINES][28];    // 每行最多 12 个汉字；空行不显示
    char qr_text[128];                      // 二维码内容
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
