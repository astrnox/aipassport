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
#define APP_ROUTINE_WEEKS     2   // 0=单周（ISO 奇数周） 1=双周（ISO 偶数周）

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
    // 外表是套别：0=单周表，1=双周表；内层 0=周日 .. 6=周六。
    // 未启用单双周时只使用单周表（下标 0）。
    app_routine_day_t days[APP_ROUTINE_WEEKS][APP_ROUTINE_DAYS];
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
int  app_routine_parse_text(app_routine_day_t *day, const char *text);   // 单日多行，返回成功条数

// 依据 ISO 周序号判断套别：奇数周为单周表（0），偶数周为双周表（1）。
// iso_week 非法（<=0，例如时间未校准）时按单周表处理，保证仍有可展示的作息。
int app_routine_week_slot(int iso_week);

// 取指定星期、指定套别的作息表。参数越界时返回 NULL。
const app_routine_day_t *app_routine_day_get(const app_routine_t *r, int weekday, int slot);
app_routine_day_t       *app_routine_day_mut(app_routine_t *r, int weekday, int slot);

// 解析整表导入文本并写入 r，返回成功导入的数据行数（写入全部七天的一行按 1 行计）。
// 指令行（行首可有空白）：
//   @单周 / @双周   选择写入哪一套表，同时把目标日重置为"全部七天"
//   @周一 .. @周日  把目标日收窄为某一天；未出现星期指令时写入该套表的全部七天
// 也接受不带 @ 的整行星期名（"周一"）。以 # 开头的行为注释，空行忽略。
// 与已有节点重叠的行按 app_routine_add_node 的规则拒绝，不计入条数。
// out_has_alt 非空时写回文本是否写入过双周表（用于提示用户已配置单双周）。
int app_routine_parse_table(app_routine_t *r, const char *text, bool *out_has_alt);

const char *app_node_type_name(app_node_type_t type);
