// tests/test_app_routine.c —— 作息表模型的主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_routine.h"
#include "logic/app_text.h"

static void test_templates(void)
{
    app_routine_t r;

    app_routine_init(&r);
    assert(r.days[0].count == 0);
    assert(r.days[6].count == 0);

    // 走读模板：工作日填满、周末留空、且有序不重叠。
    app_routine_load_template(&r, false);
    assert(r.days[0].count == 0);
    assert(r.days[6].count == 0);
    for (int d = 1; d <= 5; d++) {
        assert(r.days[d].count == 14);
        assert(app_routine_validate(&r.days[d]));
    }
    assert(r.days[1].nodes[0].type == APP_NODE_ARRIVE);
    assert(r.days[1].nodes[1].start_min == 480);
    assert(r.days[1].nodes[1].end_min == 525);
    assert(r.days[1].nodes[1].type == APP_NODE_CLASS);
    assert(strcmp(r.days[1].nodes[1].name, "第一节") == 0);
    assert(r.days[1].nodes[13].type == APP_NODE_LEAVE);

    // 住校模板：在工作日基础上多两节晚自习。
    app_routine_load_template(&r, true);
    assert(r.days[0].count == 0);
    assert(r.days[6].count == 0);
    for (int d = 1; d <= 5; d++) {
        assert(r.days[d].count == 16);
        assert(app_routine_validate(&r.days[d]));
    }
    bool has_study = false;
    for (int i = 0; i < r.days[3].count; i++) {
        if (r.days[3].nodes[i].type == APP_NODE_STUDY) has_study = true;
    }
    assert(has_study);
    assert(r.days[3].nodes[r.days[3].count - 1].type == APP_NODE_LEAVE);
}

static void test_add_remove(void)
{
    app_routine_day_t day;
    memset(&day, 0, sizeof(day));

    app_routine_node_t n = { .start_min = 480, .end_min = 525, .type = APP_NODE_CLASS };
    app_utf8_copy_prefix("第一节", 8, n.name, sizeof(n.name));
    assert(app_routine_add_node(&day, &n) == 0);
    assert(day.count == 1);

    // 重叠拒绝。
    app_routine_node_t overlap = { .start_min = 500, .end_min = 560, .type = APP_NODE_CLASS };
    assert(app_routine_add_node(&day, &overlap) == -1);
    assert(day.count == 1);

    // 相接允许。
    app_routine_node_t touch = { .start_min = 525, .end_min = 560, .type = APP_NODE_BREAK };
    assert(app_routine_add_node(&day, &touch) == 1);
    assert(day.count == 2);

    // 非法时间拒绝。
    app_routine_node_t bad = { .start_min = 600, .end_min = 600, .type = APP_NODE_CLASS };
    assert(app_routine_add_node(&day, &bad) == -1);
    app_routine_node_t bad2 = { .start_min = 100, .end_min = 2000, .type = APP_NODE_CLASS };
    assert(app_routine_add_node(&day, &bad2) == -1);

    // 插入到排序位置。
    app_routine_node_t early = { .start_min = 100, .end_min = 200, .type = APP_NODE_ARRIVE };
    assert(app_routine_add_node(&day, &early) == 0);
    assert(day.count == 3);
    assert(day.nodes[0].start_min == 100);
    assert(day.nodes[1].start_min == 480);
    assert(day.nodes[2].start_min == 525);

    // 删除移位。
    assert(app_routine_remove_node(&day, 1));
    assert(day.count == 2);
    assert(day.nodes[0].start_min == 100);
    assert(day.nodes[1].start_min == 525);
    assert(!app_routine_remove_node(&day, 5));
    assert(!app_routine_remove_node(&day, -1));
    assert(day.count == 2);

    // 表满拒绝。
    app_routine_day_t full;
    memset(&full, 0, sizeof(full));
    for (int i = 0; i < APP_ROUTINE_MAX_NODES; i++) {
        app_routine_node_t e = { .start_min = i, .end_min = i + 1, .type = APP_NODE_CLASS };
        assert(app_routine_add_node(&full, &e) == i);
    }
    assert(full.count == APP_ROUTINE_MAX_NODES);
    assert(app_routine_validate(&full));
    app_routine_node_t extra = { .start_min = 100, .end_min = 101, .type = APP_NODE_CLASS };
    assert(app_routine_add_node(&full, &extra) == -1);
}

