// main/ui/ui_pet.c —— 桌宠火柴人的线段骨架（详见 ui_pet.h）。
#include "ui_pet.h"

#include "ui_app.h"
#include "ui_theme.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "ui_pet";

// 绘制装置的数量上限：主页一张桌宠卡 + 任意页面一个状态栏剪影，页面切换时旧的先销毁、
// 新的才创建，因此同时存活最多两个；留一倍余量，避免将来多一处复用时被静默丢弃。
#define UI_PET_RIG_MAX   4

// 动画节拍。动作库里最快的过渡只有 110~160ms（抖动、抬腿、鼓掌），采样周期必须明显短于
// 它们才不会把整个关键帧段整段跳过去——8 帧/秒（125ms）时 110ms 的段一拍都采不到，动作
// 会变成"跳一下"。这里取 50ms（20 帧/秒），最快的那几段也能落到 2~3 个中间帧上。
// 代价只在 CPU 与 SPI 带宽：重绘只失效火柴人那一小块（主页 70×70、状态栏 24×24），
// 不是整屏重刷。息屏时回调直接返回，不产生任何重绘。
#define UI_PET_TICK_MS   50

// 骨架折线：右臂、左臂、躯干+左腿、右腿。把同一分支上的关节连成一条折线，比每根骨头
// 一个控件少了近一半对象。
enum { BONE_ARM_R, BONE_ARM_L, BONE_TRUNK_LEG_L, BONE_LEG_R, BONE_COUNT };
#define BONE_MAX_PTS 4

struct ui_pet_rig {
    bool      used;
    lv_obj_t *root;
    lv_obj_t *bones[BONE_COUNT];
    lv_obj_t *head;
    lv_obj_t *speech;
    lv_point_precise_t pts[BONE_COUNT][BONE_MAX_PTS];
    int        fig;        // 火柴人方块边长（等于 root 高度）
    int        line_w;
    uint32_t   color;      // 当前线色，避免每拍都重设样式
    const char *last_speech;
};

static struct ui_pet_rig s_rigs[UI_PET_RIG_MAX];
static app_pet_t   s_pet;
static bool        s_pet_ready;
static lv_timer_t *s_timer;

uint32_t ui_pet_mood_color(app_pet_mood_t mood)
{
    switch (mood) {
    case APP_PET_MOOD_HAPPY:     return ui_c_ok();
    case APP_PET_MOOD_FOCUS:     return ui_c_accent();
    case APP_PET_MOOD_TIRED:     return ui_c_warn();
    case APP_PET_MOOD_ANGRY:     return ui_c_live();
    case APP_PET_MOOD_SURPRISED: return ui_c_soon();
    case APP_PET_MOOD_CALM:
    default:                     return ui_c_text();
    }
}

// 装置随 root 销毁而失效。这里只清槽位，不释放任何内存——装置本身就在静态池里，
// 没有堆分配，也就不存在忘记释放的问题。
static void rig_deleted_cb(lv_event_t *e)
{
    ui_pet_rig_t *r = (ui_pet_rig_t *)lv_event_get_user_data(e);
    if (!r) return;
    r->used = false;
    r->root = NULL;
    r->head = NULL;
    r->speech = NULL;
    for (int i = 0; i < BONE_COUNT; i++) r->bones[i] = NULL;
}

