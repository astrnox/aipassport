// main/ui/ui_esports.c —— 英雄联盟赛事中心模块页（赛程 / 积分榜 / 战队）。
//
// 本文件实现 ui_pages.h 声明的 page_esports_* 四个回调，注册到 ui_app.c 的模块表。
// 设计要点（对应 PRD 6.4.6、7、8 章）：
//  - 缓存优先：enter() 先用 app_state_esports() 的本地缓存渲染，再调 app_net_esports_fetch()
//    后台刷新，弱网下不会长时间空屏；拉取结果返回后由 tick() 原地重绘。
//  - 离线降级：顶部横幅标注数据新鲜度（离线/正在刷新/失败原因），失败提供"长按↑重试"。
//  - 优先级排序：赛程列表调用 app_esport_sort() 排序后渲染，进行中最高、已结束最弱。
//  - 二级下钻：单场详情为本页内部视图，进入时单独请求一次对局详情（app_net_esport_detail_fetch），
//    拿到阵容/经济/选手后原地重绘；请求失败或接口无数据时回退到缓存比分，并说明原因。
//
// 按键约定（底部提示条如实写明）：
//   根列表  短按 UP/DOWN 移动或切换赛区，短按 OK 主操作，长按 OK 返回主页，
//           长按 DOWN 切到下一标签页，长按 UP 在赛程页刷新、在其它页切到上一标签页。
//   单场详情 长按 UP/DOWN 在「阵容/经济/选手」间切页，长按 OK 返回赛程列表。
//
// 说明：ui_pages.h 使用了 bool 但未自带 <stdbool.h>，本文件作为独立编译单元需先引入。
#include <stdbool.h>

#include "ui_pages.h"
#include "ui_theme.h"
#include "ui_app.h"

#include "app_state.h"
#include "logic/app_esports.h"
#include "net/app_net.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
// 主标签页下标。数量用于取模切页，改动时同步 TAB_COUNT。
enum {
    TAB_SCHEDULE = 0,
    TAB_STANDINGS,
    TAB_TEAMS,
    TAB_COUNT,
};

// 详情子页下标（阵容/经济/选手）。
enum {
    DETAIL_LINEUP = 0,
    DETAIL_GOLD,
    DETAIL_PLAYERS,
    DETAIL_TAB_COUNT,
};

// 位置中文名，索引与 app_esport_role_t 对齐。
static const char *const ROLE_NAMES[APP_ROLE_UNKNOWN + 1] = {
    "上单", "打野", "中单", "下路", "辅助", "—",
};

// 底部提示条文案。受 240px 宽度限制，尽量压缩空格但保留每个按键含义。
// 每一条都必须与 page_esports_key 里真实存在的分支一一对应，不能写没实现的按键。
static const char *const HINT_SCHEDULE  = "↑↓选 OK详情 长按↑刷新 长按↓切页 长按OK返回";
static const char *const HINT_STANDINGS = "↑↓滚动 OK换赛区 长按↑↓切页 长按OK返回";
static const char *const HINT_TEAMS     = "↑↓选 OK关注 长按↑↓切页 长按OK返回";
static const char *const HINT_DETAIL    = "↑↓滚动 长按↑↓切页 长按OK返回";

// 战队列表行：积分榜缓存与赛程去重两种来源统一成同一结构，便于选择与关注。
typedef struct {
    char name[16];   // 战队名
    int  win;        // 胜场，来源无统计时为 0
    int  loss;       // 负场
    int  points;     // 积分
    bool stats;      // 是否带胜负统计（积分榜缓存为 true，赛程去重为 false）
} esports_team_row_t;

