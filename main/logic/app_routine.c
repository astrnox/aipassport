// main/logic/app_routine.c —— 作息表模型的实现（与硬件无关）。
#include "app_routine.h"
#include "app_text.h"

#include <string.h>

// 模板表项：起始分钟、结束分钟、类型、显示名。
typedef struct {
    int             start_min;
    int             end_min;
    app_node_type_t type;
    const char     *name;
} routine_template_entry_t;

// 走读模板：每天相同，仅工作日记入；周六/周日留空。
static const routine_template_entry_t DAY_SCHOOL_TEMPLATE[] = {
    { 470,  480,  APP_NODE_ARRIVE, "到校"   },
    { 480,  525,  APP_NODE_CLASS,  "第一节" },
    { 525,  535,  APP_NODE_BREAK,  "课间"   },
    { 535,  580,  APP_NODE_CLASS,  "第二节" },
    { 580,  625,  APP_NODE_CLASS,  "第三节" },
    { 625,  640,  APP_NODE_BREAK,  "课间"   },
    { 640,  685,  APP_NODE_CLASS,  "第四节" },
    { 685,  715,  APP_NODE_CLASS,  "第五节" },
    { 715,  840,  APP_NODE_LUNCH,  "午休"   },
    { 840,  885,  APP_NODE_CLASS,  "第六节" },
    { 885,  900,  APP_NODE_BREAK,  "课间"   },
    { 900,  945,  APP_NODE_CLASS,  "第七节" },
    { 945,  990,  APP_NODE_CLASS,  "第八节" },
    { 1050, 1060, APP_NODE_LEAVE,  "放学"   },
};

// 住校模板：在工作日基础上加两节晚自习，放学挪到最后一节之后。
static const routine_template_entry_t BOARDING_TEMPLATE[] = {
    { 470,  480,  APP_NODE_ARRIVE, "到校"   },
    { 480,  525,  APP_NODE_CLASS,  "第一节" },
    { 525,  535,  APP_NODE_BREAK,  "课间"   },
    { 535,  580,  APP_NODE_CLASS,  "第二节" },
    { 580,  625,  APP_NODE_CLASS,  "第三节" },
    { 625,  640,  APP_NODE_BREAK,  "课间"   },
    { 640,  685,  APP_NODE_CLASS,  "第四节" },
    { 685,  715,  APP_NODE_CLASS,  "第五节" },
    { 715,  840,  APP_NODE_LUNCH,  "午休"   },
    { 840,  885,  APP_NODE_CLASS,  "第六节" },
    { 885,  900,  APP_NODE_BREAK,  "课间"   },
    { 900,  945,  APP_NODE_CLASS,  "第七节" },
    { 945,  990,  APP_NODE_CLASS,  "第八节" },
    { 1140, 1230, APP_NODE_STUDY,  "晚自习" },
    { 1230, 1260, APP_NODE_STUDY,  "晚自习" },
    { 1260, 1270, APP_NODE_LEAVE,  "放学"   },
};

void app_routine_init(app_routine_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

static void fill_weekdays(app_routine_t *r, const routine_template_entry_t *entries, int count)
{
    // 1..5 为周一至周五；0（周日）与 6（周六）保持空表。
    for (int day = 1; day <= 5; day++) {
        app_routine_day_t *d = &r->days[day];
        d->count = count;
        for (int i = 0; i < count; i++) {
            app_routine_node_t *node = &d->nodes[i];
            memset(node, 0, sizeof(*node));
            app_utf8_copy_prefix(entries[i].name, 8, node->name, sizeof(node->name));
            node->start_min = entries[i].start_min;
            node->end_min = entries[i].end_min;
            node->type = entries[i].type;
        }
    }
}

void app_routine_load_template(app_routine_t *r, bool boarding)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    if (boarding) {
        fill_weekdays(r, BOARDING_TEMPLATE,
                      (int)(sizeof(BOARDING_TEMPLATE) / sizeof(BOARDING_TEMPLATE[0])));
    } else {
        fill_weekdays(r, DAY_SCHOOL_TEMPLATE,
                      (int)(sizeof(DAY_SCHOOL_TEMPLATE) / sizeof(DAY_SCHOOL_TEMPLATE[0])));
    }
}

