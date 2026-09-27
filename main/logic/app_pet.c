// main/logic/app_pet.c —— 火柴人桌宠的动作库与状态机（详见 app_pet.h）。
#include "app_pet.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 关键帧与动作表
// ---------------------------------------------------------------------------
// 每个动作是一串关键帧，帧间做整数线性插值。角度单位是度：0 朝下、90 朝右、±180 朝上；
// dx/dy 是髋部相对画布中心的偏移，单位是"身高百分比"（正数向右/向下）。
// 全部用指定初始化器书写，只写需要偏离"站立"的关节，一眼能看出这个动作在动哪里。
//
// 站姿（所有动作的共同基准）：双臂自然下垂并微微外张，双腿并拢微开。
#define NEUTRAL                                                             \
    .a_l_up = -12, .a_l_fore = -8, .a_r_up = 12, .a_r_fore = 8,             \
    .l_thigh = -5, .r_thigh = 5

typedef struct {
    int16_t  torso;
    int16_t  a_l_up, a_l_fore;
    int16_t  a_r_up, a_r_fore;
    int16_t  l_thigh, l_shin;
    int16_t  r_thigh, r_shin;
    int16_t  dx, dy;
    uint16_t dur_ms;   // 从本帧过渡到下一帧（循环动作则是回到首帧）的时长
} app_pet_key_t;

typedef struct {
    const char          *name;
    const app_pet_key_t *keys;
    int                  key_count;
    bool                 loop;
} app_pet_action_t;

// 关键帧一律用"先铺 NEUTRAL 站姿、再覆盖需要变化的关节"的写法，这样每一帧只写出这个
// 动作真正在动的部位，读完一帧就知道它在动哪里。代价是同一字段会被写两次，而 GCC 的
// -Wextra 会把这种刻意的"基座 + 覆盖"当成重复初始化来告警。这里局部关掉该告警而不是
// 改写成 11 个字段全写满——后者在几十帧的规模下反而更容易看漏、写错。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#endif

// 站立呼吸：幅度极小，作用只是让画面"活着"，不至于看起来卡住。
static const app_pet_key_t k_idle[] = {
    { NEUTRAL, .dur_ms = 1200 },
    { NEUTRAL, .a_l_up = -15, .a_r_up = 15, .dy = -2, .dur_ms = 1200 },
    { NEUTRAL, .dur_ms = 1200 },
};

// 左右张望。
static const app_pet_key_t k_look[] = {
    { NEUTRAL, .dur_ms = 380 },
    { NEUTRAL, .torso = 8, .a_l_up = -22, .a_r_up = 26, .dur_ms = 480 },
    { NEUTRAL, .torso = -8, .a_l_up = -26, .a_r_up = 22, .dur_ms = 480 },
    { NEUTRAL, .dur_ms = 380 },
    { NEUTRAL, .dur_ms = 560 },
};

// 挥手：右臂举到侧上方，前臂来回摆。
static const app_pet_key_t k_wave[] = {
    { NEUTRAL, .dur_ms = 280 },
    { NEUTRAL, .torso = -4, .a_r_up = 140, .a_r_fore = 168, .dur_ms = 260 },
    { NEUTRAL, .torso = -4, .a_r_up = 140, .a_r_fore = 148, .dur_ms = 220 },
    { NEUTRAL, .torso = -4, .a_r_up = 140, .a_r_fore = 172, .dur_ms = 220 },
    { NEUTRAL, .torso = -4, .a_r_up = 140, .a_r_fore = 148, .dur_ms = 220 },
    { NEUTRAL, .dur_ms = 360 },
};

// 原地跳：下蹲、腾空收腿、落地缓冲。
static const app_pet_key_t k_jump[] = {
    { NEUTRAL, .dur_ms = 160 },
    { NEUTRAL, .dy = 8,  .l_thigh = -18, .l_shin = 22, .r_thigh = 18, .r_shin = -22,
      .a_l_up = -30, .a_r_up = 30, .dur_ms = 150 },
    { NEUTRAL, .dy = -20, .l_thigh = -26, .l_shin = 34, .r_thigh = 26, .r_shin = -34,
      .a_l_up = -120, .a_r_up = 120, .dur_ms = 340 },
    { NEUTRAL, .dy = 10, .l_thigh = -20, .l_shin = 26, .r_thigh = 20, .r_shin = -26,
      .a_l_up = -40, .a_r_up = 40, .dur_ms = 190 },
    { NEUTRAL, .dur_ms = 280 },
};