// 页面全部状态。同一时刻只有一个赛事页在屏，故用单例静态。
typedef struct {
    ui_page_t page;                              // 页面骨架（状态栏/内容区/提示条）
    lv_obj_t *tabs;                              // 主标签页控件（赛程/积分榜/战队）
    lv_obj_t *banner;                            // 赛程页数据新鲜度横幅
    lv_obj_t *banner_label;                      // 横幅内文字标签，tick 原地更新用
    ui_row_t  rows[APP_ESPORT_MAX_MATCHES];      // 当前列表的行句柄，用于选中态刷新
    int       row_count;                         // 当前列表有效行数
    int       tab;                               // 当前主标签（TAB_*）
    int       selected;                          // 当前标签下的选中行下标
    int       league_idx;                        // 积分榜当前赛区下标（对应 app_net_league_*）
    bool      in_detail;                         // 是否处于单场详情二级视图
    int       detail_tab;                        // 详情子页 0=阵容 1=经济 2=选手
    app_esport_match_t detail;                   // 进入详情时拷贝的比赛，避免列表重排后下标漂移
    esports_team_row_t team_rows[APP_ESPORT_MAX_TEAMS]; // 战队标签页的行数据
    int       team_row_count;                    // 战队行数
    // tick 变更检测签名：任一变化即重绘当前列表，避免每秒无谓重绘。
    int       sig_fetched;                       // 上次见到的缓存拉取时刻
    int       sig_count;                         // 上次见到的赛程条数
    int       sig_teams;                         // 上次见到的战队条数
    int       sig_leagues;                       // 上次见到的赛区列表条数
    int       sig_state;                         // 上次见到的拉取状态
    bool      sig_valid;                         // 上次见到的缓存有效性
    int       detail_sig_state;                  // 上次见到的详情拉取状态
    int       detail_sig_fetched;                // 上次见到的详情更新时刻
} esports_ui_t;

static esports_ui_t s;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// 把 UTC 秒格式化为本地 HH:MM（应用时区偏移取自设置）。utc_sec<=0 表示未知，给出占位。
static void fmt_local_hm(int utc_sec, char *buf, size_t cap)
{
    if (utc_sec <= 0) {
        snprintf(buf, cap, "--:--");
        return;
    }
    int off_min = app_state_settings()->utc_offset_minutes;
    int local = utc_sec + off_min * 60;
    int day_min = ((local / 60) % 1440 + 1440) % 1440;
    snprintf(buf, cap, "%02d:%02d", day_min / 60, day_min % 60);
}

// 依据比赛状态与开赛时间给出左侧标题语义色：进行中/临期/已结束/其它。
static uint32_t match_state_color(const app_esport_match_t *m, int now_utc)
{
    switch (m->state) {
    case APP_MATCH_LIVE:
        return ui_c_live();
    case APP_MATCH_FINISHED:
        return ui_c_done();
    case APP_MATCH_UPCOMING: {
        int delta = m->start_utc - now_utc;
        if (delta >= 0 && delta <= 30 * 60) return ui_c_soon();
        return ui_c_dim();
    }
    default:
        return ui_c_dim();
    }
}

// 把一场比赛格式化成列表行的"左侧状态+战队比分"与"右侧局数或时间"。
static void format_match_row(const app_esport_match_t *m, int now_utc,
                             char *title, size_t tcap,
                             char *value, size_t vcap,
                             uint32_t *color)
{
    char hm[8];
    fmt_local_hm(m->start_utc, hm, sizeof(hm));
    *color = match_state_color(m, now_utc);

    switch (m->state) {
    case APP_MATCH_LIVE:
        // 进行中：给出实时比分，右侧显示当前局数（已结束局数 + 1）。
        snprintf(title, tcap, "● 进行中 %s %d:%d %s",
                 m->team_a, m->score_a, m->score_b, m->team_b);
        snprintf(value, vcap, "第%d局", m->score_a + m->score_b + 1);
        break;
    case APP_MATCH_FINISHED:
        // 已结束：给出最终比分，右侧显示开赛时间。
        snprintf(title, tcap, "✓ 已结束 %s %d:%d %s",
                 m->team_a, m->score_a, m->score_b, m->team_b);
        snprintf(value, vcap, "%s", hm);
        break;
    case APP_MATCH_UPCOMING:
    default: {
        int delta = m->start_utc - now_utc;
        const char *tag = (delta >= 0 && delta <= 30 * 60) ? "即将" : "未开始";
        snprintf(title, tcap, "○ %s %s vs %s", tag, m->team_a, m->team_b);
        snprintf(value, vcap, "%s", hm);
        break;
    }
    }
}

// 组装赛程页顶部横幅文案与底色。优先级：刷新中 > 失败 > 在线成功 > 离线缓存。
static void schedule_banner_text(char *buf, size_t cap, uint32_t *color)
{
    app_esport_cache_t *c = app_state_esports();
    app_fetch_state_t st = app_net_esports_state();

    if (st == APP_FETCH_RUNNING) {
        snprintf(buf, cap, "正在刷新…");
        *color = ui_c_accent();
        return;
    }
    if (st == APP_FETCH_FAILED) {
        const char *err = app_net_esports_error();
        snprintf(buf, cap, "刷新失败：%s · 长按↑重试", err ? err : "网络错误");
        *color = ui_c_warn();
        return;
    }

    char hm[8];
    fmt_local_hm(c->fetched_utc, hm, sizeof(hm));
    if (c->valid && st == APP_FETCH_OK && app_state_net() == APP_NET_ONLINE) {
        snprintf(buf, cap, "更新于 %s", hm);
        *color = ui_c_ok();
    } else {
        // 离线回看：明确标注离线与更新时间，符合第 7.4 节降级规则。
        snprintf(buf, cap, "离线 · 更新于 %s", hm);
        *color = ui_c_warn();
    }
}

