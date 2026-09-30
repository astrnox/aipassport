// main/logic/app_pet.h —— 桌面火柴人纯逻辑：动作库、姿态插值、骨架落点、情绪与台词。
//
// 为什么不把桌宠直接写进界面：桌宠真正的复杂处不在画线，而在"什么时候该做什么"——
// 空闲多久做一个随机动作、番茄钟结束时欢呼、低电量时发抖、捣乱模式多久冒一次头。
// 这些都是状态机与时序，和 LVGL 无关。把它们留在这里，就能在一台普通电脑上把
// "番茄钟结束"喂进去、断言它确实切到了欢呼动作，而不必真的把设备摆够 25 分钟。
//
// 数据流：事件/时间 -> app_pet_tick() 选动作 -> app_pet_pose_at() 插值出姿态
//        -> app_pet_skeleton() 落到像素坐标 -> 界面只负责把点连成线。
//
// 姿态约定（与界面无关，纯几何）：
//  - 每个角度单位为"度"，0 表示朝下，90 朝右，180 朝上，-90（或 270）朝左。
//    手臂垂下≈0，举到侧面≈±90，举过头≈±170。这样写关键帧时一眼能判断方向。
//  - root_dx/root_dy 是根节点（髋部）相对画布中心的偏移，单位是"火柴人高度的百分比"，
//    用来表现下蹲（正）、起跳（负）与左右挪步。
// 所有角度与偏移都是 int16，插值只做整数线性插值，结果完全确定，便于主机断言。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 动作表。枚举值即动作 id，动作名字符串只用于调试与测试断言。
//
// 分四类便于挑选与维护：
//   基础/日常 —— 站立与常见姿态；
//   情绪表达 —— 喜怒哀乐，多半是非循环的一次性动作；
//   手势/社交 —— 明确的"肢体对白"，读起来像在和用户说话；
//   运动/舞蹈 —— 循环动作，幅度大、存在感强。
typedef enum {
    // 基础与日常
    APP_PET_ACT_IDLE = 0,   // 站着轻微呼吸（默认）
    APP_PET_ACT_LOOK,       // 左右张望
    APP_PET_ACT_WAVE,       // 挥手打招呼
    APP_PET_ACT_JUMP,       // 原地跳一下
    APP_PET_ACT_WALK,       // 原地踏步
    APP_PET_ACT_RUN,        // 原地小跑
    APP_PET_ACT_SIT,        // 抱膝坐下
    APP_PET_ACT_SQUAT,      // 下蹲起立
    APP_PET_ACT_SLEEP,      // 打盹
    APP_PET_ACT_YAWN,       // 打哈欠
    APP_PET_ACT_STRETCH,    // 伸懒腰
    APP_PET_ACT_PACE,       // 来回踱步
    APP_PET_ACT_TIPTOE,     // 踮脚张望
    APP_PET_ACT_TAP_FOOT,   // 抖腿（不耐烦）
    // 情绪表达
    APP_PET_ACT_THINK,      // 托腮思考
    APP_PET_ACT_CHEER,      // 欢呼
    APP_PET_ACT_VICTORY,    // 双拳上举的胜利姿势
    APP_PET_ACT_LAUGH,      // 捧腹大笑
    APP_PET_ACT_CRY,        // 揉眼哭
    APP_PET_ACT_ANGRY,      // 生气跺脚
    APP_PET_ACT_FACEPALM,   // 捂脸
    APP_PET_ACT_SIGH,       // 长叹一口气
    APP_PET_ACT_SHRUG,      // 耸肩摊手
    APP_PET_ACT_SHAKE,      // 发抖
    APP_PET_ACT_SNEEZE,     // 打喷嚏
    APP_PET_ACT_HEAD_SHAKE, // 摇头
    // 手势与社交
    APP_PET_ACT_CLAP,       // 鼓掌
    APP_PET_ACT_SALUTE,     // 敬礼
    APP_PET_ACT_POINT,      // 抬手比划
    APP_PET_ACT_BECKON,     // 招手叫过来
    APP_PET_ACT_FLEX,       // 秀肌肉
    APP_PET_ACT_ARMS_CROSSED, // 抱臂
    APP_PET_ACT_HANDS_ON_HIPS, // 叉腰
    APP_PET_ACT_PHONE,      // 打电话
    APP_PET_ACT_DRINK,      // 喝水
    APP_PET_ACT_KNOCK,      // 敲门
    APP_PET_ACT_READ,       // 低头看书
    APP_PET_ACT_WATCH,      // 抬手看表
    // 舞蹈
    APP_PET_ACT_DANCE,      // 摇摆舞（"跳鸡舞"）
    APP_PET_ACT_DAB,        // dab
    APP_PET_ACT_ROBOT,      // 机械舞
    APP_PET_ACT_DISCO,      // 迪斯科指天
    APP_PET_ACT_MOONWALK,   // 太空步
    // 运动与格斗
    APP_PET_ACT_JUMP_ROPE,  // 跳绳
    APP_PET_ACT_JUMPING_JACK, // 开合跳
    APP_PET_ACT_HULA,       // 扭胯
    APP_PET_ACT_KICK,       // 踢腿
    APP_PET_ACT_HIGH_KICK,  // 高位侧踢
    APP_PET_ACT_PUNCH,      // 左右出拳
    APP_PET_ACT_KARATE,     // 空手道劈掌
    APP_PET_ACT_CRANE,      // 白鹤亮翅
    APP_PET_ACT_COUNT
} app_pet_action_id_t;

