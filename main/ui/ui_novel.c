// main/ui/ui_novel.c —— 小说：离线阅读（正文由手机配置页上传，见 main/app_novel.c）。
//
// 一屏只做一件事：把正文分页显示出来。分页、章节识别都在 logic/app_novel 里做纯计算，
// 本页只负责"把算好的这一页画出来"与按键导航。
//
// 按键：↑ 上一页  ↓ 下一页  OK 目录/跳转  长按↑ 回书首  长按↓ 跳到书尾  长按OK 返回。
// 目录视图里：↑↓ 选章  OK 跳到该章  长按OK 回到正文。
//
// 阅读进度（正文字节偏移 + 书的 CRC）存在 NVS 里，翻页时节流写入，退出时必写一次。
#include <stdbool.h>

#include "ui_pages.h"
#include "ui_novel.h"

#include "ui_theme.h"
#include "ui_app.h"

#include "app_novel.h"
#include "app_state.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// 排版参数：一行 26 个半角单位（= 13 个汉字，约 208px，留出右侧余量），一页 10 行。
// 正文标签的高度按 10 行 x 20px 估，比 16px 字体的实际行高留了一点余量。
#define NV_UNITS_PER_LINE 26
#define NV_LINES_PER_PAGE 10

// 单页最多拷贝的字节数：10 行全汉字也只有 10*13*3 = 390 字节，留一倍余量。
#define NV_PAGE_BUF 512

// 目录一次只建这么多行：一本 96 章的书若全建出来，LVGL 对象数会顶到内存上限。
#define NV_TOC_ROWS 5

// 没有章节标记时提供的百分比跳转点。
static const int NV_PERCENT_STEPS[] = { 10, 25, 50, 75, 90 };
#define NV_PERCENT_COUNT ((int)(sizeof(NV_PERCENT_STEPS) / sizeof(NV_PERCENT_STEPS[0])))

typedef enum {
    NV_READ = 0,
    NV_TOC,
} nv_view_t;

static struct {
    bool active;
    ui_page_t page;
    nv_view_t view;

    const uint8_t *text;               // mmap 的正文（整本）
    uint32_t len;
    esp_partition_mmap_handle_t map;
    app_novel_header_t header;

    uint32_t off;                      // 当前页起始偏移
    uint32_t end;                      // 当前页结束偏移

    lv_obj_t *title_lbl;
    lv_obj_t *pct_lbl;
    lv_obj_t *body_lbl;

    int toc_focus;                     // 目录里的全局选中下标
    int toc_base;                      // 目录窗口起始下标
    ui_row_t toc_rows[NV_TOC_ROWS];
    int toc_row_count;

    uint32_t last_save_ms;             // 上次写进度的时刻（节流）
} s;

// 阅读视图的构建在目录跳转里被用到，先声明。
static void build_read(void);

static app_novel_layout_t layout(void)
{
    app_novel_layout_t l = { .units_per_line = NV_UNITS_PER_LINE,
                             .lines_per_page = NV_LINES_PER_PAGE };
    return l;
}

// ---------------------------------------------------------------------------
// 正文渲染
// ---------------------------------------------------------------------------

// 字体里没有这个码点吗？没有就替换成可见的"□"，而不是让 LVGL 画成空白——
// 生僻字在小说里难免出现，用户至少要知道"这里有一个字没显示出来"。
static bool glyph_missing(uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    return !lv_font_get_glyph_dsc(ui_font_body, &dsc, cp, 0);
}

static void fill_body(void)
{
    if (!s.body_lbl || !s.text) return;

    const char *text = (const char *)s.text;
    char buf[NV_PAGE_BUF];
    size_t out = 0;
    size_t pos = s.off;
    while (pos < s.end && out + 4 < sizeof(buf)) {
        size_t start = pos;
        uint32_t cp = app_novel_utf8_next(text, s.len, &pos);

        if (cp == '\r') continue;                 // CRLF 文本的 CR 不参与显示
        if (glyph_missing(cp)) {
            memcpy(buf + out, "\xE2\x96\xA1", 3); // U+25A1 □
            out += 3;
            continue;
        }
        size_t n = pos - start;
        memcpy(buf + out, text + start, n);
        out += n;
    }
    buf[out] = '\0';
    lv_label_set_text(s.body_lbl, buf);

    if (s.pct_lbl) {
        unsigned pct = s.len ? (unsigned)((uint64_t)s.off * 100 / s.len) : 0;
        lv_label_set_text_fmt(s.pct_lbl, "%u%%", pct);
    }
}