// 原地刷新横幅文字与底色（tick 每秒调用，不重建列表）。
static void banner_update(void)
{
    if (!s.banner_label) return;
    char buf[96];
    uint32_t col;
    schedule_banner_text(buf, sizeof(buf), &col);
    lv_label_set_text(s.banner_label, buf);
    if (s.banner) lv_obj_set_style_bg_color(s.banner, lv_color_hex(col), 0);
}

// 清空内容区，准备整体重绘。
static void content_clear(void)
{
    if (s.page.content) lv_obj_clean(s.page.content);
    s.tabs = NULL;
    s.banner = NULL;
    s.banner_label = NULL;
    s.row_count = 0;
}

// 把选中态（左侧指示条 + 背景高亮）应用到当前行并滚动到可见区域。
static void apply_selection(void)
{
    for (int i = 0; i < s.row_count; i++) {
        bool sel = (i == s.selected);
        ui_row_set_selected(s.rows[i], sel);
        if (sel) ui_scroll_into_view(s.rows[i].obj);
    }
}

// ---------------------------------------------------------------------------
// 各视图渲染
// ---------------------------------------------------------------------------
// 赛程标签页：横幅 + 优先级排序后的比赛列表；无缓存时给出空状态引导配网。
static void render_schedule(void)
{
    app_esport_cache_t *c = app_state_esports();
    int now = (int)app_state_now_unix();
    app_fetch_state_t st = app_net_esports_state();

    // 仅在"有缓存"或"正在刷新/失败"时展示横幅，纯空状态不叠加无意义横幅。
    if (c->valid || st == APP_FETCH_RUNNING || st == APP_FETCH_FAILED) {
        char btext[96];
        uint32_t bcol;
        schedule_banner_text(btext, sizeof(btext), &bcol);
        s.banner = ui_banner_create(s.page.content, btext, bcol);
        s.banner_label = lv_obj_get_child(s.banner, 0);
    }

    if (!c->valid || c->match_count <= 0) {
        ui_empty_create(s.page.content, "暂无赛事数据",
                        "请先在设置中完成配网，联网后自动获取");
        s.selected = 0;
        return;
    }

    // 就地按优先级排序缓存；相同优先级保持输入顺序（app_esport_sort 为稳定插入排序）。
    app_esport_sort(c->matches, c->match_count, now);

    int n = c->match_count;
    if (n > APP_ESPORT_MAX_MATCHES) n = APP_ESPORT_MAX_MATCHES;
    for (int i = 0; i < n; i++) {
        char title[64];
        char value[24];
        uint32_t col;
        format_match_row(&c->matches[i], now, title, sizeof(title),
                         value, sizeof(value), &col);
        ui_row_t row = ui_row_create(s.page.content, title, value);
        ui_row_set_title_color(row, col);
        s.rows[i] = row;
    }
    s.row_count = n;
    if (s.selected >= s.row_count) s.selected = s.row_count - 1;
    if (s.selected < 0) s.selected = 0;
    apply_selection();
}

// 积分榜标签页：顶部当前赛区名 + 排名列表（前三名强调）。
static void render_standings(void)
{
    app_esport_cache_t *c = app_state_esports();
    int league_n = app_net_league_count();

    // 顶部一行显示当前赛区名。优先用已缓存的赛区列表名，其次用缓存里的赛区字段。
    char header[32];
    if (league_n > 0) {
        if (s.league_idx < 0) s.league_idx = 0;
        if (s.league_idx >= league_n) s.league_idx = league_n - 1;
        const char *nm = app_net_league_name(s.league_idx);
        snprintf(header, sizeof(header), "%s", nm ? nm : "赛区");
    } else if (c->standings_league[0]) {
        snprintf(header, sizeof(header), "%s", c->standings_league);
    } else {
        snprintf(header, sizeof(header), "暂无赛区");
    }
    lv_obj_t *hl = ui_label_create(s.page.content, header, ui_font_body, ui_c_accent());
    lv_obj_set_style_pad_left(hl, 4, 0);

    if (c->team_count <= 0) {
        ui_empty_create(s.page.content, "暂无积分榜数据",
                        "请联网后重试，或稍后回到本页");
        s.row_count = 0;
        s.selected = 0;
        return;
    }

    int n = c->team_count;
    if (n > APP_ESPORT_MAX_TEAMS) n = APP_ESPORT_MAX_TEAMS;
    for (int i = 0; i < n; i++) {
        const app_esport_team_t *t = &c->teams[i];
        char title[32];
        char value[48];
        snprintf(title, sizeof(title), "%d %s", i + 1, t->name);
        snprintf(value, sizeof(value), "%d-%d  %d分", t->win, t->loss, t->points);
        ui_row_t row = ui_row_create(s.page.content, title, value);
        ui_row_set_title_color(row, i < 3 ? ui_c_accent() : ui_c_text());
        s.rows[i] = row;
    }
    s.row_count = n;
    s.selected = 0;
}