// 摇摆舞：躯干左右摆、手臂大幅交替，脚下跟着弹。就是那段"跳鸡舞"的抽象版。
static const app_pet_key_t k_dance[] = {
    { NEUTRAL, .torso = 8,  .a_l_up = -95, .a_l_fore = -70, .a_r_up = 55, .a_r_fore = 35,
      .l_thigh = -10, .r_thigh = 10, .dur_ms = 380 },
    { NEUTRAL, .torso = -8, .a_l_up = -55, .a_l_fore = -35, .a_r_up = 95, .a_r_fore = 70,
      .l_thigh = -14, .r_thigh = 14, .dy = -4, .dur_ms = 380 },
    { NEUTRAL, .torso = 8,  .a_l_up = -95, .a_l_fore = -70, .a_r_up = 55, .a_r_fore = 35,
      .l_thigh = -10, .r_thigh = 10, .dur_ms = 380 },
    { NEUTRAL, .torso = -8, .a_l_up = -55, .a_l_fore = -35, .a_r_up = 95, .a_r_fore = 70,
      .l_thigh = -14, .r_thigh = 14, .dy = -4, .dur_ms = 380 },
};

// 原地踏步：双腿前后交换，手臂反向配合，髋部左右挪。
static const app_pet_key_t k_walk[] = {
    { NEUTRAL, .l_thigh = 24, .l_shin = -6, .r_thigh = -24, .r_shin = -6,
      .a_l_up = -24, .a_r_up = 24, .dx = -3, .dur_ms = 300 },
    { NEUTRAL, .dy = -2, .dur_ms = 300 },
    { NEUTRAL, .l_thigh = -24, .l_shin = -6, .r_thigh = 24, .r_shin = -6,
      .a_l_up = 24, .a_r_up = -24, .dx = 3, .dur_ms = 300 },
    { NEUTRAL, .dy = -2, .dur_ms = 300 },
};

// 坐下：髋部整体下沉、大腿前伸折起，撑住一会儿再站起。下沉量按"折腿后小腿仍能落到
// 地面"选取（约 18% 身高），再多就会把脚推出画布下沿。
static const app_pet_key_t k_sit[] = {
    { NEUTRAL, .dur_ms = 340 },
    { NEUTRAL, .dy = 18, .torso = 4, .l_thigh = 80, .l_shin = 4, .r_thigh = 96, .r_shin = 4,
      .a_l_up = -18, .a_r_up = 18, .dur_ms = 380 },
    { NEUTRAL, .dy = 18, .torso = 4, .l_thigh = 80, .l_shin = 4, .r_thigh = 96, .r_shin = 4,
      .a_l_up = -18, .a_r_up = 18, .dur_ms = 1600 },
    { NEUTRAL, .dur_ms = 380 },
};

// 打盹：躯干前倾、幅度很慢的呼吸，配一个 "Zzz" 台词。
static const app_pet_key_t k_sleep[] = {
    { NEUTRAL, .torso = 8, .a_l_up = -6, .a_r_up = 6, .dy = 3, .dur_ms = 1700 },
    { NEUTRAL, .torso = 13, .a_l_up = -4, .a_r_up = 4, .dy = 7, .dur_ms = 1700 },
    { NEUTRAL, .torso = 8, .a_l_up = -6, .a_r_up = 6, .dy = 3, .dur_ms = 1700 },
    { NEUTRAL, .dur_ms = 460 },
};

// 伸懒腰：双臂举过头顶，躯干微微后仰。
static const app_pet_key_t k_stretch[] = {
    { NEUTRAL, .dur_ms = 280 },
    { NEUTRAL, .torso = -3, .dy = -6, .a_l_up = -165, .a_l_fore = -172,
      .a_r_up = 165, .a_r_fore = 172, .dur_ms = 780 },
    { NEUTRAL, .torso = -3, .dy = -6, .a_l_up = -165, .a_l_fore = -172,
      .a_r_up = 165, .a_r_fore = 172, .dur_ms = 560 },
    { NEUTRAL, .dur_ms = 380 },
};