static void save_pos(bool force)
{
    uint32_t now = lv_tick_get();
    if (!force && now - s.last_save_ms < 3000) return;   // 节流：翻页不必每页都落盘
    s.last_save_ms = now;
    app_state_set_novel_pos(s.off, s.header.data_crc);
}

// 跳到某处并重画正文（偏移先吸附到整页页首）。
static void goto_offset(uint32_t target, bool save)
{
    if (target >= s.len) target = 0;                     // 到底了再往下翻 → 回到书首
    const char *text = (const char *)s.text;
    app_novel_layout_t l = layout();
    s.off = (uint32_t)app_novel_page_floor(text, s.len, target, &l);
    s.end = (uint32_t)app_novel_page_next(text, s.len, s.off, &l);
    fill_body();
    if (save) save_pos(false);
}

// ---------------------------------------------------------------------------
// 目录视图
// ---------------------------------------------------------------------------

static int toc_total(void)
{
    int chapters = app_novel_chapter_count();
    return chapters > 0 ? chapters : NV_PERCENT_COUNT;
}

static void render_toc(void)
{
    if (!s.page.content) return;
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    memset(s.toc_rows, 0, sizeof(s.toc_rows));
    s.toc_row_count = 0;

    int total = toc_total();
    bool by_chapter = app_novel_chapter_count() > 0;
    char right[24];
    if (by_chapter) snprintf(right, sizeof(right), "%d 章", total);
    else            snprintf(right, sizeof(right), "%d 档", total);
    ui_header_create(c, "目录", right, NULL, NULL);

    lv_obj_t *list = ui_list_create(c);
    int count = total < NV_TOC_ROWS ? total : NV_TOC_ROWS;
    for (int i = 0; i < count; i++) {
        int idx = s.toc_base + i;
        if (idx >= total) break;

        char label[64];
        if (by_chapter) {
            app_novel_chapter_t ch;
            if (!app_novel_chapter(idx, &ch)) continue;
            snprintf(label, sizeof(label), "%s", ch.title);
        } else {
            snprintf(label, sizeof(label), "跳到全书 %d%%", NV_PERCENT_STEPS[idx]);
        }
        s.toc_rows[i] = ui_row_create(list, label, NULL);
        s.toc_row_count++;
        if (idx == s.toc_focus) ui_row_set_selected(s.toc_rows[i], true);
    }
    ui_page_set_hint("↑↓ 选择  OK 跳转  长按OK 回到正文");
}

// 目录视图的选中项滚动：窗口跟着焦点走，避免一次建出上百行。
static void toc_move(int delta)
{
    int total = toc_total();
    if (total <= 0) return;

    s.toc_focus = (s.toc_focus + delta + total) % total;
    if (s.toc_focus < s.toc_base) {
        s.toc_base = s.toc_focus;
        render_toc();
        return;
    }
    if (s.toc_focus >= s.toc_base + s.toc_row_count) {
        s.toc_base = s.toc_focus - s.toc_row_count + 1;
        render_toc();
        return;
    }
    for (int i = 0; i < s.toc_row_count; i++) {
        ui_row_set_selected(s.toc_rows[i], (s.toc_base + i) == s.toc_focus);
    }
}

static void toc_activate(void)
{
    int total = toc_total();
    if (total <= 0 || s.toc_focus >= total) return;

    uint32_t target = 0;
    if (app_novel_chapter_count() > 0) {
        app_novel_chapter_t ch;
        if (!app_novel_chapter(s.toc_focus, &ch)) return;
        target = ch.offset;
    } else {
        target = (uint32_t)((uint64_t)s.len * NV_PERCENT_STEPS[s.toc_focus] / 100);
    }

    s.view = NV_READ;
    build_read();          // 先回到阅读视图，再定位
    goto_offset(target, true);
}

