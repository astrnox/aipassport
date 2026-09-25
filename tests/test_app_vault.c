// tests/test_app_vault.c —— app_vault（密码本纯逻辑层）的主机侧单元测试。
//
// 覆盖：初始状态、明文增删改查与越界/满载、选择与轮转、定长字段的 UTF-8 截断、明文与
// 加密容器的序列化/反序列化（含损坏容器必须安全失败并把目标重置）、敲击手势与恢复码
// 工具、启用加密、上锁与解锁（含"锁定态序列化再反序列化"的重启路径）、恢复码解锁、
// 改手势、换恢复码、退回明文、密文/MAC/封装被篡改时的检测，以及满载 + 超长字段的边界。
//
// 随机源是注入式的：这里用固定种子的 xorshift32，保证同一份代码每次失败的现场一致。
// 另有一组用例专门验证"未注入随机源"时所有需要随机数的操作都返回 APP_VAULT_ERR_ENTROPY，
// 而不会退化成用可预测字节冒充密钥。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_vault.h"

// ---------------------------------------------------------------------------
// 确定性随机源
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t state;
} prng_t;

static prng_t g_prng;

static void prng_fill(void *ctx, void *out, size_t len)
{
    prng_t *p = (prng_t *)ctx;
    uint8_t *bytes = (uint8_t *)out;

    for (size_t i = 0; i < len; i++) {
        // xorshift32；state 恒不为 0。
        p->state ^= p->state << 13;
        p->state ^= p->state >> 17;
        p->state ^= p->state << 5;
        bytes[i] = (uint8_t)(p->state >> 24);
    }
}

static void inject_random(void)
{
    g_prng.state = 0x12345678u;
    app_vault_set_random(prng_fill, &g_prng);
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static void must_add(app_vault_t *v, const char *label, const char *account, const char *password)
{
    app_vault_status_t status = app_vault_add(v, label, account, password, NULL);
    assert(status == APP_VAULT_OK);
    (void)status;
}

static app_vault_knock_t knock_of(const uint8_t *taps, int len)
{
    app_vault_knock_t k;
    memset(&k, 0, sizeof(k));
    for (int i = 0; i < len && i < APP_VAULT_KNOCK_MAX; i++) k.taps[i] = taps[i];
    k.len = len;
    return k;
}

// 正确手势：上-下-上-下
static app_vault_knock_t knock_udud(void)
{
    const uint8_t taps[4] = { APP_VAULT_TAP_UP, APP_VAULT_TAP_DOWN, APP_VAULT_TAP_UP, APP_VAULT_TAP_DOWN };
    return knock_of(taps, 4);
}

// 错误手势：下-下-上-上
static app_vault_knock_t knock_dduu(void)
{
    const uint8_t taps[4] = { APP_VAULT_TAP_DOWN, APP_VAULT_TAP_DOWN, APP_VAULT_TAP_UP, APP_VAULT_TAP_UP };
    return knock_of(taps, 4);
}

// 另一个合法手势，长度 5，用于"改手势"。
static app_vault_knock_t knock_upup(void)
{
    const uint8_t taps[5] = { APP_VAULT_TAP_UP, APP_VAULT_TAP_UP, APP_VAULT_TAP_DOWN,
                              APP_VAULT_TAP_DOWN, APP_VAULT_TAP_UP };
    return knock_of(taps, 5);
}

// 往定长字段里塞 chars 个汉字"字"（每字 3 字节）。
static void fill_cn(char *out, size_t cap, int chars)
{
    size_t pos = 0;
    for (int i = 0; i < chars && pos + 3 + 1 <= cap; i++) {
        memcpy(out + pos, "字", 3);
        pos += 3;
    }
    out[pos] = '\0';
}

// 生成 "NN" + filler_len 个 'a'，用于制造超长字段。
static void make_ascii(char *out, size_t cap, int prefix, size_t filler_len)
{
    int written = snprintf(out, cap, "%02d", prefix);
    assert(written == 2);
    size_t pos = 2;
    for (size_t i = 0; i < filler_len && pos + 1 < cap; i++) out[pos++] = 'a';
    out[pos] = '\0';
}

// 把 26 个无分隔字符排成"Crockford 风格"的分组形式（每 5 个插一个 '-'）。
static void dash_recovery(const char *plain, char out[APP_VAULT_RECOVERY_LEN + 1])
{
    int pos = 0;
    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        if (i > 0 && i % 5 == 0) out[pos++] = '-';
        out[pos++] = plain[i];
    }
    out[pos] = '\0';
    assert(pos == APP_VAULT_RECOVERY_LEN);
}

static void assert_entry_eq(const app_vault_entry_t *e, const char *label,
                            const char *account, const char *password)
{
    assert(e != NULL);
    assert(e->used);
    assert(strcmp(e->label, label) == 0);
    assert(strcmp(e->account, account) == 0);
    assert(strcmp(e->password, password) == 0);
}

// ---------------------------------------------------------------------------
// 1. 初始状态
// ---------------------------------------------------------------------------
static void test_init(void)
{
    app_vault_t v;
    memset(&v, 0xAA, sizeof(v));   // 故意留脏内存，确认 init 真的清零
    app_vault_init(&v);

    assert(v.mode == APP_VAULT_PLAIN);
    assert(!app_vault_is_encrypted(&v));
    assert(!app_vault_is_locked(&v));
    assert(v.count == 0);
    assert(app_vault_selected(&v) == -1);
    assert(app_vault_at(&v, 0) == NULL);
    assert(strcmp(app_vault_pending_recovery(&v), "") == 0);
    for (int i = 0; i < APP_VAULT_MAX; i++) assert(!v.entries[i].used);

    // 空本也可序列化，得到一个 count=0 的明文容器。
    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n == 8);
    app_vault_t r;
    memset(&r, 0xCC, sizeof(r));
    assert(app_vault_deserialize(&r, buf, n) == APP_VAULT_OK);
    assert(r.mode == APP_VAULT_PLAIN && r.count == 0);
    assert(app_vault_selected(&r) == -1);
}