// 情绪：只影响"空闲时更倾向做什么"和表情文字，不直接决定动作。
typedef enum {
    APP_PET_MOOD_CALM = 0,
    APP_PET_MOOD_HAPPY,
    APP_PET_MOOD_FOCUS,
    APP_PET_MOOD_TIRED,
    APP_PET_MOOD_ANGRY,
    APP_PET_MOOD_SURPRISED,
} app_pet_mood_t;

// 外部事件。页面/系统把"发生了什么"告诉桌宠，由它自己决定怎么表现。
typedef enum {
    APP_PET_EV_FOCUS_ON = 0,     // 番茄钟进入专注段
    APP_PET_EV_FOCUS_DONE,       // 专注结束（该休息了）
    APP_PET_EV_BREAK_DONE,       // 休息结束（该干活了）
    APP_PET_EV_TIMER_DONE,       // 计时器/闹钟到点
    APP_PET_EV_REMINDER,         // 日程提醒
    APP_PET_EV_LOW_BATTERY,      // 低电量
    APP_PET_EV_FOUND_DEVICE,     // 蓝牙查找器收藏了设备
    APP_PET_EV_TRACKER,          // 追踪到防丢器
    APP_PET_EV_COUNT
} app_pet_event_t;

// 姿态：10 个绝对角度 + 根偏移。全部由关键帧插值得到。
// head 是头部相对躯干方向的偏角：0 表示头随躯干（正常），正/负表示头向左右歪，
// 用来表达"张望/歪头/低头"，这是没有五官的火柴人唯一能读出"在看"的通道。
typedef struct {
    int16_t torso;
    int16_t head;
    int16_t arm_l_up, arm_l_fore;
    int16_t arm_r_up, arm_r_fore;
    int16_t leg_l_thigh, leg_l_shin;
    int16_t leg_r_thigh, leg_r_shin;
    int16_t root_dx, root_dy;
} app_pet_pose_t;

// 骨架落点（像素，画布左上为原点）。界面只按顺序连线，不再做任何几何计算。
typedef struct {
    int16_t hip_x, hip_y;
    int16_t shoulder_x, shoulder_y;
    int16_t head_x, head_y;
    int16_t elbow_l_x, elbow_l_y, hand_l_x, hand_l_y;
    int16_t elbow_r_x, elbow_r_y, hand_r_x, hand_r_y;
    int16_t knee_l_x, knee_l_y, foot_l_x, foot_l_y;
    int16_t knee_r_x, knee_r_y, foot_r_x, foot_r_y;
    int16_t head_r;   // 头部半径，供界面画圆
} app_pet_skeleton_t;

// 运行态。不含指针以外的大数据，放静态区即可。
typedef struct {
    uint32_t rng;

    bool           mischief;      // 捣乱模式
    app_pet_mood_t mood;
    uint64_t       mood_until_ms; // 情绪回落到平静的时刻

    int      action;
    uint64_t action_start_ms;
    uint64_t action_dur_ms;    // 本次动作名义总时长（非循环动作按它判结束）
    uint64_t next_idle_ms;     // 下次自发随机动作的时刻
    uint64_t next_mischief_ms; // 捣乱模式下下次"冒头"的时刻
    int      last_pick;        // 上一次自发选中的动作，用来避免连着做同一个动作

    const char *speech;          // 当前台词（静态字符串），可为 NULL
    uint64_t    speech_until_ms;

    uint64_t last_tick_ms;
} app_pet_t;

// 初始化。seed 相同则随机序列完全一致，便于测试。
void app_pet_init(app_pet_t *p, uint32_t seed);
void app_pet_set_mischief(app_pet_t *p, bool on);
bool app_pet_mischief(const app_pet_t *p);

// 外部事件。会切换动作、情绪与台词；正在进行的自发动作会被打断。
void app_pet_trigger(app_pet_t *p, app_pet_event_t ev, uint64_t now_ms);

// 按时间推进：结束到点的一次性动作、安排下一次自发动作、打理捣乱模式与情绪回落。
void app_pet_tick(app_pet_t *p, uint64_t now_ms);

int            app_pet_action(const app_pet_t *p);
app_pet_mood_t app_pet_mood(const app_pet_t *p);
// 当前台词；没有台词返回 NULL。now_ms 用于判断台词是否过期。
const char *app_pet_speech(const app_pet_t *p, uint64_t now_ms);

// 当前动作的姿态（按时间插值）。p 为 NULL 时给出站立姿态。
void app_pet_pose_at(const app_pet_t *p, uint64_t now_ms, app_pet_pose_t *out);
// 把姿态落到 w×h 画布上的像素落点。不裁剪、不夹取，越界由界面层裁剪。
void app_pet_skeleton(const app_pet_pose_t *pose, int w, int h, app_pet_skeleton_t *out);

// 调试/测试辅助。
const char   *app_pet_action_name(int action);
bool          app_pet_action_loops(int action);
// 下一次随机数（xorshift32）。暴露以便界面/测试复用同一条随机序列。
uint32_t      app_pet_rand(app_pet_t *p);
// 定点正弦：返回 sin(deg)*65536，deg 任意整数。用于把角度转成像素方向。
int32_t       app_pet_sin_q16(int deg);