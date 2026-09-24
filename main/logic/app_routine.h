// main/logic/app_routine.h —— 作息表模型：节点、模板、导入与当前/下一节点计算。
//
// 本层不依赖 ESP-IDF 与 LVGL，可在主机上直接编译测试。界面层负责把节点画成
// 时间轴，配置页负责导入文本，二者共用这里的解析与状态计算。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_ROUTINE_MAX_NODES 24
#define APP_ROUTINE_DAYS      7

typedef enum {
    APP_NODE_ARRIVE = 0,   // 到校
    APP_NODE_CLASS,        // 上课
    APP_NODE_BREAK,        // 课间
    APP_NODE_LUNCH,        // 午休
    APP_NODE_STUDY,        // 晚自习
    APP_NODE_LEAVE,        // 放学
    APP_NODE_CUSTOM,       // 自定义
} app_node_type_t;

typedef struct {
    char            name[28];   // UTF-8，最多 8 个汉字
    int             start_min;  // 一天内分钟数 0..1440
    int             end_min;    // 必须 > start_min，且 <= 1440
    app_node_type_t type;
} app_routine_node_t;

typedef struct {
    app_routine_node_t nodes[APP_ROUTINE_MAX_NODES];
    int count;
} app_routine_day_t;

typedef struct {
    app_routine_day_t days[APP_ROUTINE_DAYS];  // 0=周日 .. 6=周六
} app_routine_t;

typedef enum {
    APP_ROUTINE_NONE = 0,   // 今日无作息表或已全部结束
    APP_ROUTINE_IN_NODE,    // 正处于某个节点内
    APP_ROUTINE_BETWEEN,    // 处于两个节点之间的空档
} app_routine_pos_t;

typedef struct {
    app_routine_pos_t pos;
    int current_index;     // 当前节点下标；无则 -1
    int next_index;        // 下一节点下标；无则 -1
    int seconds_to_next;   // 距下一节点开始（IN_NODE 时也给出，用于提前预告）
    int seconds_to_end;    // 当前节点剩余秒数；非 IN_NODE 时为 0
} app_routine_status_t;

void app_routine_init(app_routine_t *r);
void app_routine_load_template(app_routine_t *r, bool boarding);   // 走读 false / 住校 true
void app_routine_sort(app_routine_day_t *day);
bool app_routine_validate(const app_routine_day_t *day);           // 升序且不重叠
int  app_routine_add_node(app_routine_day_t *day, const app_routine_node_t *node); // 索引或 -1
bool app_routine_remove_node(app_routine_day_t *day, int index);
void app_routine_status(const app_routine_day_t *day, int minutes_of_day, int seconds_of_minute,
                        app_routine_status_t *out);
bool app_routine_parse_line(const char *line, app_routine_node_t *out);  // "08:00-08:45 第一节"
int  app_routine_parse_text(app_routine_day_t *day, const char *text);   // 多行，返回成功条数
const char *app_node_type_name(app_node_type_t type);