// 把骨架落点写进装置并重绘。坐标全部落在 [0, fig] 内，不裁剪也不夹取。
static void rig_apply(ui_pet_rig_t *r, uint64_t now)
{
    app_pet_pose_t pose;
    app_pet_skeleton_t sk;
    app_pet_pose_at(&s_pet, now, &pose);
    app_pet_skeleton(&pose, r->fig, r->fig, &sk);

    // 右臂：肩 → 肘 → 手
    r->pts[BONE_ARM_R][0] = (lv_point_precise_t){ sk.shoulder_x, sk.shoulder_y };
    r->pts[BONE_ARM_R][1] = (lv_point_precise_t){ sk.elbow_r_x, sk.elbow_r_y };
    r->pts[BONE_ARM_R][2] = (lv_point_precise_t){ sk.hand_r_x, sk.hand_r_y };
    // 左臂
    r->pts[BONE_ARM_L][0] = (lv_point_precise_t){ sk.shoulder_x, sk.shoulder_y };
    r->pts[BONE_ARM_L][1] = (lv_point_precise_t){ sk.elbow_l_x, sk.elbow_l_y };
    r->pts[BONE_ARM_L][2] = (lv_point_precise_t){ sk.hand_l_x, sk.hand_l_y };
    // 躯干 + 左腿：肩 → 髋 → 膝 → 脚。躯干与左腿共用髋部这一点，因此能连成一条折线。
    r->pts[BONE_TRUNK_LEG_L][0] = (lv_point_precise_t){ sk.shoulder_x, sk.shoulder_y };
    r->pts[BONE_TRUNK_LEG_L][1] = (lv_point_precise_t){ sk.hip_x, sk.hip_y };
    r->pts[BONE_TRUNK_LEG_L][2] = (lv_point_precise_t){ sk.knee_l_x, sk.knee_l_y };
    r->pts[BONE_TRUNK_LEG_L][3] = (lv_point_precise_t){ sk.foot_l_x, sk.foot_l_y };
    // 右腿
    r->pts[BONE_LEG_R][0] = (lv_point_precise_t){ sk.hip_x, sk.hip_y };
    r->pts[BONE_LEG_R][1] = (lv_point_precise_t){ sk.knee_r_x, sk.knee_r_y };
    r->pts[BONE_LEG_R][2] = (lv_point_precise_t){ sk.foot_r_x, sk.foot_r_y };

    if (r->root) {
        lv_line_set_points(r->bones[BONE_ARM_R], r->pts[BONE_ARM_R], 3);
        lv_line_set_points(r->bones[BONE_ARM_L], r->pts[BONE_ARM_L], 3);
        lv_line_set_points(r->bones[BONE_TRUNK_LEG_L], r->pts[BONE_TRUNK_LEG_L], 4);
        lv_line_set_points(r->bones[BONE_LEG_R], r->pts[BONE_LEG_R], 3);

        int d = sk.head_r * 2;
        // 状态栏剪影只有 20 多像素高，按 8% 算出来的头直径不到 2px，会退化成一个看不清
        // 的点——看起来就像"没有头"。这里给头一个与线宽挂钩的最小直径，保证小尺寸下
        // 依然能认出是个人形。
        int dmin = r->line_w * 2 + 2;
        if (d < dmin) d = dmin;
        lv_obj_set_size(r->head, d, d);
        lv_obj_set_pos(r->head, sk.head_x - d / 2, sk.head_y - d / 2);
    }

    // 线色跟情绪走：状态栏剪影太小，看不出表情，颜色是它唯一能表达情绪的通道。
    uint32_t col = ui_pet_mood_color(app_pet_mood(&s_pet));
    if (col != r->color) {
        r->color = col;
        if (r->root) {
            for (int i = 0; i < BONE_COUNT; i++) {
                lv_obj_set_style_line_color(r->bones[i], lv_color_hex(col), 0);
            }
            lv_obj_set_style_border_color(r->head, lv_color_hex(col), 0);
        }
    }

    if (r->speech) {
        const char *sp = app_pet_speech(&s_pet, now);
        if (sp != r->last_speech) {
            lv_label_set_text(r->speech, sp ? sp : "");
            r->last_speech = sp;
        }
    }
}