// 构建战队标签页的行数据：优先用积分榜缓存，其次从赛程中去重收集战队名。
static void rebuild_team_rows(void)
{
    app_esport_cache_t *c = app_state_esports();
    s.team_row_count = 0;

    if (c->team_count > 0) {
        int n = c->team_count;
        if (n > APP_ESPORT_MAX_TEAMS) n = APP_ESPORT_MAX_TEAMS;
        for (int i = 0; i < n; i++) {
            esports_team_row_t *r = &s.team_rows[s.team_row_count++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", c->teams[i].name);
            r->win = c->teams[i].win;
            r->loss = c->teams[i].loss;
            r->points = c->teams[i].points;
            r->stats = true;
        }
        return;
    }

    // 缓存无战队统计时，从赛程里按出场顺序去重收集，保证战队页不为空。
    for (int i = 0; i < c->match_count && s.team_row_count < APP_ESPORT_MAX_TEAMS; i++) {
        const char *names[2] = { c->matches[i].team_a, c->matches[i].team_b };
        for (int k = 0; k < 2; k++) {
            const char *nm = names[k];
            if (nm[0] == '\0') continue;
            bool dup = false;
            for (int j = 0; j < s.team_row_count; j++) {
                if (strcmp(s.team_rows[j].name, nm) == 0) { dup = true; break; }
            }
            if (dup) continue;
            if (s.team_row_count >= APP_ESPORT_MAX_TEAMS) break;
            esports_team_row_t *r = &s.team_rows[s.team_row_count++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", nm);
            r->stats = false;
        }
    }
}

// 战队标签页：列出战队并标注关注状态（● 已关注 / ○ 未关注）。
static void render_teams(void)
{
    rebuild_team_rows();

    if (s.team_row_count <= 0) {
        ui_empty_create(s.page.content, "暂无战队数据",
                        "请联网获取赛程与积分榜");
        s.row_count = 0;
        s.selected = 0;
        return;
    }

    app_esport_cache_t *c = app_state_esports();
    for (int i = 0; i < s.team_row_count; i++) {
        bool followed = app_esport_is_followed(c, s.team_rows[i].name);
        char title[24];
        char value[32];
        snprintf(title, sizeof(title), "%s %s", followed ? "●" : "○",
                 s.team_rows[i].name);
        if (s.team_rows[i].stats) {
            snprintf(value, sizeof(value), "%d胜%d负", s.team_rows[i].win,
                     s.team_rows[i].loss);
        } else {
            snprintf(value, sizeof(value), "-");
        }
        ui_row_t row = ui_row_create(s.page.content, title, value);
        ui_row_set_title_color(row, followed ? ui_c_ok() : ui_c_text());
        s.rows[i] = row;
    }
    s.row_count = s.team_row_count;
    if (s.selected >= s.row_count) s.selected = s.row_count - 1;
    if (s.selected < 0) s.selected = 0;
    apply_selection();
}

// 取当前进入比赛的详情；尚未加载或属于其它比赛时返回 NULL。
static const app_esport_detail_t *current_detail(void)
{
    const app_esport_cache_t *c = app_state_esports();
    if (!c->detail.valid) return NULL;
    if (strcmp(c->detail.match_id, s.detail.id) != 0) return NULL;
    return &c->detail;
}

// 金币转 "45.2k"；不足 1000 时直接给整数，避免出现 "0.5k" 这种不好读的写法。
static void fmt_gold(int gold, char *buf, size_t cap)
{
    if (gold < 1000) snprintf(buf, cap, "%d", gold);
    else snprintf(buf, cap, "%d.%dk", gold / 1000, (gold % 1000) / 100);
}

// 阵容页：两队各 5 人，标题为"位置 英雄"，右侧为选手名。
static void render_detail_lineup(const app_esport_detail_t *d)
{
    const char *const names[2] = { s.detail.team_a, s.detail.team_b };
    const app_esport_player_t *sides[2] = { d->team_a, d->team_b };

    for (int t = 0; t < 2; t++) {
        lv_obj_t *hl = ui_label_create(s.page.content, names[t], ui_font_body,
                                       t == 0 ? ui_c_live() : ui_c_soon());
        lv_obj_set_style_pad_left(hl, 4, 0);
        for (int i = 0; i < APP_ESPORT_TEAM_PLAYERS; i++) {
            const char *role = ROLE_NAMES[sides[t][i].role];
            const char *champ = sides[t][i].champion[0] ? sides[t][i].champion : "—";
            const char *player = sides[t][i].player[0] ? sides[t][i].player : "—";
            char title[40];
            snprintf(title, sizeof(title), "%s %s", role, champ);
            ui_row_create(s.page.content, title, player);
        }
    }

    // 接口不提供禁用名单：明确说明，避免用户以为漏显示。
    lv_obj_t *note = ui_label_create(s.page.content, "禁用名单接口未提供",
                                     ui_font_hint, ui_c_dim());
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

// 经济曲线：两队经济差随时间的走势，中线为均势。采样点不足 2 个时不绘制。
static void render_gold_curve(const app_esport_detail_t *d)
{
    int n = d->gold_points;
    if (n < 2) return;
    if (n > APP_ESPORT_GOLD_POINTS) n = APP_ESPORT_GOLD_POINTS;

    // 点数组必须存活到控件销毁，用静态存储；每次重绘都会重建控件并重填。
    static lv_point_precise_t pts[APP_ESPORT_GOLD_POINTS];

    const int w = UI_W - 2 * UI_MARGIN_X - 24;
    const int h = 56;
    int max_abs = 1;
    for (int i = 0; i < n; i++) {
        int diff = d->gold_a[i] - d->gold_b[i];
        if (diff < 0) diff = -diff;
        if (diff > max_abs) max_abs = diff;
    }
    for (int i = 0; i < n; i++) {
        int diff = d->gold_a[i] - d->gold_b[i];
        pts[i].x = (w * i) / (n - 1);
        pts[i].y = h / 2 - (diff * (h / 2 - 2)) / max_abs;   // 折线在上=team_a 领先
    }

    lv_obj_t *box = lv_obj_create(s.page.content);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    lv_obj_t *line = lv_line_create(box);
    lv_line_set_points(line, pts, n);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(ui_c_accent()), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

// 经济页：两队总经济对比条 + 经济差曲线 + 采样说明。
static void render_detail_gold(const app_esport_detail_t *d)
{
    int ga = 0, gb = 0;
    for (int i = 0; i < APP_ESPORT_TEAM_PLAYERS; i++) {
        ga += d->team_a[i].gold;
        gb += d->team_b[i].gold;
    }

    if (ga <= 0 && gb <= 0) {
        ui_empty_create(s.page.content, "暂无经济数据",
                        "接口只对进行中的对局返回实时经济");
        return;
    }

    const char *const names[2] = { s.detail.team_a, s.detail.team_b };
    const int totals[2] = { ga, gb };
    const uint32_t colors[2] = { ui_c_live(), ui_c_soon() };
    const int total = ga + gb;

    for (int t = 0; t < 2; t++) {
        char gold[16];
        char buf[40];
        fmt_gold(totals[t], gold, sizeof(gold));
        snprintf(buf, sizeof(buf), "%s  %s", names[t], gold);
        lv_obj_t *l = ui_label_create(s.page.content, buf, ui_font_body, colors[t]);
        lv_obj_set_style_pad_left(l, 4, 0);

        lv_obj_t *bar = ui_progress_create(s.page.content, UI_W - 2 * UI_MARGIN_X, 8,
                                           colors[t]);
        if (total > 0) ui_progress_set(bar, totals[t] * 1000 / total);
    }

    // 中文全角括号加固定文案约 41 字节,再留出队名(15 字节)与结尾,48 字节放不下。
    char note[64];
    const char *text = "采样点不足，暂不显示经济走势";
    if (d->gold_points >= 2) {
        snprintf(note, sizeof(note), "经济差走势（上方为 %s 领先）", s.detail.team_a);
        text = note;
    }
    lv_obj_t *nl = ui_label_create(s.page.content, text, ui_font_hint, ui_c_dim());
    lv_obj_set_width(nl, LV_PCT(100));
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);

    render_gold_curve(d);
}

// 选手页：每人一行"选手名"与"K/D/A 金币"。
static void render_detail_players(const app_esport_detail_t *d)
{
    bool have = false;
    for (int i = 0; i < APP_ESPORT_TEAM_PLAYERS; i++) {
        const app_esport_player_t *p = &d->team_a[i];
        const app_esport_player_t *q = &d->team_b[i];
        if (p->gold || p->kills || p->deaths || p->assists ||
            q->gold || q->kills || q->deaths || q->assists) {
            have = true;
            break;
        }
    }
    if (!have) {
        ui_empty_create(s.page.content, "暂无选手数据",
                        "接口只对进行中的对局返回实时数据");
        return;
    }

    const char *const names[2] = { s.detail.team_a, s.detail.team_b };
    const app_esport_player_t *sides[2] = { d->team_a, d->team_b };
    for (int t = 0; t < 2; t++) {
        lv_obj_t *hl = ui_label_create(s.page.content, names[t], ui_font_body,
                                       t == 0 ? ui_c_live() : ui_c_soon());
        lv_obj_set_style_pad_left(hl, 4, 0);
        for (int i = 0; i < APP_ESPORT_TEAM_PLAYERS; i++) {
            const app_esport_player_t *p = &sides[t][i];
            char title[32];
            char value[24];
            char gold[16];
            snprintf(title, sizeof(title), "%s %s",
                     ROLE_NAMES[p->role], p->player[0] ? p->player : "—");
            fmt_gold(p->gold, gold, sizeof(gold));
            snprintf(value, sizeof(value), "%d/%d/%d %s",
                     p->kills, p->deaths, p->assists, gold);
            ui_row_create(s.page.content, title, value);
        }
    }
}

// 单场详情二级视图：比分卡常显，三个子页按需展示阵容/经济/选手。
static void render_detail(void)
{
    const app_esport_match_t *m = &s.detail;
    const app_esport_detail_t *d = current_detail();

    char title[40];
    char right[24];
    snprintf(title, sizeof(title), "%s vs %s", m->team_a, m->team_b);
    int game_no = (d && d->game_number > 0) ? d->game_number
                                           : m->score_a + m->score_b + 1;
    snprintf(right, sizeof(right), "第%d局", game_no);
    ui_header_create(s.page.content, title, right, NULL, NULL);

    static const char *const SUB_NAMES[DETAIL_TAB_COUNT] = { "阵容", "经济", "选手" };
    s.tabs = ui_tabs_create(s.page.content, SUB_NAMES, DETAIL_TAB_COUNT);
    ui_tabs_select(s.tabs, s.detail_tab);

    // 基础比分卡：展示级数字仅含 ASCII，符合字体约束。详情拿不到时它就是全部信息。
    lv_obj_t *card = ui_card_create(s.page.content, 0, 0, UI_W - 2 * UI_MARGIN_X, 66,
                                    match_state_color(m, (int)app_state_now_unix()));
    char score[32];
    snprintf(score, sizeof(score), "%d : %d", m->score_a, m->score_b);
    lv_obj_t *sl = ui_label_create(card, score, ui_font_display_s, ui_c_text());
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, -8);
    lv_obj_t *nl = ui_label_create(card, title, ui_font_hint, ui_c_dim());
    lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, -6);

    if (!d) {
        // 未拿到详情：按拉取状态说明原因，而不是空白或假数据。
        char msg[80];
        const char *note = "对局详情尚未加载";
        uint32_t col = ui_c_dim();
        app_fetch_state_t st = app_net_esport_detail_state();
        if (st == APP_FETCH_RUNNING) {
            note = "正在加载对局详情…";
            col = ui_c_accent();
        } else if (st == APP_FETCH_FAILED) {
            const char *e = app_net_esport_detail_error();
            snprintf(msg, sizeof(msg), "详情加载失败：%s", e ? e : "网络错误");
            note = msg;
            col = ui_c_warn();
        }

        lv_obj_t *note_lbl = ui_label_create(s.page.content, note, ui_font_hint, col);
        lv_obj_set_width(note_lbl, LV_PCT(100));
        lv_obj_set_style_text_align(note_lbl, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *tip = ui_label_create(s.page.content, "退出后重新进入可重试",
                                        ui_font_hint, ui_c_dim());
        lv_obj_set_width(tip, LV_PCT(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    switch (s.detail_tab) {
    case DETAIL_GOLD:    render_detail_gold(d);    break;
    case DETAIL_PLAYERS: render_detail_players(d); break;
    case DETAIL_LINEUP:
    default:             render_detail_lineup(d);  break;
    }
}

// 整体重绘当前视图（根列表或单场详情）。
static void render(void)
{
    if (!s.page.scr) return;
    content_clear();

    if (s.in_detail) {
        render_detail();
        ui_page_set_hint(HINT_DETAIL);
        return;
    }

    static const char *const TAB_NAMES[TAB_COUNT] = { "赛程", "积分榜", "战队" };
    s.tabs = ui_tabs_create(s.page.content, TAB_NAMES, TAB_COUNT);
    ui_tabs_select(s.tabs, s.tab);

    switch (s.tab) {
    case TAB_STANDINGS:
        render_standings();
        ui_page_set_hint(HINT_STANDINGS);
        break;
    case TAB_TEAMS:
        render_teams();
        ui_page_set_hint(HINT_TEAMS);
        break;
    case TAB_SCHEDULE:
    default:
        render_schedule();
        ui_page_set_hint(HINT_SCHEDULE);
        break;
    }
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------
// 进入标签页时的按需请求：积分榜需要赛区列表与当前赛区积分榜。
static void on_tab_entered(void)
{
    if (s.tab == TAB_STANDINGS) {
        if (app_net_league_count() == 0) app_net_leagues_fetch();
        app_net_standings_fetch(NULL);
    }
}

// 切换主标签页并重绘。索引自动环绕。
static void switch_tab(int index)
{
    index = ((index % TAB_COUNT) + TAB_COUNT) % TAB_COUNT;
    if (index == s.tab) return;
    s.tab = index;
    s.selected = 0;
    on_tab_entered();
    render();
}

// 手动刷新赛程：拉起一次后台拉取并即时给出反馈。
static void manual_refresh(void)
{
    app_net_esports_fetch();
    banner_update();
    ui_hint_flash("正在刷新…", 1200);
}

// 切换关注状态。达到上限时明确提示，不静默失败。
static void toggle_follow(void)
{
    if (s.selected < 0 || s.selected >= s.team_row_count) return;
    app_esport_cache_t *c = app_state_esports();
    const char *name = s.team_rows[s.selected].name;
    bool on = !app_esport_is_followed(c, name);

    if (on && c->follow_count >= APP_ESPORT_MAX_FOLLOWS) {
        char msg[40];
        snprintf(msg, sizeof(msg), "最多关注 %d 支，请先取消一支",
                 APP_ESPORT_MAX_FOLLOWS);
        ui_hint_flash(msg, 2000);
        return;
    }
    if (!app_esport_follow_set(c, name, on)) {
        ui_hint_flash("操作失败，请重试", 1500);
        return;
    }
    // 关注变化会影响赛程排序与标记，刷新并持久化。
    app_esport_apply_follows(c);
    app_state_save_esports();
    render();
    ui_hint_flash(on ? "已关注" : "已取消关注", 1200);
}

// 短按 OK 的主操作：赛程进入详情、战队切换关注、积分榜换赛区。
// 积分榜原来把 ↑↓ 用来切赛区，而一个赛区最多 32 支战队、屏幕只放得下约 5 行，
// 结果第 6 名之后既看不到也滚不到。三键设备上长按 ↑↓ 已被"切页"占用，所以把
// 换赛区挪到本来空着的短按 OK，↑↓ 让给滚动。
static void activate(void)
{
    if (s.tab == TAB_SCHEDULE) {
        app_esport_cache_t *c = app_state_esports();
        if (!c->valid || s.row_count <= 0) return;
        if (s.selected < 0 || s.selected >= c->match_count) return;
        s.detail = c->matches[s.selected];
        s.detail_tab = DETAIL_LINEUP;
        s.in_detail = true;
        // 进入即请求详情：先请求再渲染，界面立刻显示"正在加载"而不是空状态。
        app_net_esport_detail_fetch(s.detail.id);
        render();
        s.detail_sig_state = (int)app_net_esport_detail_state();
        s.detail_sig_fetched = c->detail.fetched_utc;
    } else if (s.tab == TAB_TEAMS) {
        toggle_follow();
    } else if (s.tab == TAB_STANDINGS) {
        int n = app_net_league_count();
        if (n <= 0) return;
        s.league_idx = (s.league_idx + 1) % n;
        const char *slug = app_net_league_slug(s.league_idx);
        app_net_standings_fetch(slug);
        render();
    }
}

// 把根列表内容上下滚一格（约一行高）。列表比屏幕长时靠它翻看；不溢出时滚动量
// 被 LVGL 夹到 0，不会有副作用。
static void scroll_rows(int direction)
{
    if (!s.page.content) return;
    lv_obj_scroll_by(s.page.content, 0, (direction > 0) ? -40 : 40, LV_ANIM_OFF);
}

// 根列表下短按 UP/DOWN：赛程/战队移动选中行，积分榜滚动列表。
static void move_selection(int delta)
{
    if (s.tab == TAB_STANDINGS) {
        scroll_rows(delta);
        return;
    }
    if (s.row_count <= 0) return;
    s.selected += delta;
    if (s.selected < 0) s.selected = 0;
    if (s.selected >= s.row_count) s.selected = s.row_count - 1;
    apply_selection();
}

// ---------------------------------------------------------------------------
// 模块回调
// ---------------------------------------------------------------------------
void page_esports_enter(void)
{
    memset(&s, 0, sizeof(s));
    s.tab = TAB_SCHEDULE;
    s.page = ui_page_create(HINT_SCHEDULE);

    // 缓存优先：先渲染本地缓存，再后台刷新，避免弱网空屏。
    render();

    app_esport_cache_t *c = app_state_esports();
    s.sig_fetched = c->fetched_utc;
    s.sig_count = c->match_count;
    s.sig_teams = c->team_count;
    s.sig_leagues = app_net_league_count();
    s.sig_valid = c->valid;
    s.sig_state = (int)app_net_esports_state();

    app_net_esports_fetch();
}

void page_esports_exit(void)
{
    // 退出即释放 Wi-Fi，符合"退出页面不得继续占用 Wi-Fi"的约束。
    app_net_esports_stop();
    if (s.page.scr) lv_obj_delete(s.page.scr);
    memset(&s, 0, sizeof(s));
}

void page_esports_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s.page.scr) return;

    if (s.in_detail) {
        // 详情视图：长按 UP/DOWN 切子页，长按 OK 返回赛程列表。
        // 短按 UP/DOWN 用来滚动正文——阵容与选手子页各有 10 行选手，比一屏高得多，
        // 原先短按被直接丢掉，用户既看不到 B 队也滚不动，只觉得"按键没反应"。
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            s.in_detail = false;
            render();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
            s.detail_tab = (s.detail_tab + DETAIL_TAB_COUNT - 1) % DETAIL_TAB_COUNT;
            render();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
            s.detail_tab = (s.detail_tab + 1) % DETAIL_TAB_COUNT;
            render();
        } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            scroll_rows(btn == BSP_BTN_DOWN ? 1 : -1);
        }
        return;
    }

    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)        move_selection(-1);
        else if (btn == BSP_BTN_DOWN) move_selection(1);
        else if (btn == BSP_BTN_OK)   activate();
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) {
            // 根视图长按 OK 返回主页。
            ui_app_go_home();
        } else if (btn == BSP_BTN_UP) {
            // 赛程页长按 UP 刷新；其它页长按 UP 切到上一标签页。
            if (s.tab == TAB_SCHEDULE) manual_refresh();
            else switch_tab(s.tab - 1);
        } else if (btn == BSP_BTN_DOWN) {
            switch_tab(s.tab + 1);
        }
    }
}

