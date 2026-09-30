// main/logic/app_pet.c —— 火柴人桌宠的动作库与状态机（详见 app_pet.h）。
#include "app_pet.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 关键帧与动作表
// ---------------------------------------------------------------------------
// 每个动作是一串关键帧，帧间做整数插值。角度单位是度：0 朝下、90 朝右、±180 朝上；
// dx/dy 是髋部相对画布中心的偏移，单位是"身高百分比"（正数向右/向下）。
// 全部用指定初始化器书写，只写需要偏离"站立"的关节，一眼能看出这个动作在动哪里。
//
// 摆姿势时反复用到的三条经验（照抄 xkcd / Alan Becker 那类细长火柴人的感觉）：
//  1. 四肢别贴着躯干画。绕躯干中线 20° 以内、又和躯干同色的斜线在 70px 的画布上会和
//     躯干糊成一根粗线，看着像"叠起来了"。所以站姿本身就把手臂张开到 ±24°、双腿分开
//     到 ±15°，居中动作也尽量让肘、膝离开中线。
//  2. 关节角度是绝对的（0 朝下），不随躯干转。躯干摆动时四肢靠重力自然下垂，比刚性
//     跟随更松弛；写关键帧时也只需要关心"这一段朝哪指"。
//  3. 动作要有起势、到位、回弹。关键帧之间用三次 Hermite（Catmull-Rom）样条过渡，
//     见 hermite_i16：它既穿过每个关键帧，又在关键帧处速度连续。分段线性会让走、跑
//     在每个关键帧"拐一下"，分段缓动又会让跳跃、挥手在半空中"停一下"，两种都不够顺。
//
// 站姿（所有动作的共同基准）：双臂自然下垂并微微外张，双腿分开站稳。
#define NEUTRAL                                                             \
    .a_l_up = -24, .a_l_fore = -20, .a_r_up = 24, .a_r_fore = 20,           \
    .l_thigh = -15, .l_shin = -5, .r_thigh = 15, .r_shin = 5

typedef struct {
    int16_t  torso;
    int16_t  head;     // 头部相对躯干方向的偏角，0 = 头随躯干
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
// 改写成 12 个字段全写满——后者在几百帧的规模下反而更容易看漏、写错。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#endif

// ---------------------------------------------------------------------------
// 基础与日常
// ---------------------------------------------------------------------------

// 站立呼吸。除了极小的起伏，还夹一点头部左右偏和重心轻微挪动，让长时间的默认状态
// 不至于变成"每隔 2.4 秒重复一次"的机械循环。
static const app_pet_key_t k_idle[] = {
    { NEUTRAL, .dur_ms = 1900 },
    { NEUTRAL, .a_l_up = -27, .a_r_up = 27, .dy = -1, .dur_ms = 1900 },
    { NEUTRAL, .head = -7, .a_l_up = -25, .a_r_up = 25, .dur_ms = 1500 },
    { NEUTRAL, .a_l_up = -27, .a_r_up = 27, .dy = -1, .dur_ms = 1900 },
    { NEUTRAL, .head = 6, .a_l_up = -22, .a_r_up = 22, .dy = 1, .dur_ms = 1700 },
};

// 左右张望。头和躯干往同一侧偏，头的偏角更大：远看是"扭头在找什么"，而只掰躯干
// 会变成整个人在往一边倒。正角朝左、负角朝右，与骨架里 sin 的符号一致。
static const app_pet_key_t k_look[] = {
    { NEUTRAL, .dur_ms = 400 },
    { NEUTRAL, .torso = 6, .head = 15, .a_l_up = -30, .a_r_up = 34, .dur_ms = 620 },
    { NEUTRAL, .dur_ms = 280 },
    { NEUTRAL, .torso = -6, .head = -15, .a_l_up = -34, .a_r_up = 30, .dur_ms = 620 },
    { NEUTRAL, .dur_ms = 380 },
};

// 挥手：右臂举到侧上方反复摆，头和身体略朝挥手一侧倾。手臂走"举起—摆—摆—放下"，
// 不在头前方横穿，避免和头、躯干叠在一起。
static const app_pet_key_t k_wave[] = {
    { NEUTRAL, .dur_ms = 280 },
    { NEUTRAL, .torso = -4, .head = 5, .a_r_up = 128, .a_r_fore = 150, .dur_ms = 300 },
    { NEUTRAL, .torso = -4, .head = 5, .a_r_up = 142, .a_r_fore = 115, .dur_ms = 230 },
    { NEUTRAL, .torso = -4, .head = 5, .a_r_up = 142, .a_r_fore = 160, .dur_ms = 230 },
    { NEUTRAL, .torso = -4, .head = 5, .a_r_up = 142, .a_r_fore = 115, .dur_ms = 230 },
    { NEUTRAL, .torso = -4, .head = 5, .a_r_up = 142, .a_r_fore = 160, .dur_ms = 230 },
    { NEUTRAL, .dur_ms = 300 },
};

// 原地跳：蓄力下蹲、腾空收腿、落地缓冲、再回弹一下。收腿用"膝盖外张、小腿折回中线"的
// 写法——正视视角下这是唯一能读出"腿收起来了"的姿势。
static const app_pet_key_t k_jump[] = {
    { NEUTRAL, .dur_ms = 130 },
    { NEUTRAL, .dy = 7, .l_thigh = -30, .l_shin = 30, .r_thigh = 30, .r_shin = -30,
      .a_l_up = -30, .a_r_up = 30, .dur_ms = 150 },
    { NEUTRAL, .dy = -15, .l_thigh = -42, .l_shin = 42, .r_thigh = 42, .r_shin = -42,
      .a_l_up = -135, .a_r_up = 135, .dur_ms = 300 },
    { NEUTRAL, .dy = 4, .l_thigh = -20, .l_shin = 20, .r_thigh = 20, .r_shin = -20,
      .a_l_up = -55, .a_r_up = 55, .dur_ms = 170 },
    { NEUTRAL, .dy = -3, .a_l_up = -34, .a_r_up = 34, .dur_ms = 150 },
    { NEUTRAL, .dur_ms = 240 },
};

// 原地踏步：一条腿抬膝、另一条支撑，手臂反向摆。抬腿的膝盖向外、小腿折回中线下，脚
// 明确离开地面，才看得出是在"走"。
static const app_pet_key_t k_walk[] = {
    { NEUTRAL, .l_thigh = -48, .l_shin = 48, .r_thigh = 14, .r_shin = 5,
      .a_l_up = -16, .a_r_up = 44, .head = -4, .dy = -2, .dur_ms = 320 },
    { NEUTRAL, .dy = 2, .dur_ms = 130 },
    { NEUTRAL, .r_thigh = 48, .r_shin = -48, .l_thigh = -14, .l_shin = -5,
      .a_l_up = -44, .a_r_up = 16, .head = 4, .dy = -2, .dur_ms = 320 },
    { NEUTRAL, .dy = 2, .dur_ms = 130 },
};

// 原地小跑：抬腿更高、身体起伏更大，手臂屈肘在身侧前后甩。比 walk 快一倍。
// 每段至少 110ms：桌宠按 50ms 一拍重绘，段落短于两拍的话中间帧根本采不到，
// 看上去就是"两帧之间跳过去"。跑步是快动作，但快要靠整体周期短来体现，不能靠段落短。
static const app_pet_key_t k_run[] = {
    { NEUTRAL, .l_thigh = -58, .l_shin = 62, .r_thigh = 16, .r_shin = 6,
      .a_l_up = -24, .a_l_fore = -150, .a_r_up = 24, .a_r_fore = -5, .dy = -6, .dur_ms = 160 },
    { NEUTRAL, .l_thigh = -24, .l_shin = 30, .r_thigh = 16, .r_shin = 4,
      .a_l_up = -24, .a_l_fore = -80, .a_r_up = 24, .a_r_fore = -80, .dy = 3, .dur_ms = 110 },
    { NEUTRAL, .r_thigh = 58, .r_shin = -62, .l_thigh = -16, .l_shin = -6,
      .a_l_up = -24, .a_l_fore = -5, .a_r_up = 24, .a_r_fore = 150, .dy = -6, .dur_ms = 160 },
    { NEUTRAL, .r_thigh = 24, .r_shin = -30, .l_thigh = -16, .l_shin = -4,
      .a_l_up = -24, .a_l_fore = -80, .a_r_up = 24, .a_r_fore = -80, .dy = 3, .dur_ms = 110 },
};

// 席地而坐：髋部整体下沉、双膝向两侧抬起、双脚收回到身体中线，双手搭在身前。
static const app_pet_key_t k_sit[] = {
    { NEUTRAL, .dur_ms = 300 },
    { NEUTRAL, .dy = 20, .torso = 3, .l_thigh = -70, .l_shin = 70, .r_thigh = 70,
      .r_shin = -70, .a_l_up = -30, .a_l_fore = 55, .a_r_up = 30, .a_r_fore = -55,
      .head = -3, .dur_ms = 380 },
    { NEUTRAL, .dy = 20, .torso = 4, .l_thigh = -72, .l_shin = 72, .r_thigh = 72,
      .r_shin = -72, .a_l_up = -30, .a_l_fore = 55, .a_r_up = 30, .a_r_fore = -55,
      .head = 3, .dur_ms = 1500 },
    { NEUTRAL, .dur_ms = 380 },
};

// 深蹲起立：双脚分开、膝盖外张、髋部下沉，手臂自然张开保持平衡。
static const app_pet_key_t k_squat[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .dy = 15, .l_thigh = -58, .l_shin = 30, .r_thigh = 58, .r_shin = -30,
      .a_l_up = -30, .a_l_fore = -26, .a_r_up = 30, .a_r_fore = 26, .head = -4, .dur_ms = 380 },
    { NEUTRAL, .dy = 15, .l_thigh = -58, .l_shin = 30, .r_thigh = 58, .r_shin = -30,
      .a_l_up = -30, .a_l_fore = -26, .a_r_up = 30, .a_r_fore = 26, .head = 4, .dur_ms = 420 },
    { NEUTRAL, .dur_ms = 400 },
};

