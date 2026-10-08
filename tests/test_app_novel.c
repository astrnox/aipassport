// tests/test_app_novel.c —— 离线小说纯逻辑的主机测试：分页、章节识别、上传校验。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_novel.h"

static void test_header(void)
{
    app_novel_header_t h;
    app_novel_header_init(&h, "测试书", 1234, 0xDEADBEEFu, 3);
    assert(h.magic == APP_NOVEL_MAGIC);
    assert(h.version == APP_NOVEL_VERSION);
    assert(h.data_bytes == 1234);
    assert(h.data_crc == 0xDEADBEEFu);
    assert(h.chapter_count == 3);
    assert(strcmp(h.title, "测试书") == 0);

    uint32_t bytes = 0;
    assert(app_novel_header_check(&h, 4096, &bytes) == APP_NOVEL_OK);
    assert(bytes == 1234);

    // 标题按 UTF-8 截断，不会留下半个汉字。
    char long_title[64];
    memset(long_title, 0, sizeof(long_title));
    for (int i = 0; i < 12; i++) strcat(long_title, "中");
    app_novel_header_init(&h, long_title, 10, 1, 0);
    assert(strlen(h.title) <= APP_NOVEL_TITLE_LEN - 1);
    assert(strlen(h.title) % 3 == 0);
    assert(h.chapter_count == 0);

    // 章节数超上限要被夹住，读出来时才是自洽的。
    app_novel_header_init(&h, "x", 10, 1, 9999);
    assert(h.chapter_count == APP_NOVEL_MAX_CHAPTERS);

    // magic / 版本 / 空书 / 超容量 / 章节数越界都要能被区分出来。
    app_novel_header_t bad = h;
    bad.magic = 0;
    assert(app_novel_header_check(&bad, 4096, NULL) == APP_NOVEL_ERR_HEADER);
    bad = h;
    bad.version = 99;
    assert(app_novel_header_check(&bad, 4096, NULL) == APP_NOVEL_ERR_HEADER);
    bad = h;
    bad.chapter_count = APP_NOVEL_MAX_CHAPTERS + 1;
    assert(app_novel_header_check(&bad, 4096, NULL) == APP_NOVEL_ERR_HEADER);
    bad = h;
    bad.chapter_count = 0;
    bad.data_bytes = 0;
    assert(app_novel_header_check(&bad, 4096, NULL) == APP_NOVEL_ERR_EMPTY);
    bad = h;
    bad.chapter_count = 0;
    bad.data_bytes = 5000;
    assert(app_novel_header_check(&bad, 4096, NULL) == APP_NOVEL_ERR_CAPACITY);
}

static void test_units(void)
{
    assert(app_novel_char_units('A') == 1);
    assert(app_novel_char_units(' ') == 1);
    assert(app_novel_char_units(0x4E2D) == 2);      // 中
    assert(app_novel_char_units(0x3002) == 2);      // 。
    assert(app_novel_char_units(0xFF0C) == 2);      // ，
    assert(app_novel_char_units('\t') == 4);
    assert(app_novel_char_units('\r') == 0);
}

static void test_page_forward_ascii(void)
{
    const char *text = "abcdefghijklmno";       // 15 个半角字符
    app_novel_layout_t l = { .units_per_line = 5, .lines_per_page = 2 };

    size_t p0 = 0;
    size_t p1 = app_novel_page_next(text, strlen(text), p0, &l);
    assert(p1 == 10);                            // 5x2 个字符
    size_t p2 = app_novel_page_next(text, strlen(text), p1, &l);
    assert(p2 == 15);                            // 剩下 5 个，不足一页就到结尾
    assert(app_novel_page_next(text, strlen(text), p2, &l) == 15);
}

static void test_page_forward_newlines(void)
{
    const char *text = "ab\ncd\nef";
    app_novel_layout_t l = { .units_per_line = 5, .lines_per_page = 2 };

    size_t p1 = app_novel_page_next(text, strlen(text), 0, &l);
    assert(p1 == 6);                             // 两行："ab\n" + "cd\n"
    size_t p2 = app_novel_page_next(text, strlen(text), p1, &l);
    assert(p2 == strlen(text));
}

static void test_page_never_splits_cjk(void)
{
    const char *text = "你好世界";               // 每字 2 单位，4 个字符 12 字节
    app_novel_layout_t l = { .units_per_line = 2, .lines_per_page = 1 };

    size_t p1 = app_novel_page_next(text, strlen(text), 0, &l);
    assert(p1 == 3);                             // 恰好停在第一个字的字节边界
    size_t p2 = app_novel_page_next(text, strlen(text), p1, &l);
    assert(p2 == 6);
    size_t p3 = app_novel_page_next(text, strlen(text), p2, &l);
    assert(p3 == 9);
    size_t p4 = app_novel_page_next(text, strlen(text), p3, &l);
    assert(p4 == 12);
}

static void test_page_wide_char_wider_than_line(void)
{
    // 预算 1 单位却遇到 2 单位的汉字：必须原样放进这一行，不能死循环、不能吞字。
    const char *text = "中中";
    app_novel_layout_t l = { .units_per_line = 1, .lines_per_page = 1 };
    size_t p1 = app_novel_page_next(text, strlen(text), 0, &l);
    assert(p1 == 3);
    size_t p2 = app_novel_page_next(text, strlen(text), p1, &l);
    assert(p2 == 6);
}

static void test_page_crlf_is_invisible(void)
{
    const char *text = "ab\r\ncd";
    app_novel_layout_t l = { .units_per_line = 5, .lines_per_page = 1 };
    size_t p1 = app_novel_page_next(text, strlen(text), 0, &l);
    assert(p1 == 4);                             // "ab\r\n" 归第一页，'\r' 不占宽度
    assert(memcmp(text + p1, "cd", 2) == 0);
}