// ---------------------------------------------------------------------------
// 2. 明文增删改查
// ---------------------------------------------------------------------------
static void test_plain_crud(void)
{
    app_vault_t v;
    app_vault_init(&v);

    int idx = -1;
    assert(app_vault_add(&v, "邮箱", "me@example.com", "pw-1", &idx) == APP_VAULT_OK);
    assert(idx == 0 && v.count == 1 && v.selected == 0);
    assert(app_vault_add(&v, "github", "octocat", "pw-2", &idx) == APP_VAULT_OK);
    assert(idx == 1);
    assert(app_vault_add(&v, "bank", "alice", "pw-3", &idx) == APP_VAULT_OK);
    assert(idx == 2 && v.count == 3 && v.selected == 2);

    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");
    assert_entry_eq(app_vault_at(&v, 1), "github", "octocat", "pw-2");
    assert_entry_eq(app_vault_at(&v, 2), "bank", "alice", "pw-3");
    assert(app_vault_at(&v, 3) == NULL);
    assert(app_vault_at(&v, -1) == NULL);

    // NULL / 空串字段按文档被接受
    assert(app_vault_add(&v, NULL, NULL, NULL, &idx) == APP_VAULT_OK);
    assert(idx == 3);
    assert_entry_eq(app_vault_at(&v, 3), "", "", "");
    assert(app_vault_add(&v, "", "", "", NULL) == APP_VAULT_OK);
    assert(v.count == 5);

    // 更新已有条目：空串表示"保持原值"
    assert(app_vault_update(&v, 1, "github", "", "pw-2-new") == APP_VAULT_OK);
    assert_entry_eq(app_vault_at(&v, 1), "github", "octocat", "pw-2-new");

    // 越界更新不得破坏其它条目
    assert(app_vault_update(&v, 99, "x", NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_update(&v, -1, "x", NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(v.count == 5);
    assert_entry_eq(app_vault_at(&v, 1), "github", "octocat", "pw-2-new");

    // 删除中间一条：后面的整体前移，腾出的槽位被清空
    assert(app_vault_remove(&v, 1) == APP_VAULT_OK);
    assert(v.count == 4);
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");
    assert_entry_eq(app_vault_at(&v, 1), "bank", "alice", "pw-3");
    assert_entry_eq(app_vault_at(&v, 2), "", "", "");
    assert_entry_eq(app_vault_at(&v, 3), "", "", "");
    assert(!v.entries[4].used);
    assert(v.entries[4].label[0] == '\0');
    assert(v.entries[4].password[0] == '\0');

    // 越界删除
    assert(app_vault_remove(&v, 4) == APP_VAULT_ERR_RANGE);
    assert(app_vault_remove(&v, -1) == APP_VAULT_ERR_RANGE);
    assert(v.count == 4);

    // 非法 UTF-8 被拒绝，且不落地
    assert(app_vault_add(&v, "\xFF\xFE", NULL, NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(v.count == 4);

    // 填满到上限后再 add 返回 FULL
    while (v.count < APP_VAULT_MAX) must_add(&v, "l", "a", "p");
    assert(v.count == APP_VAULT_MAX);
    assert(app_vault_add(&v, "l", "a", "p", NULL) == APP_VAULT_ERR_FULL);
    assert(v.count == APP_VAULT_MAX);
}

// ---------------------------------------------------------------------------
// 3. 选择与轮转
// ---------------------------------------------------------------------------
static void test_selection_cycle(void)
{
    app_vault_t v;
    app_vault_init(&v);

    // 空本：select / cycle 无副作用，selected 恒为 -1
    app_vault_select(&v, 3);
    app_vault_cycle(&v, 1);
    app_vault_cycle(&v, -1);
    assert(app_vault_selected(&v) == -1);

    must_add(&v, "a", "b", "c");
    must_add(&v, "d", "e", "f");
    must_add(&v, "g", "h", "i");
    assert(app_vault_selected(&v) == 2);   // add 后选中最后一条

    app_vault_select(&v, 0);
    assert(app_vault_selected(&v) == 0);
    app_vault_select(&v, 2);
    assert(app_vault_selected(&v) == 2);
    // 越界被夹到两端
    app_vault_select(&v, 99);
    assert(app_vault_selected(&v) == 2);
    app_vault_select(&v, -5);
    assert(app_vault_selected(&v) == 0);

    // 正向环绕
    app_vault_select(&v, 2);
    app_vault_cycle(&v, 1);
    assert(app_vault_selected(&v) == 0);
    app_vault_cycle(&v, 1);
    assert(app_vault_selected(&v) == 1);
    // 反向环绕
    app_vault_cycle(&v, -1);
    assert(app_vault_selected(&v) == 0);
    app_vault_cycle(&v, -1);
    assert(app_vault_selected(&v) == 2);
    // 步长大于条目数
    app_vault_cycle(&v, 4);
    assert(app_vault_selected(&v) == 0);   // 2 + 4 = 6 -> 0
    app_vault_cycle(&v, -7);
    assert(app_vault_selected(&v) == 2);   // 0 - 7 = -7 -> 2
}

// ---------------------------------------------------------------------------
// 4. 定长字段的 UTF-8 截断
// ---------------------------------------------------------------------------
static void test_utf8_truncation(void)
{
    app_vault_t v;
    app_vault_init(&v);

    char label[64];
    char account[64];
    char password[64];
    fill_cn(label, sizeof(label), 8);        // 8 个汉字 = 24 字节，正好等于 label 容量
    fill_cn(account, sizeof(account), 14);   // 42 字节，超过 40
    fill_cn(password, sizeof(password), 17); // 51 字节，超过 48

    int idx = -1;
    assert(app_vault_add(&v, label, account, password, &idx) == APP_VAULT_OK);
    const app_vault_entry_t *e = app_vault_at(&v, idx);
    assert(e != NULL);

    // 截断必须落在字符边界：长度是 3 的倍数，且仍小于字段容量（说明留有 NUL）。
    assert(strlen(e->label) == 21 && strlen(e->label) % 3 == 0);
    assert(strlen(e->account) == 39 && strlen(e->account) % 3 == 0);
    assert(strlen(e->password) == 45 && strlen(e->password) % 3 == 0);
    assert(strlen(e->label) < APP_VAULT_LABEL_LEN);
    assert(strlen(e->account) < APP_VAULT_ACCOUNT_LEN);
    assert(strlen(e->password) < APP_VAULT_PASSWORD_LEN);
    for (size_t i = 0; i + 3 <= strlen(e->label); i += 3) assert(memcmp(e->label + i, "字", 3) == 0);
    for (size_t i = 0; i + 3 <= strlen(e->account); i += 3) assert(memcmp(e->account + i, "字", 3) == 0);
    for (size_t i = 0; i + 3 <= strlen(e->password); i += 3) assert(memcmp(e->password + i, "字", 3) == 0);

    // 纯 ASCII 超长按字符数截断：label 8 / account 16 / password 16
    app_vault_init(&v);
    assert(app_vault_add(&v, "abcdefghijklmnop", "abcdefghijklmnopqrstuvwxyz",
                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ", &idx) == APP_VAULT_OK);
    e = app_vault_at(&v, idx);
    assert_entry_eq(e, "abcdefgh", "abcdefghijklmnop", "ABCDEFGHIJKLMNOP");

    // 被切断的 UTF-8 序列直接拒绝，不占用条目
    assert(app_vault_add(&v, "\xE4\xB8", NULL, NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_add(&v, "\xFF\xFE\xFD", NULL, NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(v.count == 1);
}

// ---------------------------------------------------------------------------
// 5. 明文序列化往返与损坏容器
// ---------------------------------------------------------------------------
static void test_plain_serialization(void)
{
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");
    app_vault_select(&v, 0);

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 8 && n <= APP_VAULT_CONTAINER_MAX);

    app_vault_t r;
    memset(&r, 0xCC, sizeof(r));
    assert(app_vault_deserialize(&r, buf, n) == APP_VAULT_OK);
    assert(r.mode == APP_VAULT_PLAIN && !app_vault_is_locked(&r));
    assert(r.count == v.count);
    assert(r.selected == v.selected);
    for (int i = 0; i < v.count; i++) {
        assert(memcmp(&r.entries[i], &v.entries[i], sizeof(app_vault_entry_t)) == 0);
    }

    // 恰好容量的缓冲可以写入；少一个字节必须失败
    uint8_t exact[APP_VAULT_CONTAINER_MAX];
    assert(app_vault_serialize(&v, exact, n) == n);
    assert(app_vault_serialize(&v, exact, n - 1) == 0);
    assert(app_vault_serialize(&v, exact, 0) == 0);

    app_vault_t g;

    // NULL / 零长 / 太短
    assert(app_vault_deserialize(&g, NULL, 0) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0 && g.mode == APP_VAULT_PLAIN && !g.entries[0].used);
    assert(app_vault_deserialize(&g, buf, 0) == APP_VAULT_ERR_FORMAT);
    assert(app_vault_deserialize(&g, buf, 4) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0);

    // 垃圾字节
    uint8_t junk[64];
    memset(junk, 0x5A, sizeof(junk));
    assert(app_vault_deserialize(&g, junk, sizeof(junk)) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0 && !g.entries[0].used);
    assert(app_vault_selected(&g) == -1);

    // 失败必须把"本来有内容"的目标重置为干净空本
    app_vault_t dirty;
    app_vault_init(&dirty);
    must_add(&dirty, "x", "y", "z");
    assert(app_vault_deserialize(&dirty, junk, sizeof(junk)) == APP_VAULT_ERR_FORMAT);
    assert(dirty.count == 0);
    assert(app_vault_selected(&dirty) == -1);

    // 截断一个字节
    assert(app_vault_deserialize(&g, buf, n) == APP_VAULT_OK && g.count == 2);
    assert(app_vault_deserialize(&g, buf, n - 1) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0 && !g.entries[0].used);

    // 记录数字段与数据不符：改大 -> 记录解析越界；改小 -> 尾部有多余字节
    uint8_t tampered[APP_VAULT_CONTAINER_MAX];
    memcpy(tampered, buf, n);
    tampered[6] = (uint8_t)(tampered[6] + 1);
    assert(app_vault_deserialize(&g, tampered, n) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0);
    memcpy(tampered, buf, n);
    tampered[6] = (uint8_t)(tampered[6] - 1);
    assert(app_vault_deserialize(&g, tampered, n) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0);

    // magic / version 不符
    memcpy(tampered, buf, n);
    tampered[4] = 99;
    assert(app_vault_deserialize(&g, tampered, n) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0);
    memcpy(tampered, buf, n);
    tampered[0] ^= 0xFF;
    assert(app_vault_deserialize(&g, tampered, n) == APP_VAULT_ERR_FORMAT);
    assert(g.count == 0);
}

// ---------------------------------------------------------------------------
// 6. 敲击手势工具
// ---------------------------------------------------------------------------
static void test_knock_tools(void)
{
    app_vault_knock_t k;
    app_vault_knock_clear(&k);
    assert(k.len == 0);
    assert(!app_vault_knock_pop(&k));   // 空的手势不能退格

    // 一直推满，再推必须失败且不越界
    for (int i = 0; i < APP_VAULT_KNOCK_MAX; i++) {
        assert(app_vault_knock_push(&k, (i % 2 == 0) ? APP_VAULT_TAP_UP : APP_VAULT_TAP_DOWN));
    }
    assert(k.len == APP_VAULT_KNOCK_MAX);
    assert(!app_vault_knock_push(&k, APP_VAULT_TAP_UP));
    assert(k.len == APP_VAULT_KNOCK_MAX);
    // 非法敲击值
    assert(!app_vault_knock_push(&k, 2));
    assert(!app_vault_knock_push(&k, -1));

    assert(app_vault_knock_pop(&k));
    assert(k.len == APP_VAULT_KNOCK_MAX - 1);
    app_vault_knock_clear(&k);
    assert(k.len == 0 && k.taps[0] == 0);

    // 合法性：长度必须落在 [MIN, MAX]，每个敲击必须是 UP/DOWN
    app_vault_knock_t s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < APP_VAULT_KNOCK_MIN - 1; i++) s.taps[i] = APP_VAULT_TAP_UP;
    s.len = APP_VAULT_KNOCK_MIN - 1;
    assert(!app_vault_knock_valid(&s));
    for (int i = 0; i < APP_VAULT_KNOCK_MIN; i++) s.taps[i] = APP_VAULT_TAP_UP;
    s.len = APP_VAULT_KNOCK_MIN;
    assert(app_vault_knock_valid(&s));
    for (int i = 0; i < APP_VAULT_KNOCK_MAX; i++) s.taps[i] = APP_VAULT_TAP_DOWN;
    s.len = APP_VAULT_KNOCK_MAX;
    assert(app_vault_knock_valid(&s));
    s.taps[0] = 2;
    assert(!app_vault_knock_valid(&s));
    s.taps[0] = APP_VAULT_TAP_UP;
    s.len = APP_VAULT_KNOCK_MAX + 1;   // 超出上限：先判长度，不会去读越界的 taps
    assert(!app_vault_knock_valid(&s));
    assert(!app_vault_knock_valid(NULL));

    // 相等比较
    app_vault_knock_t a = knock_udud();
    app_vault_knock_t b = knock_udud();
    app_vault_knock_t c = knock_dduu();
    assert(app_vault_knock_same(&a, &a));
    assert(app_vault_knock_same(&a, &b));
    assert(!app_vault_knock_same(&a, &c));
    b.taps[0] = (uint8_t)(b.taps[0] ^ 1);
    assert(!app_vault_knock_same(&a, &b));
    b = a;
    b.len--;
    assert(!app_vault_knock_same(&a, &b));   // 长度不同
    assert(!app_vault_knock_same(&a, NULL));
    assert(!app_vault_knock_same(NULL, &a));

    // 允许长度 4..16 的合法手势
    a = knock_udud();
    assert(app_vault_knock_valid(&a));
    assert(a.len == APP_VAULT_KNOCK_MIN);
}

// ---------------------------------------------------------------------------
// 7. 恢复码工具
// ---------------------------------------------------------------------------
static void test_recovery_tools(void)
{
    char out[APP_VAULT_RECOVERY_LEN + 1];
    char dashed[APP_VAULT_RECOVERY_LEN + 1];

    // 26 个字符全部取自 Crockford 表（刻意避开 I/L/O/U）。
    const char plain[APP_VAULT_RECOVERY_CHARS + 1] = "0123456789ABCDEFGHJKMNPQRV";

    dash_recovery(plain, dashed);
    assert(strlen(dashed) == APP_VAULT_RECOVERY_LEN);

    // 规范带分隔线形式
    assert(app_vault_recovery_normalize(dashed, out, sizeof(out)));
    assert(strcmp(out, plain) == 0);

    // 小写 + 无分隔线 + 夹空白 / 制表符
    char messy[128];
    size_t pos = 0;
    messy[pos++] = ' ';
    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        char c = plain[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        messy[pos++] = c;
        if (i % 5 == 4) messy[pos++] = '-';
    }
    messy[pos++] = '\t';
    messy[pos] = '\0';
    assert(app_vault_recovery_normalize(messy, out, sizeof(out)));
    assert(strcmp(out, plain) == 0);

    // Crockford 易混字符折回：I/L -> 1、O -> 0、U -> V
    char folded[APP_VAULT_RECOVERY_CHARS + 1];
    memcpy(folded, plain, sizeof(folded));
    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        if (folded[i] == '1') folded[i] = 'I';
        else if (folded[i] == '0') folded[i] = 'O';
        else if (folded[i] == 'V') folded[i] = 'U';
    }
    assert(app_vault_recovery_normalize(folded, out, sizeof(out)));
    assert(strcmp(out, plain) == 0);

    memcpy(folded, plain, sizeof(folded));
    for (int i = 0; i < APP_VAULT_RECOVERY_CHARS; i++) {
        if (folded[i] == '1') folded[i] = 'L';
        else if (folded[i] == '0') folded[i] = 'O';
        else if (folded[i] == 'V') folded[i] = 'U';
    }
    assert(app_vault_recovery_normalize(folded, out, sizeof(out)));
    assert(strcmp(out, plain) == 0);

    // 长度不对：少一个 / 多一个
    char wrong_len[APP_VAULT_RECOVERY_CHARS + 3];
    memcpy(wrong_len, plain, APP_VAULT_RECOVERY_CHARS - 1);
    wrong_len[APP_VAULT_RECOVERY_CHARS - 1] = '\0';
    assert(!app_vault_recovery_normalize(wrong_len, out, sizeof(out)));   // 25
    memcpy(wrong_len, plain, APP_VAULT_RECOVERY_CHARS);
    wrong_len[APP_VAULT_RECOVERY_CHARS] = 'Z';                            // Z 在表内
    wrong_len[APP_VAULT_RECOVERY_CHARS + 1] = '\0';
    assert(!app_vault_recovery_normalize(wrong_len, out, sizeof(out)));   // 27

    // 表外字符
    char bad[APP_VAULT_RECOVERY_CHARS + 1];
    memcpy(bad, plain, sizeof(bad));
    bad[3] = '@';
    assert(!app_vault_recovery_normalize(bad, out, sizeof(out)));
    bad[3] = 'i';
    bad[4] = '~';
    assert(!app_vault_recovery_normalize(bad, out, sizeof(out)));

    // 输出缓冲太小 / 空指针
    assert(!app_vault_recovery_normalize(dashed, out, APP_VAULT_RECOVERY_CHARS));
    assert(!app_vault_recovery_normalize(NULL, out, sizeof(out)));
    assert(!app_vault_recovery_normalize(dashed, NULL, sizeof(out)));
}

// ---------------------------------------------------------------------------
// 8. 未注入随机源
// ---------------------------------------------------------------------------
static void test_entropy_unavailable(void)
{
    // 明文本上启用加密需要随机数：未注入必须报 ENTROPY，且模式不变。
    app_vault_set_random(NULL, NULL);

    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "l", "a", "p");
    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_ERR_ENTROPY);
    assert(v.mode == APP_VAULT_PLAIN);

    // 加密本上改手势 / 换恢复码 / 序列化都需要随机数。
    inject_random();
    app_vault_t e;
    app_vault_init(&e);
    must_add(&e, "l", "a", "p");
    assert(app_vault_enable_encryption(&e, &k) == APP_VAULT_OK);

    app_vault_knock_t k2 = knock_upup();
    app_vault_set_random(NULL, NULL);
    assert(app_vault_change_knock(&e, &k2) == APP_VAULT_ERR_ENTROPY);
    assert(app_vault_new_recovery_code(&e) == APP_VAULT_ERR_ENTROPY);

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    assert(app_vault_serialize(&e, buf, sizeof(buf)) == 0);

    // 解锁不需要随机数
    app_vault_lock(&e);
    assert(app_vault_unlock_knock(&e, &k) == APP_VAULT_OK);

    inject_random();   // 恢复注入，避免影响后续用例
}

// ---------------------------------------------------------------------------
// 9. 启用加密
// ---------------------------------------------------------------------------
static void test_enable_encryption(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");
    must_add(&v, "bank", "alice", "pw-3");

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);
    assert(v.mode == APP_VAULT_ENCRYPTED);
    assert(app_vault_is_encrypted(&v));
    assert(!app_vault_is_locked(&v));        // 本次会话保持解锁
    assert(v.count == 3);
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");
    assert_entry_eq(app_vault_at(&v, 2), "bank", "alice", "pw-3");

    // 恢复码：31 个字符、5 个分隔位
    const char *code = app_vault_pending_recovery(&v);
    assert(strlen(code) == APP_VAULT_RECOVERY_LEN);
    for (int i = 0; i < APP_VAULT_RECOVERY_LEN; i++) {
        bool dash = (i == 5 || i == 11 || i == 17 || i == 23 || i == 29);
        assert((code[i] == '-') == dash);
    }

    // 手势太短 -> RANGE；NULL 手势也拒绝
    const uint8_t short_taps[APP_VAULT_KNOCK_MIN - 1] = { APP_VAULT_TAP_UP, APP_VAULT_TAP_UP, APP_VAULT_TAP_UP };
    app_vault_knock_t too_short = knock_of(short_taps, APP_VAULT_KNOCK_MIN - 1);
    app_vault_t p;
    app_vault_init(&p);
    assert(app_vault_enable_encryption(&p, &too_short) == APP_VAULT_ERR_RANGE);
    assert(app_vault_enable_encryption(&p, NULL) == APP_VAULT_ERR_RANGE);
    assert(p.mode == APP_VAULT_PLAIN);

    // 已经加密再启用 -> STATE
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_ERR_STATE);
}

// ---------------------------------------------------------------------------
// 10. 上锁 / 解锁（含重启路径）
// ---------------------------------------------------------------------------
static void test_lock_unlock(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");
    must_add(&v, "bank", "alice", "pw-3");

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 0 && n <= APP_VAULT_CONTAINER_MAX);

    app_vault_entry_t expect[3];
    memcpy(expect, v.entries, sizeof(expect));

    app_vault_lock(&v);
    assert(app_vault_is_locked(&v));
    assert(app_vault_at(&v, 0) == NULL);
    assert(app_vault_at(&v, 2) == NULL);
    assert(v.count == 3);   // 条目数保留，供锁定界面显示
    assert(app_vault_add(&v, "x", NULL, NULL, NULL) == APP_VAULT_ERR_LOCKED);
    assert(app_vault_update(&v, 0, "x", NULL, NULL) == APP_VAULT_ERR_LOCKED);
    assert(app_vault_remove(&v, 0) == APP_VAULT_ERR_LOCKED);

    // 错误手势：认证失败且保持锁定
    app_vault_knock_t wrong = knock_dduu();
    assert(app_vault_unlock_knock(&v, &wrong) == APP_VAULT_ERR_AUTH);
    assert(app_vault_is_locked(&v));
    assert(app_vault_at(&v, 0) == NULL);

    // 正确手势：条目逐字节还原
    assert(app_vault_unlock_knock(&v, &k) == APP_VAULT_OK);
    assert(!app_vault_is_locked(&v));
    for (int i = 0; i < 3; i++) {
        assert(memcmp(&v.entries[i], &expect[i], sizeof(app_vault_entry_t)) == 0);
    }
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");

    // 重启路径：锁定态序列化 -> 反序列化 -> 手势解锁
    app_vault_lock(&v);
    size_t m = app_vault_serialize(&v, buf, sizeof(buf));
    assert(m == n);   // 锁定态原样写回，长度不变

    app_vault_t r;
    memset(&r, 0xCC, sizeof(r));
    assert(app_vault_deserialize(&r, buf, m) == APP_VAULT_OK);
    assert(app_vault_is_locked(&r) && r.count == 3);
    assert(app_vault_at(&r, 0) == NULL);
    assert(app_vault_unlock_knock(&r, &wrong) == APP_VAULT_ERR_AUTH);
    assert(app_vault_is_locked(&r));
    assert(app_vault_unlock_knock(&r, &k) == APP_VAULT_OK);
    for (int i = 0; i < 3; i++) {
        assert(memcmp(&r.entries[i], &expect[i], sizeof(app_vault_entry_t)) == 0);
    }
}

// ---------------------------------------------------------------------------
// 11. 恢复码解锁
// ---------------------------------------------------------------------------
static void test_recovery_unlock(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);

    // 抄下恢复码（上锁会清掉待展示值）
    char code[APP_VAULT_RECOVERY_LEN + 1];
    const char *pending = app_vault_pending_recovery(&v);
    assert(strlen(pending) == APP_VAULT_RECOVERY_LEN);
    memcpy(code, pending, sizeof(code));

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    assert(app_vault_serialize(&v, buf, sizeof(buf)) > 0);

    // 错误恢复码：认证失败，保持锁定
    char wrong[APP_VAULT_RECOVERY_LEN + 1];
    memcpy(wrong, code, sizeof(wrong));
    wrong[0] = (wrong[0] == '0') ? '1' : '0';
    app_vault_lock(&v);
    assert(app_vault_unlock_recovery(&v, wrong) == APP_VAULT_ERR_AUTH);
    assert(app_vault_is_locked(&v));

    // 小写 + 去掉分隔线的写法同样可用
    char flat[APP_VAULT_RECOVERY_CHARS + 1];
    int pos = 0;
    for (const char *p = code; *p; p++) {
        if (*p == '-') continue;
        flat[pos++] = (char)((*p >= 'A' && *p <= 'Z') ? (*p - 'A' + 'a') : *p);
    }
    flat[pos] = '\0';
    assert(pos == APP_VAULT_RECOVERY_CHARS);
    assert(app_vault_unlock_recovery(&v, flat) == APP_VAULT_OK);
    assert(!app_vault_is_locked(&v));
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");
    assert_entry_eq(app_vault_at(&v, 1), "github", "octocat", "pw-2");

    // 规范带分隔线写法
    app_vault_lock(&v);
    assert(app_vault_unlock_recovery(&v, code) == APP_VAULT_OK);

    // 明文模式下不允许恢复码解锁
    app_vault_t p;
    app_vault_init(&p);
    assert(app_vault_unlock_recovery(&p, code) == APP_VAULT_ERR_STATE);
}

// ---------------------------------------------------------------------------
// 12. 改手势
// ---------------------------------------------------------------------------
static void test_change_knock(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");

    app_vault_knock_t old_knock = knock_udud();
    app_vault_knock_t new_knock = knock_upup();
    assert(app_vault_enable_encryption(&v, &old_knock) == APP_VAULT_OK);
    assert(app_vault_serialize(&v, (uint8_t[APP_VAULT_CONTAINER_MAX]){0}, APP_VAULT_CONTAINER_MAX) > 0);

    assert(app_vault_change_knock(&v, &new_knock) == APP_VAULT_OK);

    // 旧手势不再能解锁，新手势可以，条目仍在
    app_vault_lock(&v);
    assert(app_vault_unlock_knock(&v, &old_knock) == APP_VAULT_ERR_AUTH);
    assert(app_vault_is_locked(&v));
    assert(app_vault_unlock_knock(&v, &new_knock) == APP_VAULT_OK);
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");
    assert_entry_eq(app_vault_at(&v, 1), "github", "octocat", "pw-2");

    // 改完手势立刻上锁序列化，重启后新手势仍能解锁（MAC 必须同步刷新）
    app_vault_lock(&v);
    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 0);
    app_vault_t r;
    memset(&r, 0xCC, sizeof(r));
    assert(app_vault_deserialize(&r, buf, n) == APP_VAULT_OK);
    assert(app_vault_unlock_knock(&r, &old_knock) == APP_VAULT_ERR_AUTH);
    assert(app_vault_unlock_knock(&r, &new_knock) == APP_VAULT_OK);
    assert_entry_eq(app_vault_at(&r, 1), "github", "octocat", "pw-2");

    // 未解锁时改手势 -> 非 OK
    app_vault_lock(&v);
    assert(app_vault_change_knock(&v, &old_knock) == APP_VAULT_ERR_LOCKED);

    // 明文模式 -> STATE
    app_vault_t p;
    app_vault_init(&p);
    assert(app_vault_change_knock(&p, &new_knock) == APP_VAULT_ERR_STATE);

    // 解锁后传非法手势 -> RANGE
    assert(app_vault_unlock_knock(&v, &new_knock) == APP_VAULT_OK);
    const uint8_t bad_taps[APP_VAULT_KNOCK_MIN - 1] = { APP_VAULT_TAP_UP, APP_VAULT_TAP_UP, APP_VAULT_TAP_UP };
    app_vault_knock_t too_short = knock_of(bad_taps, APP_VAULT_KNOCK_MIN - 1);
    assert(app_vault_change_knock(&v, &too_short) == APP_VAULT_ERR_RANGE);
}

