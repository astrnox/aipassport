// tests/test_app_pet.c —— 桌宠纯逻辑的主机侧单元测试。
//
// 覆盖：定点正弦、姿态插值与循环取模、骨架几何（站姿朝上/朝下判定）、事件→动作/情绪/台词、
// 一次性动作到期回站立、自发动作与捣乱模式的调度、随机序列的确定性。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "logic/app_pet.h"

int main(void)
{
    // ---- 定点正弦 ----
    assert(app_pet_sin_q16(0) == 0);
    assert(app_pet_sin_q16(90) == 65536);          // sin90 = 1
    assert(app_pet_sin_q16(30) == 32768);          // sin30 = 0.5
    assert(app_pet_sin_q16(180) == 0);
    assert(app_pet_sin_q16(270) == -65536);        // sin270 = -1
    assert(app_pet_sin_q16(-90) == -65536);        // 负数角度归一化后仍正确
    assert(app_pet_sin_q16(450) == 65536);         // 超出一圈做取模

    // ---- 随机序列确定性 ----
    app_pet_t p1, p2;
    app_pet_init(&p1, 12345);
    app_pet_init(&p2, 12345);
    for (int i = 0; i < 8; i++) assert(app_pet_rand(&p1) == app_pet_rand(&p2));
    app_pet_t p3;
    app_pet_init(&p3, 999);
    assert(app_pet_rand(&p3) != app_pet_rand(&p2));   // 不同种子序列不同

    // ---- 初始状态：站立、平静、无台词 ----
    app_pet_t pet;
    app_pet_init(&pet, 1);
    assert(app_pet_action(&pet) == APP_PET_ACT_IDLE);
    assert(app_pet_mood(&pet) == APP_PET_MOOD_CALM);
    assert(app_pet_speech(&pet, 0) == NULL);
    assert(app_pet_mischief(&pet) == false);

    // ---- 站姿骨架：肩在髋上方、头在肩上方、脚在髋下方 ----
    app_pet_pose_t pose;
    app_pet_init(&pet, 1);
    app_pet_pose_at(&pet, 0, &pose);
    app_pet_skeleton_t sk;
    app_pet_skeleton(&pose, 96, 96, &sk);
    assert(sk.shoulder_y < sk.hip_y);               // 屏幕坐标 y 向下，肩应当更小
    assert(sk.head_y < sk.shoulder_y);
    assert(sk.foot_l_y > sk.hip_y);
    assert(sk.foot_r_y > sk.hip_y);
    assert(sk.head_r > 0);
    // 站姿左右大致对称：两脚到中线的距离接近。
    int cl = 96 / 2;
    int dl = cl - sk.foot_l_x;
    int dr = sk.foot_r_x - cl;
    assert(dl > 0 && dr > 0);
    assert((dl > dr ? dl - dr : dr - dl) <= 8);

    // 站姿不能"糊成一团"：两条腿之间、两只手到躯干中线之间都要留出明显的空隙。
    // 之前站姿的腿只差 1~2px、手臂只差 3px，2px 线宽下会并成一根粗线，远看就是
    // 用户说的"有些部位叠起来了"。这条断言把"够不够开"钉死在测试里。
    assert(sk.foot_r_x - sk.foot_l_x >= 5);
    assert(sk.hand_r_x - cl >= 4);
    assert(cl - sk.hand_l_x >= 4);

    // ---- NULL 入参不崩，且给出站立姿态 ----
    app_pet_pose_at(NULL, 0, &pose);
    assert(pose.torso == 0);
    app_pet_skeleton(NULL, 40, 40, &sk);
    app_pet_skeleton(&pose, 40, 40, NULL);          // 允许 out 为 NULL

    // ---- 姿态插值：挥手动作中途右臂应当抬起 ----
    // 手动构造一个处于挥手动作中段的宠物，避免依赖内部时间调度。
    app_pet_t wave;
    app_pet_init(&wave, 7);
    app_pet_trigger(&wave, APP_PET_EV_REMINDER, 0);   // 挥手
    assert(app_pet_action(&wave) == APP_PET_ACT_WAVE);
    app_pet_pose_at(&wave, 0, &pose);                 // 动作起点 = 站姿
    assert(pose.arm_r_up <= 30);
    app_pet_pose_at(&wave, 500, &pose);               // 中途 = 右臂举起
    assert(pose.arm_r_up > 100);

    // ---- 循环动作的时间取模：跨过一整圈后回到同一姿态 ----
    app_pet_t dance;
    app_pet_init(&dance, 3);
    app_pet_set_mischief(&dance, true);
    // 直接借抖动动作（循环）验证取模：同一相位、相差整圈，姿态应一致。
    app_pet_pose_t a, b;
    app_pet_init(&dance, 3);
    dance.action = APP_PET_ACT_SHAKE;
    dance.action_start_ms = 0;
    app_pet_pose_at(&dance, 10, &a);
    app_pet_pose_at(&dance, 10 + 220, &b);            // 抖动一圈 220ms
    assert(a.torso == b.torso);
    assert(a.arm_l_up == b.arm_l_up);

    // ---- 事件：番茄钟结束 → 欢呼 + 开心 + 台词 ----
    app_pet_t ev;
    app_pet_init(&ev, 5);
    app_pet_trigger(&ev, APP_PET_EV_FOCUS_DONE, 1000);
    assert(app_pet_action(&ev) == APP_PET_ACT_CHEER);
    assert(app_pet_mood(&ev) == APP_PET_MOOD_HAPPY);
    assert(app_pet_speech(&ev, 1500) != NULL);
    assert(strcmp(app_pet_speech(&ev, 1500), "(*^_^*)") == 0);
    assert(app_pet_speech(&ev, 1000 + 99999) == NULL);   // 台词会过期

    // 低电量 → 发抖 + 疲惫。
    app_pet_trigger(&ev, APP_PET_EV_LOW_BATTERY, 2000);
    assert(app_pet_action(&ev) == APP_PET_ACT_SHAKE);
    assert(app_pet_mood(&ev) == APP_PET_MOOD_TIRED);

    // 发现追踪器 → 惊讶。
    app_pet_trigger(&ev, APP_PET_EV_TRACKER, 3000);
    assert(app_pet_action(&ev) == APP_PET_ACT_JUMP);
    assert(app_pet_mood(&ev) == APP_PET_MOOD_SURPRISED);

    // 越界事件不改变状态、不崩。
    int before = app_pet_action(&ev);
    app_pet_trigger(&ev, (app_pet_event_t)999, 3100);
    assert(app_pet_action(&ev) == before);
    app_pet_trigger(NULL, APP_PET_EV_FOCUS_DONE, 0);   // NULL 安全

    // ---- 情绪过期回落 ----
    app_pet_t mood;
    app_pet_init(&mood, 11);
    app_pet_trigger(&mood, APP_PET_EV_TRACKER, 0);       // 惊讶，约 12s
    assert(app_pet_mood(&mood) == APP_PET_MOOD_SURPRISED);
    app_pet_tick(&mood, 60000);
    assert(app_pet_mood(&mood) == APP_PET_MOOD_CALM);

    // ---- 一次性动作到期回站立 ----
    app_pet_t once;
    app_pet_init(&once, 13);
    app_pet_trigger(&once, APP_PET_EV_FOCUS_DONE, 0);    // CHEER
    assert(app_pet_action(&once) == APP_PET_ACT_CHEER);
    app_pet_tick(&once, 100);                             // 还没结束
    assert(app_pet_action(&once) == APP_PET_ACT_CHEER);
    app_pet_tick(&once, 100000);                          // 早已结束
    assert(app_pet_action(&once) == APP_PET_ACT_IDLE);

    // ---- 自发动作：到点后会离开站立 ----
    app_pet_t idle;
    app_pet_init(&idle, 21);
    uint64_t t = 0;
    bool left_idle = false;
    for (int i = 0; i < 400; i++) {                       // 模拟 40 秒，每拍 100ms
        t += 100;
        app_pet_tick(&idle, t);
        if (app_pet_action(&idle) != APP_PET_ACT_IDLE) left_idle = true;
    }
    assert(left_idle == true);

    // ---- 捣乱模式：开启后会更频繁地冒头，且动作来自捣乱动作池 ----
    app_pet_t mis;
    app_pet_init(&mis, 31);
    app_pet_set_mischief(&mis, true);
    assert(app_pet_mischief(&mis) == true);
    t = 0;
    bool saw_mischief = false;
    for (int i = 0; i < 800; i++) {                       // 模拟 80 秒
        t += 100;
        app_pet_tick(&mis, t);
        int act = app_pet_action(&mis);
        // 捣乱动作池里的动作都要"有存在感"：要么是大幅度的舞蹈/格斗动作，要么是
        // 明确的情绪表达。这里只断言"确实来自这个池子"，不锁死具体是哪几个。
        switch (act) {
        case APP_PET_ACT_DANCE: case APP_PET_ACT_DAB: case APP_PET_ACT_ROBOT:
        case APP_PET_ACT_DISCO: case APP_PET_ACT_MOONWALK: case APP_PET_ACT_PUNCH:
        case APP_PET_ACT_HIGH_KICK: case APP_PET_ACT_KARATE: case APP_PET_ACT_FLEX:
        case APP_PET_ACT_VICTORY: case APP_PET_ACT_FACEPALM: case APP_PET_ACT_HEAD_SHAKE:
        case APP_PET_ACT_SNEEZE: case APP_PET_ACT_HULA: case APP_PET_ACT_JUMP_ROPE:
        case APP_PET_ACT_JUMPING_JACK:
            saw_mischief = true;
            break;
        default:
            break;
        }
    }
    assert(saw_mischief == true);
    app_pet_set_mischief(&mis, false);
    assert(app_pet_mischief(&mis) == false);

    // ---- 时间倒退不产生荒谬姿态（回绕对齐） ----
    app_pet_t back;
    app_pet_init(&back, 41);
    app_pet_tick(&back, 100000);
    app_pet_tick(&back, 500);                             // 时间倒退
    app_pet_pose_at(&back, 500, &pose);                   // 不应崩溃或越界
    assert(pose.root_dy >= -60 && pose.root_dy <= 60);

    // ---- 动作名与循环标记 ----
    assert(strcmp(app_pet_action_name(APP_PET_ACT_DANCE), "dance") == 0);
    assert(strcmp(app_pet_action_name(APP_PET_ACT_IDLE), "idle") == 0);
    assert(strcmp(app_pet_action_name(999), "idle") == 0);      // 越界回站立
    assert(app_pet_action_loops(APP_PET_ACT_IDLE) == true);
    assert(app_pet_action_loops(APP_PET_ACT_DANCE) == true);
    assert(app_pet_action_loops(APP_PET_ACT_WAVE) == false);

    // ---- 动作表完整：每个 id 都有名字，新动作都在表里 ----
    for (int id = 0; id < APP_PET_ACT_COUNT; id++) {
        const char *n = app_pet_action_name(id);
        assert(n && *n);
    }
    assert(strcmp(app_pet_action_name(APP_PET_ACT_ROBOT), "robot") == 0);
    assert(strcmp(app_pet_action_name(APP_PET_ACT_JUMPING_JACK), "jumping_jack") == 0);
    assert(strcmp(app_pet_action_name(APP_PET_ACT_CRANE), "crane") == 0);
    assert(app_pet_action_loops(APP_PET_ACT_ROBOT) == true);
    assert(app_pet_action_loops(APP_PET_ACT_HIGH_KICK) == false);

    // ---- 头部偏角真的会带动头的位置：没有五官的火柴人，"张望"全靠它 ----
    app_pet_t lk;
    app_pet_init(&lk, 3);
    lk.action = APP_PET_ACT_LOOK;
    lk.action_start_ms = 0;
    app_pet_pose_t p_look;
    app_pet_skeleton_t sk0, sk1;
    app_pet_pose_at(&lk, 0, &pose);
    app_pet_skeleton(&pose, 96, 96, &sk0);
    app_pet_pose_at(&lk, 900, &p_look);
    app_pet_skeleton(&p_look, 96, 96, &sk1);
    assert(p_look.head != 0);
    assert(sk1.head_x != sk0.head_x);

    // ---- 关键帧是"经过点"：t 正好落在关键帧时刻时，姿态必须精确等于那一帧 ----
    // Hermite 样条的好处是既穿过每个关键帧、关键帧处速度又连续；这条断言把"穿过"钉住，
    // 一旦插值权重写错（例如把 h01 和 h00 弄混），动作会在关键帧之间漂移，这里立刻暴露。
    app_pet_t ez;
    app_pet_init(&ez, 7);
    ez.action = APP_PET_ACT_WAVE;
    ez.action_start_ms = 0;
    app_pet_pose_t pe;
    app_pet_pose_at(&ez, 0, &pe);
    assert(pe.arm_r_up == 24);                        // 首帧 = 站姿
    app_pet_pose_at(&ez, 280, &pe);                   // 第二帧
    assert(pe.arm_r_up == 128);
    app_pet_pose_at(&ez, 580, &pe);                   // 第三帧
    assert(pe.arm_r_up == 142);

    // ---- 插值是样条，不是"每段各自缓一下" ----
    // 分段缓动（smoothstep）在中点恰好等于两端的线性中点（左右对称），而 Hermite 样条
    // 因为两端切线不同、中点会偏离线性中点。这条断言能区分两种实现：如果有人把插值换回
    // "每段缓入缓出"，跳跃就会在半空中停一下，这里会失败。
    app_pet_pose_at(&ez, 140, &pe);                   // 首段（0..280，24°→128°）的中点
    assert(pe.arm_r_up != (24 + 128) / 2);

    // ---- 画布裁切不变量：逐个动作、逐个尺寸，所有落点都必须在画布内 ----
    // 桌宠既画在主页卡（约 70px）也画在状态栏剪影（约 26px）。若某个动作的根偏移或
    // 手臂角度过大，最上面的手或最下面的脚就会被画布裁掉；这条断言把"看着对不对"
    // 变成可回归的约束——改骨架比例或关键帧时一旦越界，主机测试会立刻失败。
    static const int sizes[] = { 96, 70, 44, 26, 22 };
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        int h = sizes[si];
        for (int id = 0; id < APP_PET_ACT_COUNT; id++) {
            app_pet_t ap;
            app_pet_init(&ap, 17);
            // 直接指定动作并置零起始时刻：绕开随机调度，确定性地覆盖该动作的整段关键帧。
            ap.action = id;
            ap.action_start_ms = 0;
            ap.action_dur_ms = UINT64_MAX;
            for (int t = 0; t <= 8000; t += 25) {
                app_pet_pose_t po;
                app_pet_skeleton_t s2;
                app_pet_pose_at(&ap, (uint64_t)t, &po);
                app_pet_skeleton(&po, h, h, &s2);
                const int xs[] = { s2.hip_x, s2.shoulder_x, s2.head_x, s2.elbow_l_x, s2.hand_l_x,
                                   s2.elbow_r_x, s2.hand_r_x, s2.knee_l_x, s2.foot_l_x,
                                   s2.knee_r_x, s2.foot_r_x };
                const int ys[] = { s2.hip_y, s2.shoulder_y, s2.head_y, s2.elbow_l_y, s2.hand_l_y,
                                   s2.elbow_r_y, s2.hand_r_y, s2.knee_l_y, s2.foot_l_y,
                                   s2.knee_r_y, s2.foot_r_y };
                for (int i = 0; i < 11; i++) {
                    assert(xs[i] >= 0 && xs[i] <= h);
                    assert(ys[i] >= 0 && ys[i] <= h);
                }
                assert(s2.head_y - s2.head_r >= 0);   // 头顶圆不越出上沿
                assert(s2.head_y + s2.head_r <= h);   // 也不越出下沿
            }
        }
    }

    // ---- 每个动作都要"真的动起来" ----
    // 只改大腿角度时，小腿、脚几乎只是跟着平移，脚的高度几乎不变——姿势数据在变，
    // 画面上却是一个卡住的造型（"抖腿"最早就是这么写坏的）。所以这条断言不看角度，
    // 只看 70px 画布上骨架落点的实际位移：任何一个关节的行程都不能太小。
    // 站立（呼吸）和托腮思考（保持一个姿势）是刻意接近静止的两个例外。
    for (int id = 0; id < APP_PET_ACT_COUNT; id++) {
        app_pet_t ap;
        app_pet_init(&ap, 29);
        ap.action = id;
        ap.action_start_ms = 0;
        ap.action_dur_ms = UINT64_MAX;

        int lo[22], hi[22];
        for (int k = 0; k < 22; k++) { lo[k] = 100000; hi[k] = -100000; }
        for (int t = 0; t <= 8000; t += 25) {
            app_pet_pose_t po;
            app_pet_skeleton_t s2;
            app_pet_pose_at(&ap, (uint64_t)t, &po);
            app_pet_skeleton(&po, 70, 70, &s2);
            const int16_t v[22] = { s2.hip_x, s2.hip_y, s2.shoulder_x, s2.shoulder_y, s2.head_x,
                s2.head_y, s2.elbow_l_x, s2.elbow_l_y, s2.hand_l_x, s2.hand_l_y, s2.elbow_r_x,
                s2.elbow_r_y, s2.hand_r_x, s2.hand_r_y, s2.knee_l_x, s2.knee_l_y, s2.foot_l_x,
                s2.foot_l_y, s2.knee_r_x, s2.knee_r_y, s2.foot_r_x, s2.foot_r_y };
            for (int k = 0; k < 22; k++) {
                if (v[k] < lo[k]) lo[k] = v[k];
                if (v[k] > hi[k]) hi[k] = v[k];
            }
        }
        int span = 0;
        for (int k = 0; k < 22; k++) if (hi[k] - lo[k] > span) span = hi[k] - lo[k];
        int need = (id == APP_PET_ACT_IDLE || id == APP_PET_ACT_THINK) ? 1 : 4;
        assert(span >= need);
    }

    // ---- 肘部不得穿进头部圆 ----
    // 举过头顶的手臂若角度太接近竖直，上臂会从头部圆圈里穿过去，画面上就是一根线
    // 把脑袋劈开——这是"看着诡异"最典型的来源。头随躯干/头部偏角一起摆，所以这条
    // 断言同时约束了关键帧角度和骨架比例，改动其中任何一个越界都会在这里失败。
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        int h = sizes[si];
        if (h < 44) continue;   // 状态栏剪影太小，几个像素的圆里谈不上"穿模"
        for (int id = 0; id < APP_PET_ACT_COUNT; id++) {
            app_pet_t ap;
            app_pet_init(&ap, 23);
            ap.action = id;
            ap.action_start_ms = 0;
            ap.action_dur_ms = UINT64_MAX;
            for (int t = 0; t <= 8000; t += 25) {
                app_pet_pose_t po;
                app_pet_skeleton_t s2;
                app_pet_pose_at(&ap, (uint64_t)t, &po);
                app_pet_skeleton(&po, h, h, &s2);
                int r = s2.head_r;
                long dl = (long)(s2.elbow_l_x - s2.head_x) * (s2.elbow_l_x - s2.head_x) +
                          (long)(s2.elbow_l_y - s2.head_y) * (s2.elbow_l_y - s2.head_y);
                long dr = (long)(s2.elbow_r_x - s2.head_x) * (s2.elbow_r_x - s2.head_x) +
                          (long)(s2.elbow_r_y - s2.head_y) * (s2.elbow_r_y - s2.head_y);
                assert(dl > (long)r * r);
                assert(dr > (long)r * r);
            }
        }
    }

    printf("test_app_pet: PASS\n");
    return 0;
}