// 抱胸思考：右手抬到下巴附近，身体轻轻晃。
static const app_pet_key_t k_think[] = {
    { NEUTRAL, .torso = 4, .a_r_up = 62, .a_r_fore = 158, .a_l_up = -16, .a_l_fore = -30,
      .dur_ms = 1500 },
    { NEUTRAL, .torso = 6, .a_r_up = 58, .a_r_fore = 162, .a_l_up = -16, .a_l_fore = -34,
      .dur_ms = 1500 },
};

// 欢呼：双臂高举 + 两下小跳。
static const app_pet_key_t k_cheer[] = {
    { NEUTRAL, .dur_ms = 180 },
    { NEUTRAL, .a_l_up = -165, .a_l_fore = -172, .a_r_up = 165, .a_r_fore = 172,
      .dy = -14, .dur_ms = 200 },
    { NEUTRAL, .a_l_up = -165, .a_l_fore = -172, .a_r_up = 165, .a_r_fore = 172,
      .dy = 6, .dur_ms = 190 },
    { NEUTRAL, .a_l_up = -165, .a_l_fore = -172, .a_r_up = 165, .a_r_fore = 172,
      .dy = -16, .dur_ms = 200 },
    { NEUTRAL, .a_l_up = -165, .a_l_fore = -172, .a_r_up = 165, .a_r_fore = 172,
      .dy = 6, .dur_ms = 190 },
    { NEUTRAL, .dur_ms = 320 },
};

// 捂脸：右手抬到头部，躯干前倾。
static const app_pet_key_t k_facepalm[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .torso = 14, .dy = 3, .a_r_up = 78, .a_r_fore = 205, .dur_ms = 460 },
    { NEUTRAL, .torso = 14, .dy = 3, .a_r_up = 78, .a_r_fore = 205, .dur_ms = 880 },
    { NEUTRAL, .dur_ms = 360 },
};

// dab：一臂斜上、另一臂横在胸前。
static const app_pet_key_t k_dab[] = {
    { NEUTRAL, .dur_ms = 220 },
    { NEUTRAL, .torso = 10, .a_r_up = 130, .a_r_fore = 160, .a_l_up = 55, .a_l_fore = 200,
      .dur_ms = 360 },
    { NEUTRAL, .torso = 10, .a_r_up = 130, .a_r_fore = 160, .a_l_up = 55, .a_l_fore = 200,
      .dur_ms = 560 },
    { NEUTRAL, .dur_ms = 300 },
};

// 踢腿：一条腿侧踢出去，上身反向压。
static const app_pet_key_t k_kick[] = {
    { NEUTRAL, .dur_ms = 220 },
    { NEUTRAL, .torso = -8, .l_thigh = -70, .l_shin = -20, .a_l_up = -60, .a_l_fore = -80,
      .a_r_up = 40, .dur_ms = 320 },
    { NEUTRAL, .torso = -8, .l_thigh = -70, .l_shin = -20, .a_l_up = -60, .a_l_fore = -80,
      .a_r_up = 40, .dur_ms = 360 },
    { NEUTRAL, .dur_ms = 340 },
};

// 发抖：小幅度快速抖动。
static const app_pet_key_t k_shake[] = {
    { NEUTRAL, .torso = 3, .a_l_up = -20, .a_l_fore = -14, .a_r_up = 20, .a_r_fore = 14,
      .dy = 2, .dur_ms = 110 },
    { NEUTRAL, .torso = -3, .a_l_up = -8, .a_l_fore = -4, .a_r_up = 8, .a_r_fore = 4,
      .dy = -2, .dur_ms = 110 },
};

#define KEYS(arr) arr, (int)(sizeof(arr) / sizeof((arr)[0]))

