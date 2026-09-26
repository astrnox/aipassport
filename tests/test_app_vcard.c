// tests/test_app_vcard.c —— vCard 3.0 文本生成的主机侧单元测试。
//
// 覆盖：标准字段拼装、值里的逗号/分号转义、非法 UTF-8 与空姓名被拒、容量不足时的
// 丢弃顺序（网址 → 职务 → 单位）与 dropped_fields 的如实回传、以及连最小名片都放不下
// 时返回 -1。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_vcard.h"

static void set_card(app_vcard_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "Zhang San");
    snprintf(c->org, sizeof(c->org), "Acme");
    snprintf(c->title, sizeof(c->title), "Eng");
    snprintf(c->tel, sizeof(c->tel), "13800138000");
    snprintf(c->email, sizeof(c->email), "a@b.c");
    snprintf(c->url, sizeof(c->url), "http://x");
}

static void test_basic(void)
{
    app_vcard_t c;
    set_card(&c);

    char out[512];
    int dropped = -1;
    int len = app_vcard_build(&c, out, sizeof(out), &dropped);
    assert(len > 0);
    assert(dropped == 0);
    assert((size_t)len == strlen(out));

    // 结构标记、版本与各字段都要在。
    assert(strncmp(out, "BEGIN:VCARD\r\n", 13) == 0);
    assert(strstr(out, "VERSION:3.0\r\n") != NULL);
    assert(strstr(out, "FN:Zhang San\r\n") != NULL);
    assert(strstr(out, "ORG:Acme\r\n") != NULL);
    assert(strstr(out, "TITLE:Eng\r\n") != NULL);
    assert(strstr(out, "TEL;TYPE=CELL:13800138000\r\n") != NULL);
    assert(strstr(out, "EMAIL:a@b.c\r\n") != NULL);
    assert(strstr(out, "URL:http://x\r\n") != NULL);
    assert(strstr(out, "\r\nEND:VCARD\r\n") != NULL);
    assert(out[len - 2] == '\r' && out[len - 1] == '\n');   // 以 CRLF 收尾
}

static void test_escaping(void)
{
    app_vcard_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "Doe,John");
    snprintf(c.org, sizeof(c.org), "A;B\\C");

    char out[256];
    int len = app_vcard_build(&c, out, sizeof(out), NULL);
    assert(len > 0);

    // 逗号、分号、反斜杠都要转义，否则读卡方会把一个字段拆成两个。
    assert(strstr(out, "FN:Doe\\,John\r\n") != NULL);
    assert(strstr(out, "ORG:A\\;B\\\\C\r\n") != NULL);
}

static void test_optional_fields(void)
{
    app_vcard_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "Li");

    char out[256];
    int dropped = -1;
    int len = app_vcard_build(&c, out, sizeof(out), &dropped);
    assert(len > 0);
    assert(dropped == 0);
    assert(strstr(out, "FN:Li\r\n") != NULL);
    assert(strstr(out, "ORG:") == NULL);
    assert(strstr(out, "TEL") == NULL);
}

static void test_rejects_bad_input(void)
{
    app_vcard_t c;
    char out[256];

    // 姓名为空：连最小名片都拼不出。
    memset(&c, 0, sizeof(c));
    assert(app_vcard_build(&c, out, sizeof(out), NULL) == -1);

    // 姓名是半个 UTF-8 字符：宁可不生成，也不写进二维码。
    memset(&c, 0, sizeof(c));
    c.name[0] = (char)0xE4;
    c.name[1] = (char)0xBD;
    c.name[2] = '\0';
    assert(app_vcard_build(&c, out, sizeof(out), NULL) == -1);

    // 其它字段非法同样整体拒绝。
    set_card(&c);
    c.email[0] = (char)0xFF;
    c.email[1] = '\0';
    assert(app_vcard_build(&c, out, sizeof(out), NULL) == -1);

    // 容量为 0。
    set_card(&c);
    assert(app_vcard_build(&c, out, 0, NULL) == -1);
}

static void test_drop_order(void)
{
    app_vcard_t c;
    set_card(&c);

    char full[512];
    int dropped = -1;
    int len0 = app_vcard_build(&c, full, sizeof(full), &dropped);
    assert(len0 > 0 && dropped == 0);
    size_t cap = (size_t)len0;

    // 容量正好差一字节放不下完整名片：先丢网址。
    char out[512];
    int len1 = app_vcard_build(&c, out, cap, &dropped);
    assert(len1 > 0 && dropped == 1);
    assert(strstr(out, "URL:") == NULL);
    assert(strstr(out, "ORG:") != NULL);
    assert(strstr(out, "TITLE:") != NULL);

    // 再紧一档：接着丢职务。
    cap = (size_t)len1;
    int len2 = app_vcard_build(&c, out, cap, &dropped);
    assert(len2 > 0 && dropped == 2);
    assert(strstr(out, "URL:") == NULL);
    assert(strstr(out, "TITLE:") == NULL);
    assert(strstr(out, "ORG:") != NULL);

    // 再紧一档：丢单位，只剩姓名、电话、邮箱。
    cap = (size_t)len2;
    int len3 = app_vcard_build(&c, out, cap, &dropped);
    assert(len3 > 0 && dropped == 3);
    assert(strstr(out, "ORG:") == NULL);
    assert(strstr(out, "FN:Zhang San\r\n") != NULL);
    assert(strstr(out, "TEL;TYPE=CELL:13800138000\r\n") != NULL);
    assert(strstr(out, "EMAIL:a@b.c\r\n") != NULL);

    // 连最小名片都放不下：返回 -1，不产出半截内容。
    assert(app_vcard_build(&c, out, (size_t)len3 - 1, &dropped) == -1);
    assert(app_vcard_build(&c, out, 8, NULL) == -1);
}

int main(void)
{
    test_basic();
    test_escaping();
    test_optional_fields();
    test_rejects_bad_input();
    test_drop_order();

    puts("test_app_vcard: PASS");
    return 0;
}