void app_routine_sort(app_routine_day_t *day)
{
    if (!day) return;
    // 稳定插入排序：仅当严格大于才后移，保证同 start 的相对顺序不变。
    for (int i = 1; i < day->count; i++) {
        app_routine_node_t key = day->nodes[i];
        int j = i - 1;
        while (j >= 0 && day->nodes[j].start_min > key.start_min) {
            day->nodes[j + 1] = day->nodes[j];
            j--;
        }
        day->nodes[j + 1] = key;
    }
}

bool app_routine_validate(const app_routine_day_t *day)
{
    if (!day) return false;
    if (day->count < 0 || day->count > APP_ROUTINE_MAX_NODES) return false;
    for (int i = 0; i < day->count; i++) {
        const app_routine_node_t *n = &day->nodes[i];
        if (n->start_min < 0 || n->start_min >= n->end_min || n->end_min > 1440) return false;
        if (i > 0 && day->nodes[i - 1].end_min > n->start_min) return false;
    }
    return true;
}

// 判断节点时间是否合法：0 <= start < end <= 1440。
static bool node_time_valid(const app_routine_node_t *node)
{
    return node->start_min >= 0 && node->start_min < node->end_min && node->end_min <= 1440;
}

int app_routine_add_node(app_routine_day_t *day, const app_routine_node_t *node)
{
    if (!day || !node) return -1;
    if (day->count >= APP_ROUTINE_MAX_NODES) return -1;
    if (!node_time_valid(node)) return -1;

    // 相接（end == start）允许，只有真正重叠才拒绝。
    for (int i = 0; i < day->count; i++) {
        const app_routine_node_t *e = &day->nodes[i];
        if (!(node->end_min <= e->start_min || node->start_min >= e->end_min)) return -1;
    }

    int pos = day->count;
    for (int i = 0; i < day->count; i++) {
        if (day->nodes[i].start_min > node->start_min) {
            pos = i;
            break;
        }
    }
    for (int i = day->count; i > pos; i--) day->nodes[i] = day->nodes[i - 1];
    day->nodes[pos] = *node;
    day->count++;
    return pos;
}

bool app_routine_remove_node(app_routine_day_t *day, int index)
{
    if (!day || index < 0 || index >= day->count) return false;
    for (int i = index; i < day->count - 1; i++) day->nodes[i] = day->nodes[i + 1];
    day->count--;
    memset(&day->nodes[day->count], 0, sizeof(day->nodes[0]));
    return true;
}

void app_routine_status(const app_routine_day_t *day, int minutes_of_day, int seconds_of_minute,
                        app_routine_status_t *out)
{
    if (!out) return;
    out->pos = APP_ROUTINE_NONE;
    out->current_index = -1;
    out->next_index = -1;
    out->seconds_to_next = -1;
    out->seconds_to_end = 0;
    if (!day || day->count <= 0) return;

    int now = minutes_of_day * 60 + seconds_of_minute;

    for (int i = 0; i < day->count; i++) {
        const app_routine_node_t *n = &day->nodes[i];
        int start = n->start_min * 60;
        int end = n->end_min * 60;
        if (now >= start && now < end) {
            out->pos = APP_ROUTINE_IN_NODE;
            out->current_index = i;
            out->seconds_to_end = end - now;
            if (i + 1 < day->count) {
                out->next_index = i + 1;
                out->seconds_to_next = day->nodes[i + 1].start_min * 60 - now;
            } else {
                out->next_index = -1;
                out->seconds_to_next = -1;
            }
            return;
        }
        if (now < start) {
            // 位于第一个节点之前，或落在两个节点之间的空档。
            out->pos = APP_ROUTINE_BETWEEN;
            out->current_index = -1;
            out->next_index = i;
            out->seconds_to_next = start - now;
            out->seconds_to_end = 0;
            return;
        }
    }
    // 已到最后节点结束之后。
    out->pos = APP_ROUTINE_NONE;
    out->current_index = -1;
    out->next_index = -1;
    out->seconds_to_next = -1;
    out->seconds_to_end = 0;
}

const char *app_node_type_name(app_node_type_t type)
{
    switch (type) {
    case APP_NODE_ARRIVE: return "到校";
    case APP_NODE_CLASS:  return "上课";
    case APP_NODE_BREAK:  return "课间";
    case APP_NODE_LUNCH:  return "午休";
    case APP_NODE_STUDY:  return "晚自习";
    case APP_NODE_LEAVE:  return "放学";
    case APP_NODE_CUSTOM: return "自定义";
    default:              return "??";
    }
}