// 打盹：躯干前倾、头垂向一侧，幅度很慢的呼吸，配一个 "Zzz" 台词。
static const app_pet_key_t k_sleep[] = {
    { NEUTRAL, .torso = 8, .head = 12, .a_l_up = -18, .a_l_fore = -14, .a_r_up = 18,
      .a_r_fore = 14, .dy = 3, .dur_ms = 1700 },
    { NEUTRAL, .torso = 13, .head = 17, .a_l_up = -16, .a_l_fore = -12, .a_r_up = 16,
      .a_r_fore = 12, .dy = 7, .dur_ms = 1700 },
    { NEUTRAL, .torso = 8, .head = 12, .a_l_up = -18, .a_l_fore = -14, .a_r_up = 18,
      .a_r_fore = 14, .dy = 3, .dur_ms = 1700 },
    { NEUTRAL, .dur_ms = 460 },
};

// 打哈欠：两只手拢到嘴前、身体向后沉一下，再慢慢放下。
static const app_pet_key_t k_yawn[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = -4, .dy = 2, .head = -6, .a_l_up = -32, .a_l_fore = 140,
      .a_r_up = 32, .a_r_fore = -140, .dur_ms = 700 },
    { NEUTRAL, .torso = -5, .dy = 3, .head = 5, .a_l_up = -32, .a_l_fore = 150,
      .a_r_up = 32, .a_r_fore = -150, .dur_ms = 800 },
    { NEUTRAL, .dy = 1, .dur_ms = 340 },
};

// 伸懒腰：双臂举过头顶、躯干微微后仰，撑住一会儿。上臂留在 ±148°：再往竖直里收，
// 肘关节就会落进头部圆里，看上去是一根线把头劈开（见 tests/test_app_pet.c 的肘部断言）。
static const app_pet_key_t k_stretch[] = {
    { NEUTRAL, .dur_ms = 280 },
    { NEUTRAL, .torso = -3, .dy = -6, .a_l_up = -148, .a_l_fore = -162,
      .a_r_up = 148, .a_r_fore = 162, .dur_ms = 700 },
    { NEUTRAL, .torso = -4, .dy = -7, .head = -5, .a_l_up = -150, .a_l_fore = -164,
      .a_r_up = 150, .a_r_fore = 164, .dur_ms = 560 },
    { NEUTRAL, .dur_ms = 380 },
};

// 来回踱步：重心左右挪、两条腿交替迈出，再走回来。dx 让整个人在画布里横移。
static const app_pet_key_t k_pace[] = {
    { NEUTRAL, .dx = -5, .l_thigh = -30, .l_shin = 18, .r_thigh = 20, .r_shin = -14,
      .a_l_up = -36, .a_r_up = 34, .head = -6, .dur_ms = 420 },
    { NEUTRAL, .dx = 0, .l_thigh = -16, .r_thigh = 16, .dy = -1, .dur_ms = 220 },
    { NEUTRAL, .dx = 5, .r_thigh = 30, .r_shin = -18, .l_thigh = -20, .l_shin = 14,
      .a_l_up = -34, .a_r_up = 36, .head = 6, .dur_ms = 420 },
    { NEUTRAL, .dx = 0, .l_thigh = -16, .r_thigh = 16, .dy = -1, .dur_ms = 220 },
};

// 踮脚张望：整个人拔高、手臂贴身、头歪向一侧，像在人群里找东西。
static const app_pet_key_t k_tiptoe[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .dy = -5, .l_thigh = -8, .l_shin = -2, .r_thigh = 8, .r_shin = 2,
      .a_l_up = -18, .a_l_fore = -14, .a_r_up = 18, .a_r_fore = 14, .head = -20, .dur_ms = 700 },
    { NEUTRAL, .dy = -5, .l_thigh = -8, .l_shin = -2, .r_thigh = 8, .r_shin = 2,
      .a_l_up = -18, .a_l_fore = -14, .a_r_up = 18, .a_r_fore = 14, .head = 22, .dur_ms = 700 },
    { NEUTRAL, .dur_ms = 320 },
};

// 抖腿：抱臂站着，一条腿不停地抬起、放下。循环动作。
// 抬起靠"屈膝收小腿"实现——只改大腿角度时小腿几乎跟着平移，脚的高度几乎不变，
// 画面上就成了一动不动的姿势。
static const app_pet_key_t k_tap_foot[] = {
    { NEUTRAL, .a_l_up = -40, .a_l_fore = 120, .a_r_up = 40, .a_r_fore = -120,
      .r_thigh = 28, .r_shin = -34, .head = -6, .dur_ms = 190 },
    { NEUTRAL, .a_l_up = -40, .a_l_fore = 120, .a_r_up = 40, .a_r_fore = -120,
      .r_thigh = 16, .r_shin = 2, .head = -6, .dur_ms = 190 },
};

// ---------------------------------------------------------------------------
// 情绪表达
// ---------------------------------------------------------------------------

// 托腮思考：右手托到下巴附近，头歪向手的一侧，身体轻轻晃。
static const app_pet_key_t k_think[] = {
    { NEUTRAL, .torso = 4, .head = -10, .a_r_up = 128, .a_r_fore = -46,
      .a_l_up = -30, .a_l_fore = 118, .dur_ms = 1500 },
    { NEUTRAL, .torso = 6, .head = -12, .a_r_up = 124, .a_r_fore = -42,
      .a_l_up = -28, .a_l_fore = 122, .dur_ms = 1500 },
};

// 欢呼：双臂高举 + 两下小跳。
static const app_pet_key_t k_cheer[] = {
    { NEUTRAL, .dur_ms = 170 },
    { NEUTRAL, .a_l_up = -148, .a_l_fore = -162, .a_r_up = 148, .a_r_fore = 162,
      .dy = -12, .head = -5, .dur_ms = 200 },
    { NEUTRAL, .a_l_up = -148, .a_l_fore = -162, .a_r_up = 148, .a_r_fore = 162,
      .dy = 5, .dur_ms = 190 },
    { NEUTRAL, .a_l_up = -148, .a_l_fore = -162, .a_r_up = 148, .a_r_fore = 162,
      .dy = -14, .head = 5, .dur_ms = 200 },
    { NEUTRAL, .a_l_up = -148, .a_l_fore = -162, .a_r_up = 148, .a_r_fore = 162,
      .dy = 5, .dur_ms = 190 },
    { NEUTRAL, .dur_ms = 320 },
};