// ---------------------------------------------------------------------------
// 13. 换恢复码
// ---------------------------------------------------------------------------
static void test_new_recovery_code(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);

    char old_code[APP_VAULT_RECOVERY_LEN + 1];
    memcpy(old_code, app_vault_pending_recovery(&v), sizeof(old_code));
    assert(strlen(old_code) == APP_VAULT_RECOVERY_LEN);

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    assert(app_vault_serialize(&v, buf, sizeof(buf)) > 0);

    assert(app_vault_new_recovery_code(&v) == APP_VAULT_OK);
    const char *new_pending = app_vault_pending_recovery(&v);
    assert(strlen(new_pending) == APP_VAULT_RECOVERY_LEN);
    char new_code[APP_VAULT_RECOVERY_LEN + 1];
    memcpy(new_code, new_pending, sizeof(new_code));
    assert(strcmp(new_code, old_code) != 0);

    // 旧码失效，新码可用，条目完好
    app_vault_lock(&v);
    assert(app_vault_unlock_recovery(&v, old_code) == APP_VAULT_ERR_AUTH);
    assert(app_vault_is_locked(&v));
    assert(app_vault_unlock_recovery(&v, new_code) == APP_VAULT_OK);
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");

    // 清除待展示恢复码
    app_vault_clear_pending_recovery(&v);
    assert(strcmp(app_vault_pending_recovery(&v), "") == 0);

    // 锁定态 -> LOCKED；明文 -> STATE
    app_vault_lock(&v);
    assert(app_vault_new_recovery_code(&v) == APP_VAULT_ERR_LOCKED);
    app_vault_t p;
    app_vault_init(&p);
    assert(app_vault_new_recovery_code(&p) == APP_VAULT_ERR_STATE);
}