static void test_status(void)
{
    app_routine_day_t day;
    memset(&day, 0, sizeof(day));
    app_routine_node_t a = { .start_min = 480, .end_min = 525, .type = APP_NODE_CLASS };
    app_routine_node_t b = { .start_min = 535, .end_min = 580, .type = APP_NODE_CLASS };
    app_routine_node_t c = { .start_min = 585, .end_min = 600, .type = APP_NODE_CLASS };
    assert(app_routine_add_node(&day, &a) == 0);
    assert(app_routine_add_node(&day, &b) == 1);
    assert(app_routine_add_node(&day, &c) == 2);

    app_routine_status_t st;

    // 在节点内：08:00。
    app_routine_status(&day, 480, 0, &st);
    assert(st.pos == APP_ROUTINE_IN_NODE);
    assert(st.current_index == 0);
    assert(st.next_index == 1);
    assert(st.seconds_to_end == 2700);
    assert(st.seconds_to_next == 3300);

    // 空档：08:50。
    app_routine_status(&day, 530, 0, &st);
    assert(st.pos == APP_ROUTINE_BETWEEN);
    assert(st.current_index == -1);
    assert(st.next_index == 1);
    assert(st.seconds_to_next == 300);
    assert(st.seconds_to_end == 0);

    // 第一个节点之前：06:40。
    app_routine_status(&day, 400, 0, &st);
    assert(st.pos == APP_ROUTINE_BETWEEN);
    assert(st.current_index == -1);
    assert(st.next_index == 0);
    assert(st.seconds_to_next == 4800);
    assert(st.seconds_to_end == 0);

    // 最后节点之后：11:40。
    app_routine_status(&day, 700, 0, &st);
    assert(st.pos == APP_ROUTINE_NONE);
    assert(st.current_index == -1);
    assert(st.next_index == -1);
    assert(st.seconds_to_next == -1);
    assert(st.seconds_to_end == 0);

    // 最后一个节点内：next 为 -1。
    app_routine_status(&day, 590, 0, &st);
    assert(st.pos == APP_ROUTINE_IN_NODE);
    assert(st.current_index == 2);
    assert(st.next_index == -1);
    assert(st.seconds_to_next == -1);
    assert(st.seconds_to_end == 600);

    // 空表。
    app_routine_day_t empty;
    memset(&empty, 0, sizeof(empty));
    app_routine_status(&empty, 480, 0, &st);
    assert(st.pos == APP_ROUTINE_NONE);
    assert(st.current_index == -1);
    assert(st.next_index == -1);
}

static void test_parse_line(void)
{
    app_routine_node_t n;

    assert(app_routine_parse_line("08:00-08:45 第一节", &n));
    assert(n.start_min == 480);
    assert(n.end_min == 525);
    assert(n.type == APP_NODE_CLASS);
    assert(strcmp(n.name, "第一节") == 0);

    // 空格分隔形式。
    assert(app_routine_parse_line("08:00 08:45 第一节", &n));
    assert(n.start_min == 480 && n.end_min == 525);

    // 全角波浪号与全角短横。
    assert(app_routine_parse_line("08:00~08:45 课间", &n));
    assert(n.type == APP_NODE_BREAK);
    assert(app_routine_parse_line("07:50–08:00 到校", &n));
    assert(n.type == APP_NODE_ARRIVE);

    // 名称关键字推断类型。
    assert(app_routine_parse_line("11:55-14:00 午间", &n));
    assert(n.type == APP_NODE_LUNCH);
    assert(app_routine_parse_line("19:00-20:30 晚自习", &n));
    assert(n.type == APP_NODE_STUDY);
    assert(app_routine_parse_line("17:30-17:40 放学", &n));
    assert(n.type == APP_NODE_LEAVE);
    assert(app_routine_parse_line("10:00-10:45 随便写点", &n));
    assert(n.type == APP_NODE_CUSTOM);

    // 名称为空时用类型名。
    assert(app_routine_parse_line("08:00-09:00", &n));
    assert(n.type == APP_NODE_CUSTOM);
    assert(strcmp(n.name, "自定义") == 0);

    // 名称截断到 8 个字符，不切断多字节字符。
    assert(app_routine_parse_line("08:00-09:00 一二三四五六七八九十", &n));
    assert(app_utf8_valid(n.name));
    assert(app_utf8_chars(n.name) == 8);
    assert(strcmp(n.name, "一二三四五六七八") == 0);

    // 非法时间 / 结束不晚于开始。
    assert(!app_routine_parse_line("08:0-09:00 x", &n));
    assert(!app_routine_parse_line("25:00-26:00 x", &n));
    assert(!app_routine_parse_line("0800-0900 x", &n));
    assert(!app_routine_parse_line("09:00-08:00 x", &n));
    assert(!app_routine_parse_line("09:00-09:00 x", &n));
}

static void test_parse_text(void)
{
    app_routine_day_t day;
    memset(&day, 0, sizeof(day));

    const char *text =
        "# 作息表备注\n"
        "08:00-08:45 第一节\n"
        "\n"
        "08:45-08:55 课间\n"
        "这不是一行合法数据\n"
        "09:00-09:40 第二节\n";

    int ok = app_routine_parse_text(&day, text);
    assert(ok == 3);
    assert(day.count == 3);
    assert(day.nodes[0].start_min == 480);
    assert(day.nodes[1].start_min == 525);
    assert(day.nodes[2].start_min == 540);
    assert(day.nodes[2].type == APP_NODE_CLASS);
    assert(app_routine_validate(&day));

    // 乱序输入也应按顺序插入。
    app_routine_day_t d2;
    memset(&d2, 0, sizeof(d2));
    assert(app_routine_parse_text(&d2, "09:00-09:40 第二节\n08:00-08:45 第一节\n") == 2);
    assert(d2.nodes[0].start_min == 480);
    assert(d2.nodes[1].start_min == 540);
}

int main(void)
{
    test_templates();
    test_add_remove();
    test_status();
    test_parse_line();
    test_parse_text();
    puts("test_app_routine: PASS");
    return 0;
}