static void test_page_back_matches_forward(void)
{
    // 用一段中文（含换行与标点）造出多页，逐页前进记录页首，再逐页后退验证一致：
    // 前进与后退必须落在同一批边界上，否则往回翻会漏页。
    const char *text =
        "第一段：这是一个用来测试分页的故事。\n"
        "第二段：他说，山的那边还是山。\n"
        "第三段：短句。\n"
        "第四段：abcdefghijklmnopqrstuvwxyz0123456789\n";
    size_t len = strlen(text);
    app_novel_layout_t l = { .units_per_line = 10, .lines_per_page = 3 };

    size_t starts[64];
    int count = 0;
    size_t cur = 0;
    while (cur < len && count < 64) {
        starts[count++] = cur;
        size_t next = app_novel_page_next(text, len, cur, &l);
        assert(next > cur);
        cur = next;
    }
    assert(count >= 3);                          // 至少要有几页才有意义

    for (int i = 1; i < count; i++) {
        assert(app_novel_page_prev(text, len, starts[i], &l) == starts[i - 1]);
    }
}

static void test_page_floor(void)
{
    const char *text = "0123456789abcdefghij";   // 20 个半角字符
    app_novel_layout_t l = { .units_per_line = 5, .lines_per_page = 1 };

    assert(app_novel_page_floor(text, 20, 0, &l) == 0);
    assert(app_novel_page_floor(text, 20, 3, &l) == 0);      // 落在第一页里
    assert(app_novel_page_floor(text, 20, 5, &l) == 5);      // 正好是页首
    assert(app_novel_page_floor(text, 20, 7, &l) == 5);
    assert(app_novel_page_floor(text, 20, 19, &l) == 15);
    assert(app_novel_page_floor(text, 20, 20, &l) == 15);    // 越界吸附到最后一页
}

static void test_chapters(void)
{
    const char *text =
        "第一章 初见\n"
        "他说：天亮了。\n"
        "第二章 山那边\n"
        "他想起第三章里写过的那句话，可那句话现在怎么也想不起来了，于是他决定翻回去看看。\n"
        "第12节 番外\n"
        "第一百零三回 大雪\n"
        "尾声\n";
    size_t len = strlen(text);

    // 逐条向前扫描（与上传时写索引的方式一致），收集章节并检查顺序与标题。
    app_novel_chapter_t chapters[8];
    int n = 0;
    size_t from = 0;
    while (n < 8) {
        size_t at = app_novel_next_chapter(text, len, from, chapters[n].title,
                                           sizeof(chapters[n].title));
        if (at >= len) break;
        chapters[n].offset = (uint32_t)at;
        n++;
        from = at + 1;
    }
    assert(n == 4);

    assert(strcmp(chapters[0].title, "第一章 初见") == 0);
    assert(strcmp(chapters[1].title, "第二章 山那边") == 0);
    assert(strcmp(chapters[2].title, "第12节 番外") == 0);
    assert(strcmp(chapters[3].title, "第一百零三回 大雪") == 0);

    // offset 必须指向标题行首，且按顺序递增。
    assert(chapters[0].offset == 0);
    assert(memcmp(text + chapters[1].offset, "\xE7\xAC\xAC", 3) == 0);   // "第"
    assert(chapters[1].offset < chapters[2].offset);
    assert(chapters[2].offset < chapters[3].offset);

    // 章节标题不能越过行尾把下一行读进来。
    assert(chapters[2].title[0] != '\0');
    assert(strchr(chapters[2].title, '\n') == NULL);

    // 没有章节标记的书在正文长度处停下，界面据此退化为按百分比跳转。
    const char *plain = "只有正文。\n还是一行。\n";
    assert(app_novel_next_chapter(plain, strlen(plain), 0, NULL, 0) == strlen(plain));

    // 从已找到的章节行里继续向后扫（上传写索引时就是这样推进的）：同一行不会重复命中，
    // 下一处命中的是下一章的标题行。
    size_t again = app_novel_next_chapter(text, len, 1, NULL, 0);
    assert(again == (size_t)chapters[1].offset);
}

static void test_utf8_feed(void)
{
    uint32_t state = 0;

    // 合法文本被切成任意小块（含把一个汉字劈成两半）都必须通过。
    const char *text = "你好，世界";
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        assert(app_novel_utf8_feed(&state, (const uint8_t *)text + i, 1));
    }
    assert(app_novel_utf8_finish(state));

    // 续字节开头、非法首字节、被截断的序列都要判非法。
    state = 0;
    const uint8_t bad1[] = { 0x80 };
    assert(!app_novel_utf8_feed(&state, bad1, sizeof(bad1)));

    state = 0;
    const uint8_t bad2[] = { 0xFF };
    assert(!app_novel_utf8_feed(&state, bad2, sizeof(bad2)));

    state = 0;
    const uint8_t truncated[] = { 0xE4, 0xBD };
    assert(app_novel_utf8_feed(&state, truncated, sizeof(truncated)));
    assert(!app_novel_utf8_finish(state));
}

int main(void)
{
    test_header();
    test_units();
    test_page_forward_ascii();
    test_page_forward_newlines();
    test_page_never_splits_cjk();
    test_page_wide_char_wider_than_line();
    test_page_crlf_is_invisible();
    test_page_back_matches_forward();
    test_page_floor();
    test_chapters();
    test_utf8_feed();
    printf("test_app_novel: OK\n");
    return 0;
}