static const app_pet_action_t ACTIONS[APP_PET_ACT_COUNT] = {
    [APP_PET_ACT_IDLE]     = { "idle",     KEYS(k_idle),     true  },
    [APP_PET_ACT_LOOK]     = { "look",     KEYS(k_look),     false },
    [APP_PET_ACT_WAVE]     = { "wave",     KEYS(k_wave),     false },
    [APP_PET_ACT_JUMP]     = { "jump",     KEYS(k_jump),     false },
    [APP_PET_ACT_DANCE]    = { "dance",    KEYS(k_dance),    true  },
    [APP_PET_ACT_WALK]     = { "walk",     KEYS(k_walk),     true  },
    [APP_PET_ACT_SIT]      = { "sit",      KEYS(k_sit),      false },
    [APP_PET_ACT_SLEEP]    = { "sleep",    KEYS(k_sleep),    false },
    [APP_PET_ACT_STRETCH]  = { "stretch",  KEYS(k_stretch),  false },
    [APP_PET_ACT_THINK]    = { "think",    KEYS(k_think),    true  },
    [APP_PET_ACT_CHEER]    = { "cheer",    KEYS(k_cheer),    false },
    [APP_PET_ACT_FACEPALM] = { "facepalm", KEYS(k_facepalm), false },
    [APP_PET_ACT_DAB]      = { "dab",      KEYS(k_dab),      false },
    [APP_PET_ACT_KICK]     = { "kick",     KEYS(k_kick),     false },
    [APP_PET_ACT_SHAKE]    = { "shake",    KEYS(k_shake),    true  },
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static const app_pet_action_t *action_of(int id)
{
    if (id < 0 || id >= APP_PET_ACT_COUNT) id = APP_PET_ACT_IDLE;
    return &ACTIONS[id];
}

static uint64_t action_total_ms(int id)
{
    const app_pet_action_t *a = action_of(id);
    uint64_t total = 0;
    for (int i = 0; i < a->key_count; i++) total += a->keys[i].dur_ms;
    return total ? total : 1;
}

uint32_t app_pet_rand(app_pet_t *p)
{
    if (!p) return 0;
    uint32_t x = p->rng ? p->rng : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p->rng = x;
    return x;
}

// [lo, hi] 闭区间随机。rng 为 0 时退回 lo，绝不产生除零或越界。
static uint32_t rand_range(app_pet_t *p, uint32_t lo, uint32_t hi)
{
    if (hi <= lo) return lo;
    return lo + app_pet_rand(p) % (hi - lo + 1);
}

// 定点正弦：Bhaskara I 近似，最大误差约 0.0016，足够把角度转成像素方向。
// 返回 sin(deg) * 65536，避免了浮点（ESP32-C3 没有硬件浮点单元）。
int32_t app_pet_sin_q16(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;

    int sign = 1;
    if (deg > 180) {
        deg -= 180;
        sign = -1;
    }
    // deg ∈ [0,180]：sin(x) ≈ 4x(180-x) / (40500 - x(180-x))
    int32_t p = (int32_t)deg * (180 - deg);
    int32_t den = 40500 - p;
    int64_t s = ((int64_t)4 * p * 65536) / den;
    return (int32_t)(sign * s);
}

// ---------------------------------------------------------------------------
// 动作切换
// ---------------------------------------------------------------------------

// window_ms > 0 时表示"这个动作只保持这么久就回站立"（循环动作必须这样做，否则永远
// 停不下来）；为 0 时用动作自身时长。站立本身是无限保持。
static void start_action(app_pet_t *p, int id, uint64_t now_ms, uint64_t window_ms)
{
    p->action = id;
    p->action_start_ms = now_ms;
    if (id == APP_PET_ACT_IDLE) {
        p->action_dur_ms = UINT64_MAX;
    } else if (window_ms) {
        p->action_dur_ms = window_ms;
    } else {
        p->action_dur_ms = action_total_ms(id);
    }
}

// 动作结束回到站立，并安排下一次自发动作。
static void back_to_idle(app_pet_t *p, uint64_t now_ms)
{
    start_action(p, APP_PET_ACT_IDLE, now_ms, 0);
    p->next_idle_ms = now_ms + rand_range(p, 2600, 7200);
}

// 情绪决定空闲时更愿意做什么：疲惫就多坐多睡，开心就多挥手多跳。
static int pick_idle_action(app_pet_t *p)
{
    static const int calm[]      = { APP_PET_ACT_LOOK, APP_PET_ACT_WAVE, APP_PET_ACT_WALK,
                                     APP_PET_ACT_STRETCH, APP_PET_ACT_JUMP, APP_PET_ACT_DANCE,
                                     APP_PET_ACT_THINK, APP_PET_ACT_SIT };
    static const int happy[]     = { APP_PET_ACT_WAVE, APP_PET_ACT_JUMP, APP_PET_ACT_DANCE,
                                     APP_PET_ACT_CHEER, APP_PET_ACT_LOOK };
    static const int focus[]     = { APP_PET_ACT_THINK, APP_PET_ACT_LOOK, APP_PET_ACT_SIT };
    static const int tired[]     = { APP_PET_ACT_SIT, APP_PET_ACT_SLEEP, APP_PET_ACT_STRETCH,
                                     APP_PET_ACT_LOOK };
    static const int angry[]     = { APP_PET_ACT_FACEPALM, APP_PET_ACT_KICK, APP_PET_ACT_SHAKE };
    static const int surprised[] = { APP_PET_ACT_LOOK, APP_PET_ACT_JUMP, APP_PET_ACT_STRETCH };

    const int *pool = calm;
    int n = (int)(sizeof(calm) / sizeof(calm[0]));
    switch (p->mood) {
    case APP_PET_MOOD_HAPPY:     pool = happy;     n = (int)(sizeof(happy) / sizeof(happy[0]));     break;
    case APP_PET_MOOD_FOCUS:     pool = focus;     n = (int)(sizeof(focus) / sizeof(focus[0]));     break;
    case APP_PET_MOOD_TIRED:     pool = tired;     n = (int)(sizeof(tired) / sizeof(tired[0]));     break;
    case APP_PET_MOOD_ANGRY:     pool = angry;     n = (int)(sizeof(angry) / sizeof(angry[0]));     break;
    case APP_PET_MOOD_SURPRISED: pool = surprised; n = (int)(sizeof(surprised) / sizeof(surprised[0])); break;
    case APP_PET_MOOD_CALM:
    default:                     break;
    }
    return pool[app_pet_rand(p) % (uint32_t)n];
}

static void say(app_pet_t *p, const char *text, uint64_t now_ms, uint64_t ms)
{
    p->speech = text;
    p->speech_until_ms = now_ms + ms;
}

static void set_mood(app_pet_t *p, app_pet_mood_t mood, uint64_t now_ms, uint64_t ms)
{
    p->mood = mood;
    p->mood_until_ms = now_ms + ms;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void app_pet_init(app_pet_t *p, uint32_t seed)
{
    if (!p) return;
    uint32_t rng = seed ? seed : 0x9E3779B9u;

    memset(p, 0, sizeof(*p));
    p->rng = rng;
    p->mood = APP_PET_MOOD_CALM;
    p->action = APP_PET_ACT_IDLE;
    p->action_dur_ms = UINT64_MAX;
    // 首次自发动作别太快出现，否则一进主页就手舞足蹈会显得吵。
    p->next_idle_ms = rand_range(p, 2600, 7200);
    p->next_mischief_ms = rand_range(p, 9000, 20000);
}

void app_pet_set_mischief(app_pet_t *p, bool on)
{
    if (!p) return;
    p->mischief = on;
    if (on) p->next_mischief_ms = p->last_tick_ms + rand_range(p, 4000, 9000);
}

bool app_pet_mischief(const app_pet_t *p) { return p ? p->mischief : false; }

int app_pet_action(const app_pet_t *p) { return p ? p->action : APP_PET_ACT_IDLE; }

app_pet_mood_t app_pet_mood(const app_pet_t *p)
{
    return p ? p->mood : APP_PET_MOOD_CALM;
}

const char *app_pet_speech(const app_pet_t *p, uint64_t now_ms)
{
    if (!p || !p->speech) return NULL;
    if (now_ms >= p->speech_until_ms) return NULL;
    return p->speech;
}

const char *app_pet_action_name(int action)
{
    return action_of(action)->name;
}

bool app_pet_action_loops(int action)
{
    return action_of(action)->loop;
}

void app_pet_trigger(app_pet_t *p, app_pet_event_t ev, uint64_t now_ms)
{
    if (!p) return;

    // 事件一律打断当前动作：用户刚做完一件值得反应的事，桌宠却还在原地发呆，会显得
    // 没听见。台词与情绪也都由事件统一设定，界面不参与"该说什么"的判断。
    switch (ev) {
    case APP_PET_EV_FOCUS_ON:
        set_mood(p, APP_PET_MOOD_FOCUS, now_ms, 25u * 60u * 1000u);
        start_action(p, APP_PET_ACT_THINK, now_ms, 6000);
        say(p, "安静专注中", now_ms, 2200);
        break;
    case APP_PET_EV_FOCUS_DONE:
        set_mood(p, APP_PET_MOOD_HAPPY, now_ms, 45u * 1000u);
        start_action(p, APP_PET_ACT_CHEER, now_ms, 0);
        say(p, "(*^_^*)", now_ms, 2600);
        break;
    case APP_PET_EV_BREAK_DONE:
        set_mood(p, APP_PET_MOOD_FOCUS, now_ms, 25u * 60u * 1000u);
        start_action(p, APP_PET_ACT_STRETCH, now_ms, 0);
        say(p, "开工啦", now_ms, 2200);
        break;
    case APP_PET_EV_TIMER_DONE:
        set_mood(p, APP_PET_MOOD_SURPRISED, now_ms, 8000);
        start_action(p, APP_PET_ACT_JUMP, now_ms, 0);
        say(p, "!", now_ms, 2200);
        break;
    case APP_PET_EV_REMINDER:
        set_mood(p, APP_PET_MOOD_SURPRISED, now_ms, 8000);
        start_action(p, APP_PET_ACT_WAVE, now_ms, 0);
        say(p, "该走了", now_ms, 2600);
        break;
    case APP_PET_EV_LOW_BATTERY:
        set_mood(p, APP_PET_MOOD_TIRED, now_ms, 30u * 1000u);
        start_action(p, APP_PET_ACT_SHAKE, now_ms, 3000);
        say(p, ">_<", now_ms, 2600);
        break;
    case APP_PET_EV_FOUND_DEVICE:
        set_mood(p, APP_PET_MOOD_HAPPY, now_ms, 20u * 1000u);
        start_action(p, APP_PET_ACT_CHEER, now_ms, 0);
        say(p, "找到了", now_ms, 2200);
        break;
    case APP_PET_EV_TRACKER:
        set_mood(p, APP_PET_MOOD_SURPRISED, now_ms, 12u * 1000u);
        start_action(p, APP_PET_ACT_JUMP, now_ms, 0);
        say(p, "?", now_ms, 2200);
        break;
    case APP_PET_EV_COUNT:
    default:
        return;
    }

    // 事件动作结束后由 tick 统一安排下一次自发动作，这里只需保证不马上叠加。
    p->next_idle_ms = now_ms + 3000;
}

void app_pet_tick(app_pet_t *p, uint64_t now_ms)
{
    if (!p) return;

    // 时间回绕/重启后倒退：不追算，直接对齐，避免算出天文数字的 elapsed。
    if (now_ms + 1000 < p->last_tick_ms) {
        p->last_tick_ms = now_ms;
        p->action_start_ms = now_ms;
    }
    p->last_tick_ms = now_ms;

    if (p->speech && now_ms >= p->speech_until_ms) p->speech = NULL;
    if (p->mood != APP_PET_MOOD_CALM && now_ms >= p->mood_until_ms) p->mood = APP_PET_MOOD_CALM;

    // 一次性/有窗口的动作到点 → 回站立。
    if (p->action != APP_PET_ACT_IDLE && p->action_dur_ms != UINT64_MAX &&
        now_ms >= p->action_start_ms + p->action_dur_ms) {
        back_to_idle(p, now_ms);
    }

    if (p->action != APP_PET_ACT_IDLE) return;   // 忙时不叠加新动作

    // 捣乱模式优先于普通随机动作：冒头频率更低，但更有存在感。
    if (p->mischief && now_ms >= p->next_mischief_ms) {
        static const int pool[] = { APP_PET_ACT_DANCE, APP_PET_ACT_DAB, APP_PET_ACT_KICK,
                                    APP_PET_ACT_FACEPALM, APP_PET_ACT_JUMP };
        static const char *const lines[] = { "看我的", "嘿嘿", "走你", "哼唧", "啪!" };
        int n = (int)(sizeof(pool) / sizeof(pool[0]));
        int pick = (int)(app_pet_rand(p) % (uint32_t)n);
        // 循环动作给一个窗口，让"捣乱"结束得干净；一次性动作用自己的时长。
        start_action(p, pool[pick], now_ms, app_pet_action_loops(pool[pick]) ? 3600 : 0);
        say(p, lines[pick], now_ms, 2000);
        p->next_mischief_ms = now_ms + rand_range(p, 12000, 26000);
        return;
    }

    if (now_ms >= p->next_idle_ms) {
        int pick = pick_idle_action(p);
        uint64_t window = app_pet_action_loops(pick) ? rand_range(p, 2200, 4600) : 0;
        start_action(p, pick, now_ms, window);
        if (pick == APP_PET_ACT_SLEEP) say(p, "Zzz", now_ms, window ? window : 4000);
        return;
    }
}

// ---------------------------------------------------------------------------
// 姿态插值
// ---------------------------------------------------------------------------

static int16_t lerp_i16(int16_t a, int16_t b, int num, int den)
{
    if (den <= 0) return a;
    return (int16_t)(a + (int)(((int32_t)(b - a) * num) / den));
}

void app_pet_pose_at(const app_pet_t *p, uint64_t now_ms, app_pet_pose_t *out)
{
    if (!out) return;

    int action = p ? p->action : APP_PET_ACT_IDLE;
    const app_pet_action_t *a = action_of(action);
    uint64_t start = p ? p->action_start_ms : 0;

    uint64_t elapsed = (now_ms > start) ? (now_ms - start) : 0;
    uint64_t total = action_total_ms(action);
    if (a->loop && total) elapsed %= total;

    // 找到 elapsed 落入的关键帧区间。循环动作的最后一帧过渡回首帧。
    uint64_t acc = 0;
    const app_pet_key_t *k0 = &a->keys[0];
    const app_pet_key_t *k1 = &a->keys[a->key_count - 1];
    int num = 0, den = 1;
    if (elapsed < total || !a->loop) {
        for (int i = 0; i < a->key_count; i++) {
            uint32_t d = a->keys[i].dur_ms;
            if (elapsed < acc + d || i == a->key_count - 1) {
                k0 = &a->keys[i];
                k1 = a->loop ? &a->keys[(i + 1) % a->key_count]
                             : &a->keys[i + (i + 1 < a->key_count ? 1 : 0)];
                num = (int)(elapsed - acc);
                den = d ? (int)d : 1;
                if (num > den) num = den;
                break;
            }
            acc += d;
        }
    }

    out->torso      = lerp_i16(k0->torso, k1->torso, num, den);
    out->arm_l_up   = lerp_i16(k0->a_l_up, k1->a_l_up, num, den);
    out->arm_l_fore = lerp_i16(k0->a_l_fore, k1->a_l_fore, num, den);
    out->arm_r_up   = lerp_i16(k0->a_r_up, k1->a_r_up, num, den);
    out->arm_r_fore = lerp_i16(k0->a_r_fore, k1->a_r_fore, num, den);
    out->leg_l_thigh = lerp_i16(k0->l_thigh, k1->l_thigh, num, den);
    out->leg_l_shin  = lerp_i16(k0->l_shin, k1->l_shin, num, den);
    out->leg_r_thigh = lerp_i16(k0->r_thigh, k1->r_thigh, num, den);
    out->leg_r_shin  = lerp_i16(k0->r_shin, k1->r_shin, num, den);
    out->root_dx = lerp_i16(k0->dx, k1->dx, num, den);
    out->root_dy = lerp_i16(k0->dy, k1->dy, num, den);
}

// ---------------------------------------------------------------------------
// 骨架落点
// ---------------------------------------------------------------------------
// 各段长度按画布高度取百分比：躯干 20%、大臂 13%、小臂 11%、大腿/小腿各 14%、头半径 8%。
// 站姿髋部落在 62% 处，于是脚底约在 90%、头顶约在 24%：上下各留出约一成余量，
// 双臂高举（欢呼/伸懒腰）与起跳、坐下都不会被画布裁掉。比例刻意偏"头小身长"，
// 与 xkcd / Alan Becker 那种细长火柴人是同一路数。
//
// 这条"所有落点都在 [0, w]×[0, h] 内"的约束由 tests/test_app_pet.c 逐动作断言，
// 改动比例或关键帧时若越界会被主机测试直接拦下，不必靠肉眼在设备上找。

static int limb_x(int base, int ang, int len)
{
    return base + (int)(((int64_t)app_pet_sin_q16(ang) * len) >> 16);
}

static int limb_y(int base, int ang, int len)
{
    // cos(ang) = sin(ang + 90)
    return base + (int)(((int64_t)app_pet_sin_q16(ang + 90) * len) >> 16);
}

void app_pet_skeleton(const app_pet_pose_t *pose, int w, int h, app_pet_skeleton_t *out)
{
    if (!out) return;
    app_pet_pose_t neutral = {0};
    if (!pose) pose = &neutral;

    int torso_len = h * 20 / 100;
    int upper_len = h * 13 / 100;
    int fore_len  = h * 11 / 100;
    int thigh_len = h * 14 / 100;
    int shin_len  = h * 14 / 100;
    int head_r    = h * 8 / 100;
    int neck_len  = h * 2 / 100;

    int hip_x = w / 2 + (int)pose->root_dx * h / 100;
    int hip_y = h * 62 / 100 + (int)pose->root_dy * h / 100;

    // 躯干由髋向上（len 取负即反向），肩在躯干末端。
    int sh_x = limb_x(hip_x, pose->torso, -torso_len);
    int sh_y = limb_y(hip_y, pose->torso, -torso_len);
    // 头继续沿躯干方向向上，隔一个脖子长度。
    int hd_x = limb_x(sh_x, pose->torso, -(neck_len + head_r));
    int hd_y = limb_y(sh_y, pose->torso, -(neck_len + head_r));

    out->hip_x = (int16_t)hip_x;
    out->hip_y = (int16_t)hip_y;
    out->shoulder_x = (int16_t)sh_x;
    out->shoulder_y = (int16_t)sh_y;
    out->head_x = (int16_t)hd_x;
    out->head_y = (int16_t)hd_y;
    out->head_r = (int16_t)head_r;

    out->elbow_l_x = (int16_t)limb_x(sh_x, pose->arm_l_up, upper_len);
    out->elbow_l_y = (int16_t)limb_y(sh_y, pose->arm_l_up, upper_len);
    out->hand_l_x  = (int16_t)limb_x(out->elbow_l_x, pose->arm_l_fore, fore_len);
    out->hand_l_y  = (int16_t)limb_y(out->elbow_l_y, pose->arm_l_fore, fore_len);

    out->elbow_r_x = (int16_t)limb_x(sh_x, pose->arm_r_up, upper_len);
    out->elbow_r_y = (int16_t)limb_y(sh_y, pose->arm_r_up, upper_len);
    out->hand_r_x  = (int16_t)limb_x(out->elbow_r_x, pose->arm_r_fore, fore_len);
    out->hand_r_y  = (int16_t)limb_y(out->elbow_r_y, pose->arm_r_fore, fore_len);

    out->knee_l_x = (int16_t)limb_x(hip_x, pose->leg_l_thigh, thigh_len);
    out->knee_l_y = (int16_t)limb_y(hip_y, pose->leg_l_thigh, thigh_len);
    out->foot_l_x = (int16_t)limb_x(out->knee_l_x, pose->leg_l_shin, shin_len);
    out->foot_l_y = (int16_t)limb_y(out->knee_l_y, pose->leg_l_shin, shin_len);

    out->knee_r_x = (int16_t)limb_x(hip_x, pose->leg_r_thigh, thigh_len);
    out->knee_r_y = (int16_t)limb_y(hip_y, pose->leg_r_thigh, thigh_len);
    out->foot_r_x = (int16_t)limb_x(out->knee_r_x, pose->leg_r_shin, shin_len);
    out->foot_r_y = (int16_t)limb_y(out->knee_r_y, pose->leg_r_shin, shin_len);
}