// ---------------------------------------------------------------------------
// 阅读视图
// ---------------------------------------------------------------------------

static void build_read(void)
{
    lv_obj_t *c = s.page.content;
    lv_obj_clean(c);
    s.title_lbl = NULL;
    s.pct_lbl = NULL;
    s.body_lbl = NULL;

    char right[16] = { 0 };
    unsigned pct = s.len ? (unsigned)((uint64_t)s.off * 100 / s.len) : 0;
    snprintf(right, sizeof(right), "%u%%", pct);
    ui_header_create(c, s.header.title, right, &s.title_lbl, &s.pct_lbl);

    s.body_lbl = ui_label_create(c, "", ui_font_body, ui_c_text());
    lv_obj_set_width(s.body_lbl, LV_PCT(100));
    lv_label_set_long_mode(s.body_lbl, LV_LABEL_LONG_WRAP);

    fill_body();
    ui_page_set_hint("↑↓ 翻页  OK 目录  长按↑↓ 首/尾  长按OK 返回");
}

static void clear_page(void)
{
    if (s.page.content) lv_obj_clean(s.page.content);
    s.title_lbl = NULL;
    s.pct_lbl = NULL;
    s.body_lbl = NULL;
}

// ---------------------------------------------------------------------------
// 页面接口
// ---------------------------------------------------------------------------

void page_novel_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.page = ui_page_create(NULL);

    if (!app_novel_ready()) {
        ui_empty_create(s.page.content, "小说存储不可用",
                        "本机固件没有可用的 novel 分区，无法保存小说");
        ui_page_set_hint("长按OK 返回");
        return;
    }
    if (app_novel_header(&s.header) != ESP_OK) {
        ui_empty_create(s.page.content, "还没有小说",
                        "在手机配置页上传 UTF-8 的 TXT：系统设置 → 开启配网 → 小说");
        ui_page_set_hint("长按OK 返回");
        return;
    }
    if (app_novel_map(&s.text, &s.len, &s.map) != ESP_OK) {
        ui_empty_create(s.page.content, "小说打不开",
                        "正文映射失败，请重新上传一次");
        ui_page_set_hint("长按OK 返回");
        return;
    }

    // 恢复阅读进度：只有 CRC 对得上（还是同一本书）才用旧偏移。
    uint32_t start = 0;
    if (app_state_novel_crc() == s.header.data_crc) start = app_state_novel_offset();
    s.view = NV_READ;
    build_read();
    goto_offset(start, false);
    save_pos(true);
}

void page_novel_exit(void)
{
    if (!s.active && !s.page.scr) return;
    if (s.text) save_pos(true);
    app_novel_unmap(s.map);
    s.text = NULL;
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_novel_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.active || !s.page.scr || !s.text) return;

    if (s.view == NV_TOC) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            s.view = NV_READ;
            build_read();
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) toc_move(-1);
        else if (btn == BSP_BTN_DOWN) toc_move(1);
        else if (btn == BSP_BTN_OK) toc_activate();
        return;
    }

    if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) {
            ui_app_go_home();
        } else if (btn == BSP_BTN_UP) {
            goto_offset(0, true);
        } else if (btn == BSP_BTN_DOWN) {
            goto_offset(s.len ? s.len - 1 : 0, true);
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    app_novel_layout_t l = layout();
    if (btn == BSP_BTN_UP) {
        if (s.off == 0) {
            ui_hint_flash("已经是第一页", 1200);
            return;
        }
        goto_offset((uint32_t)app_novel_page_prev((const char *)s.text, s.len, s.off, &l), true);
    } else if (btn == BSP_BTN_DOWN) {
        if (s.end >= s.len) {
            ui_hint_flash("已经读到结尾", 1200);
            return;
        }
        goto_offset(s.end, true);
    } else if (btn == BSP_BTN_OK) {
        s.view = NV_TOC;
        s.toc_focus = 0;
        s.toc_base = 0;
        render_toc();
    }
}

void page_novel_tick(void)
{
    // 正文是静态内容，不需要按节拍重绘；进度只在翻页与退出时落盘。
}