// ---------------------------------------------------------------------------
// 14. 退回明文
// ---------------------------------------------------------------------------
static void test_disable_encryption(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);
    assert(strlen(app_vault_pending_recovery(&v)) == APP_VAULT_RECOVERY_LEN);

    assert(app_vault_disable_encryption(&v) == APP_VAULT_OK);
    assert(v.mode == APP_VAULT_PLAIN && !app_vault_is_encrypted(&v));
    assert(!app_vault_is_locked(&v));
    assert(v.count == 2);
    assert_entry_eq(app_vault_at(&v, 0), "邮箱", "me@example.com", "pw-1");
    assert_entry_eq(app_vault_at(&v, 1), "github", "octocat", "pw-2");
    assert(strcmp(app_vault_pending_recovery(&v), "") == 0);

    // 已明文再退回 -> STATE
    assert(app_vault_disable_encryption(&v) == APP_VAULT_ERR_STATE);

    // 退回后写出的是明文容器，可直接反序列化且无需解锁
    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 8);
    app_vault_t r;
    memset(&r, 0xCC, sizeof(r));
    assert(app_vault_deserialize(&r, buf, n) == APP_VAULT_OK);
    assert(r.mode == APP_VAULT_PLAIN && !app_vault_is_locked(&r) && r.count == 2);
    assert_entry_eq(app_vault_at(&r, 1), "github", "octocat", "pw-2");
}