// 胜利：双臂屈肘握拳上举，撑住，收势时下沉一下。
static const app_pet_key_t k_victory[] = {
    { NEUTRAL, .dur_ms = 220 },
    { NEUTRAL, .a_l_up = -142, .a_l_fore = 172, .a_r_up = 142, .a_r_fore = -172,
      .dy = -4, .dur_ms = 340 },
    { NEUTRAL, .a_l_up = -142, .a_l_fore = 172, .a_r_up = 142, .a_r_fore = -172,
      .dy = -5, .head = -6, .dur_ms = 700 },
    { NEUTRAL, .a_l_up = -142, .a_l_fore = 172, .a_r_up = 142, .a_r_fore = -172,
      .dy = -5, .head = 6, .dur_ms = 500 },
    { NEUTRAL, .dur_ms = 320 },
};

// 捧腹大笑：身体后仰着上下抖，两只手捂在肚子上。
static const app_pet_key_t k_laugh[] = {
    { NEUTRAL, .dur_ms = 190 },
    { NEUTRAL, .torso = -6, .head = -10, .dy = 4, .a_l_up = -26, .a_l_fore = 52,
      .a_r_up = 26, .a_r_fore = -52, .dur_ms = 240 },
    { NEUTRAL, .torso = -11, .head = -14, .dy = 8, .a_l_up = -24, .a_l_fore = 58,
      .a_r_up = 24, .a_r_fore = -58, .dur_ms = 220 },
    { NEUTRAL, .torso = -6, .head = -10, .dy = 4, .a_l_up = -26, .a_l_fore = 52,
      .a_r_up = 26, .a_r_fore = -52, .dur_ms = 240 },
    { NEUTRAL, .torso = -11, .head = -14, .dy = 8, .a_l_up = -24, .a_l_fore = 58,
      .a_r_up = 24, .a_r_fore = -58, .dur_ms = 220 },
    { NEUTRAL, .dur_ms = 280 },
};

// 揉眼哭：大臂向外平举、前臂折回来捂到脸前，肩膀一抽一抽地抖。
// 不用"肘抬到头顶"那种姿势——那样肘关节会插进脑袋里，看着像头被穿了个洞。
static const app_pet_key_t k_cry[] = {
    { NEUTRAL, .dur_ms = 220 },
    { NEUTRAL, .torso = 5, .head = -6, .dy = 3, .a_l_up = -100, .a_l_fore = 99,
      .a_r_up = 100, .a_r_fore = -99, .dur_ms = 320 },
    { NEUTRAL, .torso = 7, .head = 6, .dy = 5, .a_l_up = -104, .a_l_fore = 97,
      .a_r_up = 104, .a_r_fore = -97, .dur_ms = 240 },
    { NEUTRAL, .torso = 5, .head = -6, .dy = 3, .a_l_up = -100, .a_l_fore = 99,
      .a_r_up = 100, .a_r_fore = -99, .dur_ms = 240 },
    { NEUTRAL, .torso = 7, .head = 6, .dy = 5, .a_l_up = -104, .a_l_fore = 97,
      .a_r_up = 104, .a_r_fore = -97, .dur_ms = 240 },
    { NEUTRAL, .dur_ms = 300 },
};

// 生气跺脚：握拳在身侧、抬膝跺下，头跟着甩。
static const app_pet_key_t k_angry[] = {
    { NEUTRAL, .dur_ms = 180 },
    { NEUTRAL, .dy = -5, .head = -9, .r_thigh = 52, .r_shin = -52, .l_thigh = -14,
      .l_shin = -5, .a_l_up = -40, .a_l_fore = 25, .a_r_up = 40, .a_r_fore = -25, .dur_ms = 210 },
    { NEUTRAL, .dy = 4, .head = 9, .a_l_up = -34, .a_l_fore = 18,
      .a_r_up = 34, .a_r_fore = -18, .dur_ms = 130 },
    { NEUTRAL, .dy = -5, .head = -9, .l_thigh = -52, .l_shin = 52, .r_thigh = 14,
      .r_shin = 5, .a_l_up = -40, .a_l_fore = 25, .a_r_up = 40, .a_r_fore = -25, .dur_ms = 210 },
    { NEUTRAL, .dy = 4, .head = 9, .a_l_up = -34, .a_l_fore = 18,
      .a_r_up = 34, .a_r_fore = -18, .dur_ms = 130 },
    { NEUTRAL, .dur_ms = 260 },
};

// 捂脸：右手抬到脸部，躯干前倾，头和身体一起往手的一侧沉，撑住不动。
static const app_pet_key_t k_facepalm[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = 8, .head = -18, .a_r_up = 136, .a_r_fore = -58,
      .a_l_up = -22, .a_l_fore = -30, .dur_ms = 360 },
    { NEUTRAL, .torso = 9, .head = -20, .a_r_up = 138, .a_r_fore = -56,
      .a_l_up = -22, .a_l_fore = -30, .dur_ms = 1000 },
    { NEUTRAL, .dur_ms = 340 },
};

// 叹气：肩膀先抬起来，再整个沉下去，头垂向一侧，停一会儿才直起身。
static const app_pet_key_t k_sigh[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = -2, .head = -4, .dy = 1, .a_l_up = -20, .a_r_up = 20, .dur_ms = 420 },
    { NEUTRAL, .torso = 5, .head = -12, .dy = 6, .a_l_up = -12, .a_l_fore = -8,
      .a_r_up = 12, .a_r_fore = 8, .dur_ms = 900 },
    { NEUTRAL, .torso = 1, .head = -3, .dy = 2, .a_l_up = -20, .a_r_up = 20, .dur_ms = 640 },
    { NEUTRAL, .dur_ms = 300 },
};

// 耸肩摊手：肘向外、前臂向上翻，掌心朝上，头一歪。
static const app_pet_key_t k_shrug[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = -3, .head = -10, .dy = 2, .a_l_up = -52, .a_l_fore = -118,
      .a_r_up = 52, .a_r_fore = 118, .dur_ms = 420 },
    { NEUTRAL, .torso = -3, .head = -11, .dy = 3, .a_l_up = -58, .a_l_fore = -110,
      .a_r_up = 58, .a_r_fore = 110, .dur_ms = 520 },
    { NEUTRAL, .dur_ms = 340 },
};

// 发抖：小幅度快速抖动。
static const app_pet_key_t k_shake[] = {
    { NEUTRAL, .torso = 3, .head = -3, .a_l_up = -28, .a_l_fore = -22, .a_r_up = 28,
      .a_r_fore = 22, .dy = 2, .dur_ms = 110 },
    { NEUTRAL, .torso = -3, .head = 3, .a_l_up = -18, .a_l_fore = -12, .a_r_up = 18,
      .a_r_fore = 12, .dy = -2, .dur_ms = 110 },
};

// 打喷嚏：先缩起来蓄势，再猛地向前一冲、双手捂到嘴上。
static const app_pet_key_t k_sneeze[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = -8, .head = 6, .dy = -3, .a_l_up = -118, .a_l_fore = -40,
      .a_r_up = 118, .a_r_fore = 40, .dur_ms = 420 },
    { NEUTRAL, .torso = 15, .head = -6, .dy = 8, .a_l_up = -142, .a_l_fore = 58,
      .a_r_up = 142, .a_r_fore = -58, .dur_ms = 120 },
    { NEUTRAL, .torso = 5, .dy = 2, .a_l_up = -96, .a_l_fore = 40,
      .a_r_up = 96, .a_r_fore = -40, .dur_ms = 220 },
    { NEUTRAL, .dur_ms = 300 },
};