// 解析 "HH:MM"，严格两位时两位分；成功时更新游标并写回一天内分钟数。
static bool parse_hhmm(const char **pp, int *out_min)
{
    const char *p = *pp;
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return false;
    int hh = (p[0] - '0') * 10 + (p[1] - '0');
    p += 2;
    if (*p != ':') return false;
    p++;
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return false;
    int mm = (p[0] - '0') * 10 + (p[1] - '0');
    p += 2;
    if (hh > 24 || mm > 59) return false;
    int total = hh * 60 + mm;
    if (total > 1440) return false;
    *out_min = total;
    *pp = p;
    return true;
}

// 依据名称关键词推断节点类型。
static app_node_type_t derive_type(const char *name)
{
    if (strstr(name, "到校")) return APP_NODE_ARRIVE;
    if (strstr(name, "放学")) return APP_NODE_LEAVE;
    if (strstr(name, "课间")) return APP_NODE_BREAK;
    if (strstr(name, "午休") || strstr(name, "午间")) return APP_NODE_LUNCH;
    if (strstr(name, "晚自习")) return APP_NODE_STUDY;
    if (strstr(name, "上课")) return APP_NODE_CLASS;
    if (strstr(name, "第") && strstr(name, "节")) return APP_NODE_CLASS;
    return APP_NODE_CUSTOM;
}

bool app_routine_parse_line(const char *line, app_routine_node_t *out)
{
    if (!line || !out) return false;

    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    int start = 0;
    if (!parse_hhmm(&p, &start)) return false;

    while (*p == ' ' || *p == '\t') p++;

    // 分隔符支持半角 '-'、'~' 以及全角短横 '–'（E2 80 93）与长横 '—'（E2 80 94）；
    // 也允许直接用空格分隔两个时间（此时空格已被跳过，p 指向第二位数字）。
    if (*p == '-' || *p == '~') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
    } else if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 &&
               ((unsigned char)p[2] == 0x93 || (unsigned char)p[2] == 0x94)) {
        p += 3;
        while (*p == ' ' || *p == '\t') p++;
    } else if (*p < '0' || *p > '9') {
        return false;
    }

    int end = 0;
    if (!parse_hhmm(&p, &end)) return false;

    while (*p == ' ' || *p == '\t') p++;

    if (end <= start || end > 1440 || start < 0) return false;

    // 名称可能为空；去掉尾部空白后再推断类型并截断到 8 个字符。
    const char *name = p;
    size_t name_len = strlen(name);
    while (name_len > 0 && (name[name_len - 1] == ' ' || name[name_len - 1] == '\t' ||
                            name[name_len - 1] == '\r')) {
        name_len--;
    }
    char trimmed[64];
    size_t copy = name_len < sizeof(trimmed) - 1 ? name_len : sizeof(trimmed) - 1;
    memcpy(trimmed, name, copy);
    trimmed[copy] = '\0';

    memset(out, 0, sizeof(*out));
    out->start_min = start;
    out->end_min = end;
    out->type = derive_type(trimmed);
    if (trimmed[0] == '\0') {
        // 名称为空时用类型名占位，界面不会出现空标签。
        app_utf8_copy_prefix(app_node_type_name(out->type), 8, out->name, sizeof(out->name));
    } else {
        app_utf8_copy_prefix(trimmed, 8, out->name, sizeof(out->name));
    }
    return true;
}

int app_routine_parse_text(app_routine_day_t *day, const char *text)
{
    if (!day || !text) return 0;
    int success = 0;
    const char *cursor = text;
    while (*cursor) {
        const char *nl = strchr(cursor, '\n');
        size_t len = nl ? (size_t)(nl - cursor) : strlen(cursor);
        char line[160];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, cursor, len);
        line[len] = '\0';

        char *trimmed = app_text_trim(line);
        if (*trimmed != '\0' && *trimmed != '#') {
            app_routine_node_t node;
            if (app_routine_parse_line(trimmed, &node) && app_routine_add_node(day, &node) >= 0) {
                success++;
            }
        }

        if (!nl) break;
        cursor = nl + 1;
    }
    return success;
}