static void pet_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    // 息屏时背光已灭，重绘看不见却要占用 CPU 与 SPI 带宽。这里直接返回，桌宠状态由
    // 下一次唤醒后的第一拍重新对齐（app_pet_tick 会把过期动作收回站立）。
    if (ui_app_is_asleep()) return;

    uint64_t now = (uint64_t)lv_tick_get();
    app_pet_tick(&s_pet, now);
    for (int i = 0; i < UI_PET_RIG_MAX; i++) {
        if (s_rigs[i].used) rig_apply(&s_rigs[i], now);
    }
}

void ui_pet_refresh(void)
{
    uint64_t now = (uint64_t)lv_tick_get();
    for (int i = 0; i < UI_PET_RIG_MAX; i++) {
        if (s_rigs[i].used) rig_apply(&s_rigs[i], now);
    }
}

ui_pet_rig_t *ui_pet_rig_create(lv_obj_t *parent, int x, int y, int w, int h,
                                const lv_font_t *speech_font, uint32_t speech_color)
{
    if (!parent || w <= 0 || h <= 0) return NULL;

    if (!s_pet_ready) {
        // 种子取上电以来的毫秒数：同一次开机内完全确定、便于复现，每次开机又不一样，
        // 免得每次上电都踩着同一套动作序列。
        app_pet_init(&s_pet, (uint32_t)lv_tick_get() ^ 0x9E3779B9u);
        s_pet_ready = true;
    }

    int slot = -1;
    for (int i = 0; i < UI_PET_RIG_MAX; i++) {
        if (!s_rigs[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "桌宠装置已满，跳过这次创建");
        return NULL;
    }

    ui_pet_rig_t *r = &s_rigs[slot];
    memset(r, 0, sizeof(*r));
    r->fig = h;
    r->line_w = (h >= 44) ? 2 : 1;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_size(root, w, h);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    r->root = root;
    lv_obj_add_event_cb(root, rig_deleted_cb, LV_EVENT_DELETE, r);

    for (int i = 0; i < BONE_COUNT; i++) {
        lv_obj_t *ln = lv_line_create(root);
        lv_obj_set_style_line_width(ln, r->line_w, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        r->bones[i] = ln;
    }

    lv_obj_t *head = lv_obj_create(root);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(head, r->line_w, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    r->head = head;

    if (speech_font) {
        lv_obj_t *sp = lv_label_create(root);
        lv_obj_set_style_text_font(sp, speech_font, 0);
        lv_obj_set_style_text_color(sp, lv_color_hex(speech_color), 0);
        lv_obj_set_width(sp, w - h - 6);
        lv_label_set_long_mode(sp, LV_LABEL_LONG_WRAP);
        lv_obj_align(sp, LV_ALIGN_LEFT_MID, h + 6, 0);
        r->speech = sp;
    }

    r->used = true;
    // 定时器只建一次、常驻：状态栏剪影在每一页都存在，本来就需要它一直跑。
    if (!s_timer) s_timer = lv_timer_create(pet_timer_cb, UI_PET_TICK_MS, NULL);

    rig_apply(r, (uint64_t)lv_tick_get());
    return r;
}

// ---------------------------------------------------------------------------
// 全局事件
// ---------------------------------------------------------------------------

void ui_pet_event(app_pet_event_t ev)
{
    if (!s_pet_ready) {
        // 事件可能早于第一个装置（例如开机就把番茄钟喂进来）。先把状态建起来，
        // 免得这次事件被丢掉。
        app_pet_init(&s_pet, (uint32_t)lv_tick_get() ^ 0x9E3779B9u);
        s_pet_ready = true;
    }
    app_pet_trigger(&s_pet, ev, (uint64_t)lv_tick_get());
    ui_pet_refresh();
}

void ui_pet_set_mischief(bool on)
{
    if (!s_pet_ready) {
        app_pet_init(&s_pet, (uint32_t)lv_tick_get() ^ 0x9E3779B9u);
        s_pet_ready = true;
    }
    app_pet_set_mischief(&s_pet, on);
}

bool ui_pet_mischief(void)
{
    return s_pet_ready && app_pet_mischief(&s_pet);
}