// 摇头：头和上身一起左右摆，是最短的"不同意"表达。头只是圆，光靠头部偏角自己摆，
// 在 70px 上位移不到 3px 几乎看不出来，所以让躯干跟着一起晃。
static const app_pet_key_t k_head_shake[] = {
    { NEUTRAL, .dur_ms = 220 },
    { NEUTRAL, .torso = 9, .head = 20, .a_l_up = -28, .a_r_up = 28, .dur_ms = 250 },
    { NEUTRAL, .torso = -9, .head = -20, .dur_ms = 250 },
    { NEUTRAL, .torso = 9, .head = 20, .dur_ms = 250 },
    { NEUTRAL, .torso = -9, .head = -20, .dur_ms = 250 },
    { NEUTRAL, .dur_ms = 260 },
};

// ---------------------------------------------------------------------------
// 手势与社交
// ---------------------------------------------------------------------------

// 鼓掌：两肘固定在身侧，前臂在"张开"与"合到胸前"之间来回，连拍三下。
static const app_pet_key_t k_clap[] = {
    { NEUTRAL, .dur_ms = 180 },
    { NEUTRAL, .a_l_up = -22, .a_l_fore = -30, .a_r_up = 22, .a_r_fore = 30, .dur_ms = 140 },
    { NEUTRAL, .a_l_up = -22, .a_l_fore = 150, .a_r_up = 22, .a_r_fore = -150, .dur_ms = 110 },
    { NEUTRAL, .a_l_up = -22, .a_l_fore = -30, .a_r_up = 22, .a_r_fore = 30, .dur_ms = 140 },
    { NEUTRAL, .a_l_up = -22, .a_l_fore = 150, .a_r_up = 22, .a_r_fore = -150, .dur_ms = 110 },
    { NEUTRAL, .a_l_up = -22, .a_l_fore = -30, .a_r_up = 22, .a_r_fore = 30, .dur_ms = 140 },
    { NEUTRAL, .a_l_up = -22, .a_l_fore = 150, .a_r_up = 22, .a_r_fore = -150, .dur_ms = 200 },
    { NEUTRAL, .dur_ms = 260 },
};

// 敬礼：右手抬到太阳穴旁，身体绷直，停住再放下。
static const app_pet_key_t k_salute[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .dy = -1, .head = -3, .a_r_up = 50, .a_r_fore = -160,
      .a_l_up = -14, .a_l_fore = -12, .dur_ms = 400 },
    { NEUTRAL, .dy = -1, .head = -3, .a_r_up = 52, .a_r_fore = -162,
      .a_l_up = -14, .a_l_fore = -12, .dur_ms = 1100 },
    { NEUTRAL, .dur_ms = 360 },
};

// 抬手比划：右臂斜上举着指出去，头和身体跟着看过去。
static const app_pet_key_t k_point[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = 5, .head = -14, .a_r_up = 142, .a_r_fore = 142,
      .a_l_up = -20, .a_l_fore = 40, .dur_ms = 380 },
    { NEUTRAL, .torso = 5, .head = -15, .a_r_up = 145, .a_r_fore = 145,
      .a_l_up = -20, .a_l_fore = 40, .dur_ms = 900 },
    { NEUTRAL, .dur_ms = 320 },
};

// 招手叫过来：右臂横举，前臂一遍遍往里勾。
static const app_pet_key_t k_beckon[] = {
    { NEUTRAL, .dur_ms = 200 },
    { NEUTRAL, .torso = -3, .head = 6, .a_r_up = 78, .a_r_fore = 142, .dur_ms = 230 },
    { NEUTRAL, .torso = -3, .head = 6, .a_r_up = 78, .a_r_fore = 205, .dur_ms = 230 },
    { NEUTRAL, .torso = -3, .head = 6, .a_r_up = 78, .a_r_fore = 142, .dur_ms = 230 },
    { NEUTRAL, .torso = -3, .head = 6, .a_r_up = 78, .a_r_fore = 205, .dur_ms = 230 },
    { NEUTRAL, .dur_ms = 300 },
};

// 秀肌肉：双臂屈到九十度、拳头上举，停住，再微微用力抖一下。
static const app_pet_key_t k_flex[] = {
    { NEUTRAL, .dur_ms = 220 },
    { NEUTRAL, .dy = -1, .a_l_up = -70, .a_l_fore = -168, .a_r_up = 70, .a_r_fore = 168,
      .dur_ms = 320 },
    { NEUTRAL, .dy = -2, .head = -7, .a_l_up = -74, .a_l_fore = -172, .a_r_up = 74,
      .a_r_fore = 172, .dur_ms = 420 },
    { NEUTRAL, .dy = -2, .head = 7, .a_l_up = -70, .a_l_fore = -168, .a_r_up = 70,
      .a_r_fore = 168, .dur_ms = 420 },
    { NEUTRAL, .dur_ms = 320 },
};

// 抱臂：两只手在胸前交叉握住，撑住。
static const app_pet_key_t k_arms_crossed[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .dy = -1, .head = -4, .a_l_up = -40, .a_l_fore = 118,
      .a_r_up = 40, .a_r_fore = -118, .dur_ms = 400 },
    { NEUTRAL, .dy = -1, .head = -5, .a_l_up = -42, .a_l_fore = 122,
      .a_r_up = 42, .a_r_fore = -122, .dur_ms = 1300 },
    { NEUTRAL, .dur_ms = 320 },
};

// 叉腰：肘向外撑开，手收到腰上。
static const app_pet_key_t k_hands_on_hips[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .head = -6, .a_l_up = -50, .a_l_fore = 40, .a_r_up = 50, .a_r_fore = -40,
      .dur_ms = 420 },
    { NEUTRAL, .head = 6, .a_l_up = -52, .a_l_fore = 42, .a_r_up = 52, .a_r_fore = -42,
      .dur_ms = 900 },
    { NEUTRAL, .dur_ms = 320 },
};

// 打电话：右手抬到耳边，左手搭在腰上，头偏向听筒一侧。
static const app_pet_key_t k_phone[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = 4, .head = -11, .a_r_up = 120, .a_r_fore = -130,
      .a_l_up = -30, .a_l_fore = 40, .dur_ms = 380 },
    { NEUTRAL, .torso = 4, .head = -12, .a_r_up = 122, .a_r_fore = -128,
      .a_l_up = -30, .a_l_fore = 40, .dur_ms = 1500 },
    { NEUTRAL, .dur_ms = 340 },
};

// 喝水：右手把杯子端到嘴边，仰头喝两口，另一只手自然垂着。
static const app_pet_key_t k_drink[] = {
    { NEUTRAL, .dur_ms = 240 },
    { NEUTRAL, .torso = 3, .head = -9, .a_r_up = 118, .a_r_fore = -136,
      .a_l_up = -22, .a_l_fore = -18, .dur_ms = 400 },
    { NEUTRAL, .torso = 5, .head = -13, .a_r_up = 124, .a_r_fore = -146,
      .a_l_up = -22, .a_l_fore = -18, .dur_ms = 420 },
    { NEUTRAL, .torso = 3, .head = -9, .a_r_up = 118, .a_r_fore = -136,
      .a_l_up = -22, .a_l_fore = -18, .dur_ms = 400 },
    { NEUTRAL, .torso = 5, .head = -13, .a_r_up = 124, .a_r_fore = -146,
      .a_l_up = -22, .a_l_fore = -18, .dur_ms = 420 },
    { NEUTRAL, .dur_ms = 340 },
};

// 敲门：抬右臂，前臂一下一下往里敲，身体微微前倾。
static const app_pet_key_t k_knock[] = {
    { NEUTRAL, .dur_ms = 200 },
    { NEUTRAL, .torso = 3, .head = 5, .a_r_up = 66, .a_r_fore = 160, .dur_ms = 150 },
    { NEUTRAL, .torso = 3, .head = 5, .a_r_up = 66, .a_r_fore = 118, .dur_ms = 150 },
    { NEUTRAL, .torso = 3, .head = 5, .a_r_up = 66, .a_r_fore = 160, .dur_ms = 150 },
    { NEUTRAL, .torso = 3, .head = 5, .a_r_up = 66, .a_r_fore = 118, .dur_ms = 150 },
    { NEUTRAL, .dur_ms = 300 },
};

