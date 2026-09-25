// main/logic/app_esports.h —— 赛事数据模型、优先级排序与关注战队（与硬件无关）。
//
// 本层不依赖 ESP-IDF 与 LVGL，可在主机上直接编译测试。网络层把拉取到的 JSON
// 填进缓存，界面层依据优先级决定主页赛事卡展示哪一场。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_ESPORT_MAX_MATCHES  48
#define APP_ESPORT_MAX_TEAMS    32
#define APP_ESPORT_MAX_FOLLOWS  5
#define APP_ESPORT_TEAM_PLAYERS 5   // 每队选手数（上单/打野/中单/下路/辅助）

#define APP_ESPORT_ROLE_LEN     8   // 中文位置名（"上单" 为 6 字节 + NUL）
#define APP_ESPORT_CHAMP_LEN    20  // 英雄标识（接口原文，最长如 "MonkeyKing"）
#define APP_ESPORT_PLAYER_LEN   20  // 选手名（接口原文）

typedef enum {
    APP_MATCH_UNKNOWN = 0,
    APP_MATCH_UPCOMING,
    APP_MATCH_LIVE,
    APP_MATCH_FINISHED,
} app_match_state_t;

typedef struct {
    char id[32];
    char league[24];      // 赛区/联赛，如 "LPL"
    char block[24];       // 赛事名或阶段
    char team_a[16];
    char team_b[16];
    int  score_a;
    int  score_b;
    int  start_utc;       // 开赛 Unix 秒（UTC）
    app_match_state_t state;
    bool follow_a;
    bool follow_b;
} app_esport_match_t;

typedef struct {
    char name[16];
    int  win;
    int  loss;
    int  points;
} app_esport_team_t;

typedef struct {
    app_esport_match_t matches[APP_ESPORT_MAX_MATCHES];
    int  match_count;
    app_esport_team_t   teams[APP_ESPORT_MAX_TEAMS];
    int  team_count;
    char standings_league[24];
    char followed[APP_ESPORT_MAX_FOLLOWS][16];
    int  follow_count;
    int  fetched_utc;     // 上次成功拉取的时刻（UTC 秒）
    bool valid;           // 是否有可用缓存
} app_esport_cache_t;

// 越小越靠前。now_utc 为当前 UTC 秒。
int  app_esport_priority(const app_esport_match_t *m, int now_utc);
void app_esport_sort(app_esport_match_t *matches, int count, int now_utc);
// 主页赛事卡应展示的比赛下标；无合适比赛返回 -1。
int  app_esport_home_pick(const app_esport_match_t *matches, int count, int now_utc);
bool app_esport_follow_set(app_esport_cache_t *c, const char *team, bool on);
bool app_esport_is_followed(const app_esport_cache_t *c, const char *team);
void app_esport_apply_follows(app_esport_cache_t *c);   // 依据 followed[] 刷新每场比赛的 follow_a/follow_b
const char *app_match_state_name(app_match_state_t state);
app_match_state_t app_esport_state_from_text(const char *text);