// ---------------------------------------------------------------------------
// 15. 篡改检测
// ---------------------------------------------------------------------------
static void test_tamper_detection(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);
    must_add(&v, "邮箱", "me@example.com", "pw-1");
    must_add(&v, "github", "octocat", "pw-2");

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 0);

    // 控制组：原样容器可以解锁
    app_vault_t good;
    assert(app_vault_deserialize(&good, buf, n) == APP_VAULT_OK);
    assert(app_vault_unlock_knock(&good, &k) == APP_VAULT_OK);

    // 封装的 DEK 位于固定头部；payload 从 APP_VAULT_ENC_HEADER 开始
    const size_t wrap_knock_offset = 4 + 1 + 1 + 2 + 16 + 16 + 4 + 16;

    struct {
        const char *name;
        size_t      offset;
    } cases[] = {
        { "payload",        APP_VAULT_ENC_HEADER },   // 密文正文
        { "mac",            n - 1 },                  // MAC 末字节
        { "wrapped_dek",    wrap_knock_offset },      // 被封装 DEK 的首字节
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t bad[APP_VAULT_CONTAINER_MAX];
        memcpy(bad, buf, n);
        bad[cases[c].offset] ^= 0x01;

        app_vault_t r;
        memset(&r, 0xCC, sizeof(r));
        // 结构仍然完整，能解析出来
        assert(app_vault_deserialize(&r, bad, n) == APP_VAULT_OK);
        assert(app_vault_is_locked(&r));
        // 但解锁必须失败，且不能吐出垃圾条目
        app_vault_status_t status = app_vault_unlock_knock(&r, &k);
        assert(status != APP_VAULT_OK);
        assert(app_vault_is_locked(&r));
        assert(app_vault_at(&r, 0) == NULL);
        (void)cases[c].name;
    }

    // 加密容器被截断：失败且把目标重置为干净的空明文本
    app_vault_t t;
    app_vault_init(&t);
    assert(app_vault_deserialize(&t, buf, n - 1) == APP_VAULT_ERR_FORMAT);
    assert(t.mode == APP_VAULT_PLAIN && t.count == 0 && !app_vault_is_locked(&t));
    assert(t.payload_len == 0);

    // 加密容器把 payload_len 改坏：同样失败并重置
    // 固定头布局：magic(4) ver(1) mode(1) count(2) salt_knock(16) salt_recovery(16)
    // iterations(4) iv_wrap_knock(16) wrap_knock(48) iv_wrap_recovery(16) wrap_recovery(48)
    // -> payload_len 从偏移 172 开始（4 字节），iv_payload 在 176。
    uint8_t bad_len[APP_VAULT_CONTAINER_MAX];
    memcpy(bad_len, buf, n);
    bad_len[172] = 0x00;   // payload_len 低字节改成 0 -> 长度非法
    app_vault_init(&t);
    assert(app_vault_deserialize(&t, bad_len, n) == APP_VAULT_ERR_FORMAT);
    assert(t.mode == APP_VAULT_PLAIN && t.count == 0);
}