// 看书：双手捧在胸前，头低下去，身体随呼吸轻晃。
static const app_pet_key_t k_read[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .dy = 2, .torso = 4, .head = -7, .a_l_up = -32, .a_l_fore = 138,
      .a_r_up = 32, .a_r_fore = -138, .dur_ms = 520 },
    { NEUTRAL, .dy = 3, .torso = 5, .head = -7, .a_l_up = -32, .a_l_fore = 142,
      .a_r_up = 32, .a_r_fore = -142, .dur_ms = 900 },
    { NEUTRAL, .dy = 2, .torso = 4, .head = -6, .a_l_up = -32, .a_l_fore = 138,
      .a_r_up = 32, .a_r_fore = -138, .dur_ms = 620 },
    { NEUTRAL, .dur_ms = 300 },
};

// 看表：左臂横到胸前，头低下去盯着手腕，停一会儿。
static const app_pet_key_t k_watch[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .torso = 6, .head = 14, .dy = 1, .a_l_up = -34, .a_l_fore = 150,
      .a_r_up = 24, .a_r_fore = 18, .dur_ms = 460 },
    { NEUTRAL, .torso = 6, .head = 15, .dy = 1, .a_l_up = -34, .a_l_fore = 152,
      .a_r_up = 24, .a_r_fore = 18, .dur_ms = 1100 },
    { NEUTRAL, .dur_ms = 340 },
};

// ---------------------------------------------------------------------------
// 舞蹈
// ---------------------------------------------------------------------------

// 摇摆舞：躯干左右摆、手臂大幅交替，脚下跟着弹。就是那段"跳鸡舞"的抽象版。
static const app_pet_key_t k_dance[] = {
    { NEUTRAL, .torso = 8, .head = -6, .a_l_up = -92, .a_l_fore = -64, .a_r_up = 50,
      .a_r_fore = 24, .l_thigh = -20, .l_shin = 6, .r_thigh = 12, .dur_ms = 380 },
    { NEUTRAL, .torso = -8, .head = 6, .a_l_up = -50, .a_l_fore = -24, .a_r_up = 92,
      .a_r_fore = 64, .r_thigh = 20, .r_shin = -6, .l_thigh = -12, .dy = -4, .dur_ms = 380 },
    { NEUTRAL, .torso = 8, .head = -6, .a_l_up = -92, .a_l_fore = -64, .a_r_up = 50,
      .a_r_fore = 24, .l_thigh = -20, .l_shin = 6, .r_thigh = 12, .dur_ms = 380 },
    { NEUTRAL, .torso = -8, .head = 6, .a_l_up = -50, .a_l_fore = -24, .a_r_up = 92,
      .a_r_fore = 64, .r_thigh = 20, .r_shin = -6, .l_thigh = -12, .dy = -4, .dur_ms = 380 },
};

// dab：一臂斜上、另一臂横在胸前。
static const app_pet_key_t k_dab[] = {
    { NEUTRAL, .dur_ms = 200 },
    { NEUTRAL, .torso = 10, .head = -20, .dy = 1, .a_r_up = 128, .a_r_fore = 150,
      .a_l_up = 58, .a_l_fore = 196, .dur_ms = 320 },
    { NEUTRAL, .torso = 11, .head = -22, .dy = 1, .a_r_up = 132, .a_r_fore = 152,
      .a_l_up = 60, .a_l_fore = 198, .dur_ms = 520 },
    { NEUTRAL, .dur_ms = 300 },
};

// 机械舞：两条手臂折成直角、一格一格地转，配合僵硬的小碎步。
static const app_pet_key_t k_robot[] = {
    { NEUTRAL, .a_l_up = -90, .a_l_fore = -90, .a_r_up = 90, .a_r_fore = 90,
      .l_thigh = -12, .l_shin = -4, .r_thigh = 12, .r_shin = 4, .head = 0, .dur_ms = 300 },
    { NEUTRAL, .a_l_up = -90, .a_l_fore = 0, .a_r_up = 90, .a_r_fore = 180,
      .l_thigh = -12, .l_shin = -4, .r_thigh = 12, .r_shin = 4, .head = -6, .dx = -2,
      .dur_ms = 300 },
    { NEUTRAL, .a_l_up = -90, .a_l_fore = 90, .a_r_up = 90, .a_r_fore = 90,
      .l_thigh = -12, .l_shin = -4, .r_thigh = 12, .r_shin = 4, .head = 0, .dur_ms = 300 },
    { NEUTRAL, .a_l_up = -90, .a_l_fore = 180, .a_r_up = 90, .a_r_fore = 0,
      .l_thigh = -12, .l_shin = -4, .r_thigh = 12, .r_shin = 4, .head = 6, .dx = 2,
      .dur_ms = 300 },
};

// 迪斯科指天：一手指天、一手按胯，左右交替，胯跟着扭。
static const app_pet_key_t k_disco[] = {
    { NEUTRAL, .torso = -7, .head = -14, .r_thigh = 16, .r_shin = 4, .l_thigh = -26,
      .l_shin = 10, .a_r_up = 128, .a_r_fore = 132, .a_l_up = -34, .a_l_fore = 80, .dur_ms = 360 },
    { NEUTRAL, .torso = 7, .head = 14, .l_thigh = 26, .l_shin = -4, .r_thigh = -16,
      .r_shin = -10, .a_l_up = -128, .a_l_fore = -132, .a_r_up = 34, .a_r_fore = -80,
      .dur_ms = 360 },
};

// 太空步：重心横移、双脚一前一后地滑，脚下不抬起。
static const app_pet_key_t k_moonwalk[] = {
    { NEUTRAL, .dx = -4, .head = -6, .l_thigh = -30, .l_shin = 10, .r_thigh = 22,
      .r_shin = -4, .a_l_up = -44, .a_l_fore = -30, .a_r_up = 30, .a_r_fore = 20,
      .dy = 1, .dur_ms = 380 },
    { NEUTRAL, .dx = 4, .head = 6, .r_thigh = 30, .r_shin = -10, .l_thigh = -22,
      .l_shin = 4, .a_l_up = -30, .a_l_fore = -20, .a_r_up = 44, .a_r_fore = 30,
      .dy = 1, .dur_ms = 380 },
};

// ---------------------------------------------------------------------------
// 运动与格斗
// ---------------------------------------------------------------------------

// 跳绳：两只手收在身前转绳，整个人一下一下地跳、落地时收腿。
static const app_pet_key_t k_jump_rope[] = {
    { NEUTRAL, .dy = -8, .l_thigh = -16, .l_shin = 26, .r_thigh = 16, .r_shin = -26,
      .a_l_up = -28, .a_l_fore = 40, .a_r_up = 28, .a_r_fore = -40, .dur_ms = 200 },
    { NEUTRAL, .dy = 1, .l_thigh = -14, .l_shin = 2, .r_thigh = 14, .r_shin = -2,
      .a_l_up = -28, .a_l_fore = 48, .a_r_up = 28, .a_r_fore = -48, .dur_ms = 200 },
};

// 开合跳：跳起时双臂上举、双腿张开，落下时收回。
static const app_pet_key_t k_jumping_jack[] = {
    { NEUTRAL, .l_thigh = -32, .l_shin = -12, .r_thigh = 32, .r_shin = 12,
      .a_l_up = -142, .a_l_fore = -164, .a_r_up = 142, .a_r_fore = 164, .dy = -5, .dur_ms = 300 },
    { NEUTRAL, .l_thigh = -10, .l_shin = -3, .r_thigh = 10, .r_shin = 3,
      .a_l_up = -22, .a_l_fore = -18, .a_r_up = 22, .a_r_fore = 18, .dy = 1, .dur_ms = 300 },
};

// 扭胯：重心大幅左右挪，臀部跟着摆，双臂张开举在身侧。
static const app_pet_key_t k_hula[] = {
    { NEUTRAL, .dx = -4, .torso = 7, .head = -6, .l_thigh = -22, .l_shin = -8,
      .r_thigh = 10, .r_shin = 2, .a_l_up = -62, .a_l_fore = -72, .a_r_up = 62,
      .a_r_fore = 72, .dur_ms = 400 },
    { NEUTRAL, .dx = 4, .torso = -7, .head = 6, .r_thigh = 22, .r_shin = 8,
      .l_thigh = -10, .l_shin = -2, .a_l_up = -62, .a_l_fore = -72, .a_r_up = 62,
      .a_r_fore = 72, .dur_ms = 400 },
};

