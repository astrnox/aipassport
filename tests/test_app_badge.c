// tests/test_app_badge.c —— 电子工牌数据模型的主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_badge.h"
#include "logic/app_text.h"

static void test_init_add_remove(void)
{
    app_badge_list_t l;
    app_badge_list_init(&l);
    assert(l.count == 0);
    assert(l.selected == 0);

    // 最多 5 张，默认昵称“未命名”、行内容为空。
    for (int i = 0; i < APP_BADGE_MAX; i++) {
        assert(app_badge_add(&l) == i);
        assert(l.items[i].used);
        assert(strcmp(l.items[i].nickname, "未命名") == 0);
        assert(l.items[i].lines[0][0] == '\0');
    }
    assert(app_badge_add(&l) == -1);
    assert(l.count == APP_BADGE_MAX);

    // 删除最后一张会 clamp selected。
    l.selected = 4;
    assert(app_badge_remove(&l, 4));
    assert(l.count == 4);
    assert(l.selected == 3);
    assert(!l.items[4].used);

    // 删除头部会整体前移并再次 clamp。
    assert(app_badge_remove(&l, 0));
    assert(l.count == 3);
    assert(l.selected == 2);

    assert(!app_badge_remove(&l, 10));
    assert(!app_badge_remove(&l, -1));
    assert(l.count == 3);
}

static void test_cycle(void)
{
    app_badge_list_t l;
    app_badge_list_init(&l);

    // 空表不应崩溃。
    app_badge_cycle(&l, 1);
    app_badge_cycle(&l, -3);
    assert(l.selected == 0);

    app_badge_add(&l);
    app_badge_add(&l);
    app_badge_add(&l);
    l.selected = 0;

    app_badge_cycle(&l, 1);
    assert(l.selected == 1);
    app_badge_cycle(&l, 1);
    assert(l.selected == 2);
    app_badge_cycle(&l, 1);
    assert(l.selected == 0);

    // 负方向环绕。
    app_badge_cycle(&l, -1);
    assert(l.selected == 2);
    app_badge_cycle(&l, -1);
    assert(l.selected == 1);

    // 超出范围的 delta 也要安全环绕。
    app_badge_cycle(&l, 5);
    assert(l.selected == 0);   // (1 + 5) % 3 = 0
    app_badge_cycle(&l, -7);
    assert(l.selected == 2);   // (0 - 7) % 3 -> 2
    app_badge_cycle(&l, 100);
    assert(l.selected >= 0 && l.selected < 3);
}

static void test_set_text(void)
{
    app_badge_t b;
    memset(&b, 0, sizeof(b));

    assert(!app_badge_set_text(NULL, "x", NULL, 0));

    // 普通设置。
    const char *lines[] = { "第一行", "第二行" };
    assert(app_badge_set_text(&b, "小明", lines, 2));
    assert(strcmp(b.nickname, "小明") == 0);
    assert(strcmp(b.lines[0], "第一行") == 0);
    assert(strcmp(b.lines[1], "第二行") == 0);
    assert(b.lines[2][0] == '\0');
    assert(b.lines[3][0] == '\0');

    // 空行保留为空字符串。
    const char *lines2[] = { "", "第三行" };
    assert(app_badge_set_text(&b, "n", lines2, 2));
    assert(b.lines[0][0] == '\0');
    assert(strcmp(b.lines[1], "第三行") == 0);

    // NULL 昵称与 NULL 行变成空字符串。
    const char *lines3[] = { NULL, "x" };
    assert(app_badge_set_text(&b, NULL, lines3, 2));
    assert(b.nickname[0] == '\0');
    assert(b.lines[0][0] == '\0');
    assert(strcmp(b.lines[1], "x") == 0);

    // ASCII 截断：昵称 8 个字符、每行 12 个字符。
    assert(app_badge_set_text(&b, "abcdefghijk", NULL, 0));
    assert(strcmp(b.nickname, "abcdefgh") == 0);
    assert(app_utf8_chars(b.nickname) == 8);

    const char *long_line[] = { "0123456789ABCDEF" };
    assert(app_badge_set_text(&b, "n", long_line, 1));
    assert(strcmp(b.lines[0], "0123456789AB") == 0);
    assert(app_utf8_chars(b.lines[0]) == 12);

    // 多字节字符不会被从中间切断；实际字符数受缓冲区上限约束。
    assert(app_badge_set_text(&b, "一二三四五六七八九十", NULL, 0));
    assert(app_utf8_valid(b.nickname));
    assert(app_utf8_chars(b.nickname) == 7);   // 24 字节缓冲最多容纳 7 个汉字
    assert(memcmp(b.nickname, "一二三四五六七", strlen("一二三四五六七")) == 0);

    const char *zh[] = { "一二三四五六七八九十十一十二十三" };
    assert(app_badge_set_text(&b, "n", zh, 1));
    assert(app_utf8_valid(b.lines[0]));
    assert(app_utf8_chars(b.lines[0]) == 9);   // 28 字节缓冲最多容纳 9 个汉字
    assert(memcmp(b.lines[0], "一二三四五六七八九", strlen("一二三四五六七八九")) == 0);

    // 非法 UTF-8 拒绝。
    assert(!app_badge_set_text(&b, "\xFF\xFE", NULL, 0));
    const char *bad[] = { "\xE4\xB8" };   // 半个汉字
    assert(!app_badge_set_text(&b, "ok", bad, 1));
}

int main(void)
{
    test_init_add_remove();
    test_cycle();
    test_set_text();
    puts("test_app_badge: PASS");
    return 0;
}