// ---------------------------------------------------------------------------
// 16. 容量边界
// ---------------------------------------------------------------------------
static void test_boundary(void)
{
    inject_random();
    app_vault_t v;
    app_vault_init(&v);

    char label_in[64];
    char account_in[64];
    char password_in[64];
    for (int i = 0; i < APP_VAULT_MAX; i++) {
        make_ascii(label_in, sizeof(label_in), i, 40);
        make_ascii(account_in, sizeof(account_in), i, 40);
        make_ascii(password_in, sizeof(password_in), i, 40);
        assert(app_vault_add(&v, label_in, account_in, password_in, NULL) == APP_VAULT_OK);
    }
    assert(v.count == APP_VAULT_MAX);

    uint8_t buf[APP_VAULT_CONTAINER_MAX];
    size_t n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 0 && n <= APP_VAULT_CONTAINER_MAX);

    app_vault_knock_t k = knock_udud();
    assert(app_vault_enable_encryption(&v, &k) == APP_VAULT_OK);
    n = app_vault_serialize(&v, buf, sizeof(buf));
    assert(n > 0 && n <= APP_VAULT_CONTAINER_MAX);

    app_vault_lock(&v);
    size_t m = app_vault_serialize(&v, buf, sizeof(buf));
    assert(m == n);

    app_vault_t r;
    memset(&r, 0xCC, sizeof(r));
    assert(app_vault_deserialize(&r, buf, m) == APP_VAULT_OK);
    assert(app_vault_is_locked(&r) && r.count == APP_VAULT_MAX);
    assert(app_vault_unlock_knock(&r, &k) == APP_VAULT_OK);
    for (int i = 0; i < APP_VAULT_MAX; i++) {
        make_ascii(label_in, sizeof(label_in), i, 40);
        make_ascii(account_in, sizeof(account_in), i, 40);
        make_ascii(password_in, sizeof(password_in), i, 40);
        char want_label[APP_VAULT_LABEL_LEN];
        char want_account[APP_VAULT_ACCOUNT_LEN];
        char want_password[APP_VAULT_PASSWORD_LEN];
        memcpy(want_label, label_in, 8);
        want_label[8] = '\0';
        memcpy(want_account, account_in, 16);
        want_account[16] = '\0';
        memcpy(want_password, password_in, 16);
        want_password[16] = '\0';
        assert_entry_eq(app_vault_at(&r, i), want_label, want_account, want_password);
    }

    // 缓冲不足：必须返回 0，绝不越界写
    uint8_t small[APP_VAULT_CONTAINER_MAX];
    assert(app_vault_serialize(&r, small, 8) == 0);
    assert(app_vault_serialize(&r, small, n - 1) == 0);
    assert(app_vault_serialize(&r, small, 0) == 0);
    // 足量缓冲仍然成功，长度与容量无关
    assert(app_vault_serialize(&r, small, sizeof(small)) == n);
}