// 踢腿：一条腿侧踢出去，上身反向压，手臂配合摆开。
static const app_pet_key_t k_kick[] = {
    { NEUTRAL, .dur_ms = 200 },
    { NEUTRAL, .torso = -10, .head = 8, .l_thigh = -78, .l_shin = -60, .a_l_up = -50,
      .a_l_fore = -90, .a_r_up = 44, .a_r_fore = 90, .dur_ms = 260 },
    { NEUTRAL, .torso = -11, .head = 9, .l_thigh = -82, .l_shin = -64, .a_l_up = -52,
      .a_l_fore = -92, .a_r_up = 46, .a_r_fore = 92, .dur_ms = 360 },
    { NEUTRAL, .dur_ms = 320 },
};

// 高位侧踢：整条腿抬到髋以上水平踢出，上身后仰配平。
static const app_pet_key_t k_high_kick[] = {
    { NEUTRAL, .dur_ms = 200 },
    { NEUTRAL, .torso = 12, .head = 12, .l_thigh = -110, .l_shin = -108, .a_l_up = -50,
      .a_l_fore = -100, .a_r_up = 55, .a_r_fore = 100, .dy = 1, .dur_ms = 300 },
    { NEUTRAL, .torso = 13, .head = 13, .l_thigh = -114, .l_shin = -112, .a_l_up = -52,
      .a_l_fore = -102, .a_r_up = 57, .a_r_fore = 102, .dy = 1, .dur_ms = 420 },
    { NEUTRAL, .dur_ms = 340 },
};

// 左右出拳：一只手横向打直，另一只手护在胸前，交替出拳。
static const app_pet_key_t k_punch[] = {
    { NEUTRAL, .torso = -6, .head = 4, .a_l_up = -92, .a_l_fore = -92, .a_r_up = 46,
      .a_r_fore = 118, .dur_ms = 180 },
    { NEUTRAL, .head = 0, .a_l_up = -40, .a_l_fore = 120, .a_r_up = 40, .a_r_fore = -120,
      .dur_ms = 120 },
    { NEUTRAL, .torso = 6, .head = -4, .a_l_up = 46, .a_l_fore = -118, .a_r_up = 92,
      .a_r_fore = 92, .dur_ms = 180 },
    { NEUTRAL, .head = 0, .a_l_up = -40, .a_l_fore = 120, .a_r_up = 40, .a_r_fore = -120,
      .dur_ms = 120 },
};

// 空手道劈掌：右手举过头顶蓄势，再向斜下劈出，收势回站姿。
static const app_pet_key_t k_karate[] = {
    { NEUTRAL, .dur_ms = 200 },
    { NEUTRAL, .torso = -6, .head = -8, .a_r_up = 140, .a_r_fore = 160,
      .a_l_up = -30, .a_l_fore = 90, .dur_ms = 320 },
    { NEUTRAL, .torso = 6, .head = 8, .dy = 3, .a_r_up = 78, .a_r_fore = 78,
      .a_l_up = -30, .a_l_fore = 90, .dur_ms = 160 },
    { NEUTRAL, .torso = 6, .head = 8, .dy = 3, .a_r_up = 80, .a_r_fore = 80,
      .a_l_up = -30, .a_l_fore = 90, .dur_ms = 400 },
    { NEUTRAL, .dur_ms = 300 },
};

// 白鹤亮翅：单腿站立、另一条腿提膝，双臂一高一低张开，撑住。
static const app_pet_key_t k_crane[] = {
    { NEUTRAL, .dur_ms = 260 },
    { NEUTRAL, .torso = 6, .head = 6, .l_thigh = -95, .l_shin = 30, .r_thigh = 15,
      .r_shin = 5, .a_l_up = -140, .a_l_fore = -166, .a_r_up = 34, .a_r_fore = 62, .dur_ms = 400 },
    { NEUTRAL, .torso = 6, .head = 7, .l_thigh = -98, .l_shin = 32, .r_thigh = 15,
      .r_shin = 5, .a_l_up = -142, .a_l_fore = -168, .a_r_up = 36, .a_r_fore = 64, .dur_ms = 900 },
    { NEUTRAL, .dur_ms = 380 },
};

#define KEYS(arr) arr, (int)(sizeof(arr) / sizeof((arr)[0]))

// 动作表。loop 的给一个"窗口"就能自动收尾，插值方式对所有动作统一（Hermite 样条），
// 因此这里不需要再区分"是否缓动"。
static const app_pet_action_t ACTIONS[APP_PET_ACT_COUNT] = {
    [APP_PET_ACT_IDLE]          = { "idle",          KEYS(k_idle),          true  },
    [APP_PET_ACT_LOOK]          = { "look",          KEYS(k_look),          false },
    [APP_PET_ACT_WAVE]          = { "wave",          KEYS(k_wave),          false },
    [APP_PET_ACT_JUMP]          = { "jump",          KEYS(k_jump),          false },
    [APP_PET_ACT_WALK]          = { "walk",          KEYS(k_walk),          true  },
    [APP_PET_ACT_RUN]           = { "run",           KEYS(k_run),           true  },
    [APP_PET_ACT_SIT]           = { "sit",           KEYS(k_sit),           false },
    [APP_PET_ACT_SQUAT]         = { "squat",         KEYS(k_squat),         false },
    [APP_PET_ACT_SLEEP]         = { "sleep",         KEYS(k_sleep),         false },
    [APP_PET_ACT_YAWN]          = { "yawn",          KEYS(k_yawn),          false },
    [APP_PET_ACT_STRETCH]       = { "stretch",       KEYS(k_stretch),       false },
    [APP_PET_ACT_PACE]          = { "pace",          KEYS(k_pace),          true  },
    [APP_PET_ACT_TIPTOE]        = { "tiptoe",        KEYS(k_tiptoe),        false },
    [APP_PET_ACT_TAP_FOOT]      = { "tap_foot",      KEYS(k_tap_foot),      true  },
    [APP_PET_ACT_THINK]         = { "think",         KEYS(k_think),         true  },
    [APP_PET_ACT_CHEER]         = { "cheer",         KEYS(k_cheer),         false },
    [APP_PET_ACT_VICTORY]       = { "victory",       KEYS(k_victory),       false },
    [APP_PET_ACT_LAUGH]         = { "laugh",         KEYS(k_laugh),         false },
    [APP_PET_ACT_CRY]           = { "cry",           KEYS(k_cry),           false },
    [APP_PET_ACT_ANGRY]         = { "angry",         KEYS(k_angry),         false },
    [APP_PET_ACT_FACEPALM]      = { "facepalm",      KEYS(k_facepalm),      false },
    [APP_PET_ACT_SIGH]          = { "sigh",          KEYS(k_sigh),          false },
    [APP_PET_ACT_SHRUG]         = { "shrug",         KEYS(k_shrug),         false },
    [APP_PET_ACT_SHAKE]         = { "shake",         KEYS(k_shake),         true  },
    [APP_PET_ACT_SNEEZE]        = { "sneeze",        KEYS(k_sneeze),        false },
    [APP_PET_ACT_HEAD_SHAKE]    = { "head_shake",    KEYS(k_head_shake),    false },
    [APP_PET_ACT_CLAP]          = { "clap",          KEYS(k_clap),          false },
    [APP_PET_ACT_SALUTE]        = { "salute",        KEYS(k_salute),        false },
    [APP_PET_ACT_POINT]         = { "point",         KEYS(k_point),         false },
    [APP_PET_ACT_BECKON]        = { "beckon",        KEYS(k_beckon),        false },
    [APP_PET_ACT_FLEX]          = { "flex",          KEYS(k_flex),          false },
    [APP_PET_ACT_ARMS_CROSSED]  = { "arms_crossed",  KEYS(k_arms_crossed),  false },
    [APP_PET_ACT_HANDS_ON_HIPS] = { "hands_on_hips", KEYS(k_hands_on_hips), false },
    [APP_PET_ACT_PHONE]         = { "phone",         KEYS(k_phone),         false },
    [APP_PET_ACT_DRINK]         = { "drink",         KEYS(k_drink),         false },
    [APP_PET_ACT_KNOCK]         = { "knock",         KEYS(k_knock),         false },
    [APP_PET_ACT_READ]          = { "read",          KEYS(k_read),          false },
    [APP_PET_ACT_WATCH]         = { "watch",         KEYS(k_watch),         false },
    [APP_PET_ACT_DANCE]         = { "dance",         KEYS(k_dance),         true  },
    [APP_PET_ACT_DAB]           = { "dab",           KEYS(k_dab),           false },
    [APP_PET_ACT_ROBOT]         = { "robot",         KEYS(k_robot),         true  },
    [APP_PET_ACT_DISCO]         = { "disco",         KEYS(k_disco),         true  },
    [APP_PET_ACT_MOONWALK]      = { "moonwalk",      KEYS(k_moonwalk),      true  },
    [APP_PET_ACT_JUMP_ROPE]     = { "jump_rope",     KEYS(k_jump_rope),     true  },
    [APP_PET_ACT_JUMPING_JACK]  = { "jumping_jack",  KEYS(k_jumping_jack),  true  },
    [APP_PET_ACT_HULA]          = { "hula",          KEYS(k_hula),          true  },
    [APP_PET_ACT_KICK]          = { "kick",          KEYS(k_kick),          false },
    [APP_PET_ACT_HIGH_KICK]     = { "high_kick",     KEYS(k_high_kick),     false },
    [APP_PET_ACT_PUNCH]         = { "punch",         KEYS(k_punch),         true  },
    [APP_PET_ACT_KARATE]        = { "karate",        KEYS(k_karate),        false },
    [APP_PET_ACT_CRANE]         = { "crane",         KEYS(k_crane),         false },
};