void page_esports_tick(void)
{
    if (!s.page.scr) return;

    app_esport_cache_t *c = app_state_esports();
    app_fetch_state_t st = app_net_esports_state();

    // 详情视图：只在详情状态或数据更新时重绘，避免每秒重建整页。
    if (s.in_detail) {
        int dstate = (int)app_net_esport_detail_state();
        int dfetched = c->detail.fetched_utc;
        if (dstate != s.detail_sig_state || dfetched != s.detail_sig_fetched) {
            s.detail_sig_state = dstate;
            s.detail_sig_fetched = dfetched;
            render();
        }
        return;
    }

    // 每秒刷新"更新于"与刷新状态文案。
    if (s.tab == TAB_SCHEDULE) banner_update();

    // 缓存或拉取状态变化时重绘当前列表（拉取完成、比分更新、积分榜返回等）。
    bool changed = (c->fetched_utc != s.sig_fetched) ||
                   (c->match_count != s.sig_count) ||
                   (c->team_count != s.sig_teams) ||
                   (app_net_league_count() != s.sig_leagues) ||
                   (c->valid != s.sig_valid) ||
                   ((int)st != s.sig_state);
    if (changed) {
        s.sig_fetched = c->fetched_utc;
        s.sig_count = c->match_count;
        s.sig_teams = c->team_count;
        s.sig_leagues = app_net_league_count();
        s.sig_valid = c->valid;
        s.sig_state = (int)st;
        render();
    }
}