// ---------------------------------------------------------------------------
// 17. 空指针与文案
// ---------------------------------------------------------------------------
static void test_null_and_text(void)
{
    assert(!app_vault_is_locked(NULL));
    assert(!app_vault_is_encrypted(NULL));
    assert(app_vault_at(NULL, 0) == NULL);
    assert(app_vault_selected(NULL) == -1);
    assert(strcmp(app_vault_pending_recovery(NULL), "") == 0);
    assert(app_vault_serialize(NULL, NULL, 0) == 0);
    assert(app_vault_deserialize(NULL, NULL, 0) == APP_VAULT_ERR_RANGE);
    assert(app_vault_add(NULL, "a", "b", "c", NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_update(NULL, 0, "a", "b", "c") == APP_VAULT_ERR_RANGE);
    assert(app_vault_remove(NULL, 0) == APP_VAULT_ERR_RANGE);
    assert(app_vault_enable_encryption(NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_disable_encryption(NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_change_knock(NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_new_recovery_code(NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_unlock_knock(NULL, NULL) == APP_VAULT_ERR_RANGE);
    assert(app_vault_unlock_recovery(NULL, "x") == APP_VAULT_ERR_RANGE);
    assert(!app_vault_knock_push(NULL, APP_VAULT_TAP_UP));
    assert(!app_vault_knock_pop(NULL));
    assert(!app_vault_knock_valid(NULL));
    assert(!app_vault_knock_same(NULL, NULL));
    app_vault_knock_clear(NULL);
    app_vault_select(NULL, 0);
    app_vault_cycle(NULL, 1);
    app_vault_lock(NULL);
    app_vault_clear_pending_recovery(NULL);
    app_vault_init(NULL);

    // 文案接口对已知与未知取值都返回非空串，且不同状态不同文案
    assert(app_vault_status_text(APP_VAULT_OK) != NULL);
    assert(strcmp(app_vault_status_text(APP_VAULT_OK), app_vault_status_text(APP_VAULT_ERR_AUTH)) != 0);
    assert(app_vault_status_text((app_vault_status_t)77) != NULL);
    assert(app_vault_mode_name(APP_VAULT_PLAIN) != NULL);
    assert(strcmp(app_vault_mode_name(APP_VAULT_PLAIN), app_vault_mode_name(APP_VAULT_ENCRYPTED)) != 0);
    assert(app_vault_mode_name((app_vault_mode_t)77) != NULL);
}

int main(void)
{
    inject_random();

    test_init();
    test_plain_crud();
    test_selection_cycle();
    test_utf8_truncation();
    test_plain_serialization();
    test_knock_tools();
    test_recovery_tools();
    test_entropy_unavailable();
    test_enable_encryption();
    test_lock_unlock();
    test_recovery_unlock();
    test_change_knock();
    test_new_recovery_code();
    test_disable_encryption();
    test_tamper_detection();
    test_boundary();
    test_null_and_text();

    puts("test_app_vault: PASS");
    return 0;
}