#undef KEYS

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
// 每个池子都放得足够宽（十来个），再配合"不连着做同一个动作"的规则，避免桌宠在
// 几分钟里翻来覆去只演那两三个动作。
static int pick_idle_action(app_pet_t *p)
{
    static const int calm[] = {
        APP_PET_ACT_LOOK, APP_PET_ACT_WAVE, APP_PET_ACT_WALK, APP_PET_ACT_RUN,
        APP_PET_ACT_STRETCH, APP_PET_ACT_JUMP, APP_PET_ACT_DANCE, APP_PET_ACT_SIT,
        APP_PET_ACT_SQUAT, APP_PET_ACT_THINK, APP_PET_ACT_PACE, APP_PET_ACT_TIPTOE,
        APP_PET_ACT_HANDS_ON_HIPS, APP_PET_ACT_ARMS_CROSSED, APP_PET_ACT_READ,
        APP_PET_ACT_KNOCK, APP_PET_ACT_POINT, APP_PET_ACT_ROBOT, APP_PET_ACT_DISCO,
        APP_PET_ACT_HULA, APP_PET_ACT_YAWN, APP_PET_ACT_HEAD_SHAKE, APP_PET_ACT_CLAP,
        APP_PET_ACT_SALUTE, APP_PET_ACT_PHONE, APP_PET_ACT_DRINK, APP_PET_ACT_SNEEZE,
        APP_PET_ACT_SIGH, APP_PET_ACT_SHRUG, APP_PET_ACT_CRANE, APP_PET_ACT_JUMPING_JACK,
    };
    static const int happy[] = {
        APP_PET_ACT_WAVE, APP_PET_ACT_JUMP, APP_PET_ACT_DANCE, APP_PET_ACT_CHEER,
        APP_PET_ACT_CLAP, APP_PET_ACT_VICTORY, APP_PET_ACT_LAUGH, APP_PET_ACT_DISCO,
        APP_PET_ACT_ROBOT, APP_PET_ACT_DAB, APP_PET_ACT_FLEX, APP_PET_ACT_BECKON,
        APP_PET_ACT_HIGH_KICK, APP_PET_ACT_MOONWALK, APP_PET_ACT_JUMP_ROPE,
        APP_PET_ACT_JUMPING_JACK,
    };
    static const int focus[] = {
        APP_PET_ACT_THINK, APP_PET_ACT_READ, APP_PET_ACT_WATCH, APP_PET_ACT_ARMS_CROSSED,
        APP_PET_ACT_HANDS_ON_HIPS, APP_PET_ACT_SIT, APP_PET_ACT_LOOK, APP_PET_ACT_TAP_FOOT,
    };
    static const int tired[] = {
        APP_PET_ACT_SIT, APP_PET_ACT_SLEEP, APP_PET_ACT_YAWN, APP_PET_ACT_STRETCH,
        APP_PET_ACT_SIGH, APP_PET_ACT_SHAKE, APP_PET_ACT_LOOK, APP_PET_ACT_SQUAT,
    };
    static const int angry[] = {
        APP_PET_ACT_ANGRY, APP_PET_ACT_FACEPALM, APP_PET_ACT_KICK, APP_PET_ACT_PUNCH,
        APP_PET_ACT_SHAKE, APP_PET_ACT_TAP_FOOT, APP_PET_ACT_HEAD_SHAKE,
        APP_PET_ACT_ARMS_CROSSED, APP_PET_ACT_KARATE,
    };
    static const int surprised[] = {
        APP_PET_ACT_LOOK, APP_PET_ACT_JUMP, APP_PET_ACT_STRETCH, APP_PET_ACT_POINT,
        APP_PET_ACT_HEAD_SHAKE, APP_PET_ACT_SHAKE, APP_PET_ACT_WAVE, APP_PET_ACT_SNEEZE,
    };

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

    int pick = pool[app_pet_rand(p) % (uint32_t)n];
    // 抽中刚做过的那一个就换一个：连续重复同一个动作比"动作少"更让人出戏。
    if (pick == p->last_pick && n > 1) {
        pick = pool[app_pet_rand(p) % (uint32_t)n];
    }
    p->last_pick = pick;
    return pick;
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
    p->last_pick = -1;
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
        static const int pool[] = {
            APP_PET_ACT_DANCE, APP_PET_ACT_DAB, APP_PET_ACT_ROBOT, APP_PET_ACT_DISCO,
            APP_PET_ACT_MOONWALK, APP_PET_ACT_PUNCH, APP_PET_ACT_HIGH_KICK,
            APP_PET_ACT_KARATE, APP_PET_ACT_FLEX, APP_PET_ACT_VICTORY,
            APP_PET_ACT_FACEPALM, APP_PET_ACT_HEAD_SHAKE, APP_PET_ACT_SNEEZE,
            APP_PET_ACT_HULA, APP_PET_ACT_JUMP_ROPE, APP_PET_ACT_JUMPING_JACK,
        };
        static const char *const lines[] = {
            "看我的", "嘿嘿", "走你", "哼唧", "啪!", "哈!", "来呀", "不服?",
            "哼哼", "耶!", "无语了", "不不不", "阿嚏!", "扭起来", "跳一个", "我跳!",
        };
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

// 三次 Hermite 插值（Catmull-Rom）：p1、p2 是这一段的两个关键帧，m1、m2 是它们处的
// 切线（单位与"整段的位移"同量纲，Q0），num/den 是段内进度。返回 p1→p2 之间任意位置的值。
//
// 为什么不用更简单的两种做法：
//  * 线性：每个关键帧处速度突变，走、跑的循环在关键帧上会"拐一下"，看着发顿；
//  * 分段缓动（smoothstep）：每个关键帧速度归零，于是跳跃在半空中会"停一下"、
//    挥手的往返之间会一顿。两种都做不到"丝滑"，而 Hermite 在关键帧处速度连续。
// 切线由相邻两帧的差分估计（duration 不均匀时按两段时长加权），所以一次性的
// "起势—到位—过冲—回收"和循环动作的连续摆动用的是同一套数学。
//
// 全部定点整数：ESP32-C3 没有硬件浮点，这里只用 int64 乘加，结果完全确定、便于断言。
//
// 切线不是直接取中心差分，而是再过一道"单调性限制"（见 spline_tangent）。原因：
// 裸的 Catmull-Rom 在"前一段大幅移动、本段几乎不动"的接缝处切线会非常大，曲线会先冲
// 出去再拐回来——实测伸懒腰的手臂会从 150° 甩到 171°，直接穿进脑袋；而这类"到位后
// 保持"的接缝在动作库里到处都是。限制之后曲线永远不会越出两端关键帧的取值，
// 于是"关键帧都在画布内"就自动保证了"整个动作都在画布内"。
static int32_t spline_tangent(int16_t vm, int16_t v0, int16_t v1, int dprev, int dcur,
                              bool has_prev, bool has_next)
{
    if (!has_prev || !has_next) return 0;      // 非循环动作的首尾：切线为零 = 自然起势/收势

    const int32_t dp = (int32_t)v0 - vm;       // 前一段的总变化
    const int32_t dn = (int32_t)v1 - v0;       // 本段的总变化
    if (dp == 0 || dn == 0) return 0;          // 有一侧是"停住"的，切线取零
    if ((dp > 0) != (dn > 0)) return 0;        // 局部极值点，切线取零（速度在此换向）

    int32_t m = (int32_t)((int64_t)(v1 - vm) * dcur / (dprev + dcur));
    int32_t lim = dp < 0 ? -dp : dp;
    const int32_t adn = dn < 0 ? -dn : dn;
    if (adn < lim) lim = adn;
    lim *= 3;                                  // 经典单调性上界：不超过三倍的较小变化量
    if (m > lim) m = lim;
    if (m < -lim) m = -lim;
    return m;
}

static int16_t hermite_i16(int16_t p1, int16_t p2, int32_t m1, int32_t m2, int den, int num)
{
    if (den <= 0 || num <= 0) return p1;
    if (num >= den) return p2;

    const int32_t Q = 65536;
    const int32_t u = (int32_t)((int64_t)num * Q / den);   // 段内归一化进度，Q16
    const int32_t u2 = (int32_t)(((int64_t)u * u) >> 16);
    const int32_t u3 = (int32_t)(((int64_t)u2 * u) >> 16);

    // 三次 Hermite 基函数：h00 = 2u³-3u²+1, h10 = u³-2u²+u, h01 = -2u³+3u², h11 = u³-u²
    const int32_t h00 = 2 * u3 - 3 * u2 + Q;
    const int32_t h10 = u3 - 2 * u2 + u;
    const int32_t h01 = -2 * u3 + 3 * u2;
    const int32_t h11 = u3 - u2;

    int64_t v = (int64_t)h00 * p1 + (int64_t)h10 * m1 +
                (int64_t)h01 * p2 + (int64_t)h11 * m2;
    return (int16_t)(v >> 16);
}

void app_pet_pose_at(const app_pet_t *p, uint64_t now_ms, app_pet_pose_t *out)
{
    if (!out) return;

    int action = p ? p->action : APP_PET_ACT_IDLE;
    const app_pet_action_t *a = action_of(action);
    const int n = a->key_count;
    uint64_t start = p ? p->action_start_ms : 0;

    uint64_t elapsed = (now_ms > start) ? (now_ms - start) : 0;
    uint64_t total = action_total_ms(action);
    if (a->loop && total) elapsed %= total;

    if (n < 2) {   // 单帧动作（理论上不存在）：直接就是那一帧
        const app_pet_key_t *k = &a->keys[n > 0 ? 0 : 0];
        out->torso = k->torso; out->head = k->head;
        out->arm_l_up = k->a_l_up; out->arm_l_fore = k->a_l_fore;
        out->arm_r_up = k->a_r_up; out->arm_r_fore = k->a_r_fore;
        out->leg_l_thigh = k->l_thigh; out->leg_l_shin = k->l_shin;
        out->leg_r_thigh = k->r_thigh; out->leg_r_shin = k->r_shin;
        out->root_dx = k->dx; out->root_dy = k->dy;
        return;
    }

    // 定位当前段：段 i 从关键帧 i 走到 i+1，时长关键帧 i 的 dur_ms。循环动作的最后一帧
    // 绕回首帧；非循环动作走到头就停在最后一帧（把 num 取满，等价于落在段终点）。
    int i = n - 2;
    int num, den;
    uint64_t acc = 0;
    for (int k = 0; k < n; k++) {
        uint32_t d = a->keys[k].dur_ms;
        if (elapsed < acc + d) {
            i = k;
            num = (int)(elapsed - acc);
            den = d ? (int)d : 1;
            goto located;
        }
        acc += d;
    }
    num = den = a->keys[n - 2].dur_ms ? (int)a->keys[n - 2].dur_ms : 1;
located:
    ;

    const app_pet_key_t *k1 = &a->keys[i];
    const app_pet_key_t *k2 = &a->keys[(i + 1) % n];
    // 前一帧 / 后两帧只用来估切线；非循环动作的首尾没有邻居，切线取 0。
    const bool has_prev = a->loop || i > 0;
    const bool has_next = a->loop || i + 2 < n;
    const app_pet_key_t *km = has_prev ? &a->keys[(i - 1 + n) % n] : k1;
    const app_pet_key_t *kn = has_next ? &a->keys[(i + 2) % n] : k2;
    const int dprev = has_prev ? (int)a->keys[(i - 1 + n) % n].dur_ms : 0;
    const int dnext = (int)a->keys[(i + 1) % n].dur_ms;

    // 12 个通道做同一套插值，用宏写出 12 行，而不是把同一段话抄 12 遍。
    // 每个关键帧处的切线只算一次（m1 也是上一段的 m2），所以关键帧处速度连续。
#define PET_SPLINE(pose_f, key_f)                                                   \
    do {                                                                            \
        const int16_t v1 = k1->key_f, v2 = k2->key_f;                               \
        const int32_t t1 = spline_tangent(km->key_f, v1, v2, dprev, den, has_prev, true); \
        const int32_t t2 = spline_tangent(v1, v2, kn->key_f, den, dnext, true, has_next); \
        out->pose_f = hermite_i16(v1, v2, t1, t2, den, num);                        \
    } while (0)

    PET_SPLINE(torso, torso);
    PET_SPLINE(head, head);
    PET_SPLINE(arm_l_up, a_l_up);
    PET_SPLINE(arm_l_fore, a_l_fore);
    PET_SPLINE(arm_r_up, a_r_up);
    PET_SPLINE(arm_r_fore, a_r_fore);
    PET_SPLINE(leg_l_thigh, l_thigh);
    PET_SPLINE(leg_l_shin, l_shin);
    PET_SPLINE(leg_r_thigh, r_thigh);
    PET_SPLINE(leg_r_shin, r_shin);
    PET_SPLINE(root_dx, dx);
    PET_SPLINE(root_dy, dy);

#undef PET_SPLINE
}

// ---------------------------------------------------------------------------
// 骨架落点
// ---------------------------------------------------------------------------
// 各段长度按画布高度取百分比：躯干 20%、大臂 13%、小臂 11%、大腿/小腿各 15%、头半径 6%。
// 站姿髋部落在 60% 处，于是脚底约在 90%、头顶约在 24%：上下各留出约一成余量，
// 双臂高举（欢呼/伸懒腰）与起跳、坐下都不会被画布裁掉。比例刻意偏"头小、腿长、脖子
// 露得出来"——头太大、脖子被肩膀吃掉时，横举的手臂会从下巴位置划过，看着像把头劈开，
// 正是 xkcd / Alan Becker 那类火柴人极力避免的。
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
    int thigh_len = h * 15 / 100;
    int shin_len  = h * 15 / 100;
    int head_r    = h * 6 / 100;
    int neck_len  = h * 4 / 100;

    int hip_x = w / 2 + (int)pose->root_dx * h / 100;
    int hip_y = h * 60 / 100 + (int)pose->root_dy * h / 100;

    // 躯干由髋向上（len 取负即反向），肩在躯干末端。
    int sh_x = limb_x(hip_x, pose->torso, -torso_len);
    int sh_y = limb_y(hip_y, pose->torso, -torso_len);
    // 头继续沿"躯干 + 头部偏角"的方向向上，隔一个脖子长度。
    int head_ang = pose->torso + pose->head;
    int hd_x = limb_x(sh_x, head_ang, -(neck_len + head_r));
    int hd_y = limb_y(sh_y, head_ang, -(neck_len + head_r));

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
