// main/logic/app_vault.h —— 密码保险箱的数据模型与加密容器（与硬件无关，可主机测试）。
//
// 两种模式，用户自己选：
//   明文保存  条目直接以明文存在 NVS 里。开箱即用、无需解锁，适合"这台设备本来就只有
//             我自己用"的场景。这是默认模式，因为强行加密会让忘记手势的用户永久失去
//             全部密码，代价远大于它挡住的那些人。
//   加密保护  整本条目用 AES-256-CBC 加密后再落盘，密钥由"敲击手势"经 PBKDF2 派生。
//
// 密钥层次（为什么不是直接用口令加密条目）：
//   随机 DEK（32 字节，只存在于 RAM 与两份密文封装里）
//     ├── 用 KDF(敲击手势, salt_knock) 派生的 KEK 封装一份
//     └── 用 KDF(恢复码,   salt_recovery) 派生的 KEK 封装一份
//   条目正文用 DEK 派生的加密密钥 AES-256-CBC 加密，再用 DEK 派生的 MAC 密钥做
//   HMAC-SHA256（先加密后认证，覆盖 magic 到密文的全部字节）。
// 这样"改手势"或"换恢复码"只需要重新封装 DEK，不必重写整本条目；而恢复码给了用户
// 一条忘掉手势后的退路——没有它，加密一旦丢失密钥，内容就永久解不开了。
//
// 安全定位（必须如实写进产品文档，不能含糊）：
//   敲击手势只有 4..16 次、每次 2 个方向，熵最多 16 bit，只能防"捡到设备的旁人"，
//   挡不住拿到 Flash 镜像后的离线暴力破解。PBKDF2 迭代次数刻意只取
//   APP_VAULT_ITERATIONS：本机 MVGL 渲染与按键处理共用同一个任务，密钥派生会在这条
//   路径上同步阻塞，迭代太高会让解锁明显卡顿。真正的强口令是 128 位随机恢复码。
//
// 容器格式（小端；指纹字段按字节显式读写，不用结构体直拷，避免编译器对齐差异把
// 存量数据变成不可读）：
//   明文模式：magic(4) ver(1) mode(1) count(2) 之后是 count 条记录
//   加密模式：magic(4) ver(1) mode(1) count(2)
//             salt_knock(16) salt_recovery(16) iterations(4)
//             iv_wrap_knock(16) wrap_knock(48) iv_wrap_recovery(16) wrap_recovery(48)
//             payload_len(4) iv_payload(16) payload(payload_len) mac(32)
//   记录：label_len(1) label account_len(1) account password_len(1) password
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_VAULT_MAX          30    // 条目上限
#define APP_VAULT_LABEL_LEN    24    // 名称，UTF-8，最多 8 个汉字
#define APP_VAULT_ACCOUNT_LEN  40    // 账号，UTF-8
#define APP_VAULT_PASSWORD_LEN 48    // 口令，UTF-8，最多 16 个汉字
#define APP_VAULT_KNOCK_MIN    4     // 手势最短敲击数
#define APP_VAULT_KNOCK_MAX    16    // 手势最长敲击数
#define APP_VAULT_ITERATIONS   20000 // PBKDF2 迭代次数，见文件头的定位说明

// 恢复码：128 位随机数用 Crockford Base32 表示成 26 个字符，按 5 位一组用 '-' 分隔，
// 共 31 个字符（26 + 5）。Crockford 表去掉了 I/L/O/U，避免与 1/0 混淆。
#define APP_VAULT_RECOVERY_CHARS 26
#define APP_VAULT_RECOVERY_LEN   31

#define APP_VAULT_TAP_UP   0
#define APP_VAULT_TAP_DOWN 1

#define APP_VAULT_ENTRY_BYTES \
    (3 + APP_VAULT_LABEL_LEN + APP_VAULT_ACCOUNT_LEN + APP_VAULT_PASSWORD_LEN)
// 明文记录区上限（不含容器头）。
#define APP_VAULT_RECORDS_MAX (APP_VAULT_MAX * APP_VAULT_ENTRY_BYTES)
// CBC 固定补一块，所以密文上限比明文多 16 字节。
#define APP_VAULT_PAYLOAD_MAX (APP_VAULT_RECORDS_MAX + 16)
// 加密容器固定头部长度（magic 到 iv_payload 结束）。
#define APP_VAULT_ENC_HEADER  192
#define APP_VAULT_MAC_LEN     32
#define APP_VAULT_WRAP_LEN    48    // AES-256-CBC 封装 32 字节 DEK 后为 48 字节
// 容器序列化缓冲上限。
#define APP_VAULT_CONTAINER_MAX \
    (APP_VAULT_ENC_HEADER + APP_VAULT_PAYLOAD_MAX + APP_VAULT_MAC_LEN)

typedef enum {
    APP_VAULT_PLAIN = 0,
    APP_VAULT_ENCRYPTED,
} app_vault_mode_t;

typedef enum {
    APP_VAULT_OK = 0,
    APP_VAULT_ERR_FORMAT,    // 容器损坏、版本不符或长度自相矛盾
    APP_VAULT_ERR_LOCKED,    // 加密模式下未解锁
    APP_VAULT_ERR_AUTH,      // 手势或恢复码不对
    APP_VAULT_ERR_STATE,     // 当前模式/状态不允许该操作
    APP_VAULT_ERR_FULL,      // 条目已满
    APP_VAULT_ERR_RANGE,     // 下标或长度越界
    APP_VAULT_ERR_MEMORY,    // 输出缓冲不足
    APP_VAULT_ERR_ENTROPY,   // 随机源未注入或返回失败
} app_vault_status_t;

typedef struct {
    char label[APP_VAULT_LABEL_LEN];
    char account[APP_VAULT_ACCOUNT_LEN];
    char password[APP_VAULT_PASSWORD_LEN];
    bool used;
} app_vault_entry_t;

// 敲击手势：只由 UP / DOWN 两种敲击组成，OK 键用于提交、长按 UP 退格、长按 DOWN 清空。
// 不把 OK 也算作敲击，是为了让"输入"和"确认"永远不歧义。
typedef struct {
    uint8_t taps[APP_VAULT_KNOCK_MAX];
    int len;
} app_vault_knock_t;

typedef struct {
    app_vault_mode_t mode;
    int count;                                  // 条目数；加密模式下也保存，供锁定态显示
    app_vault_entry_t entries[APP_VAULT_MAX];    // 仅解锁时有效
    int selected;

    // 加密元数据（明文模式下全为 0）。
    uint8_t  salt_knock[16];
    uint8_t  salt_recovery[16];
    uint32_t iterations;
    uint8_t  iv_wrap_knock[16];
    uint8_t  wrap_knock[APP_VAULT_WRAP_LEN];
    uint8_t  iv_wrap_recovery[16];
    uint8_t  wrap_recovery[APP_VAULT_WRAP_LEN];
    uint8_t  iv_payload[16];
    uint8_t  payload[APP_VAULT_PAYLOAD_MAX];
    size_t   payload_len;
    uint8_t  mac[APP_VAULT_MAC_LEN];

    // ---- 运行态，不序列化 ----
    bool     unlocked;                          // 明文模式下恒为 true
    bool     has_dek;                           // dek 是否已恢复
    uint8_t  dek[32];
    char     recovery[APP_VAULT_RECOVERY_LEN + 1];  // 待展示的恢复码；空串表示不展示
} app_vault_t;

// ---------------------------------------------------------------------------
// 随机源
// ---------------------------------------------------------------------------
// 设备端注入 esp_fill_random，主机测试注入确定性序列。未注入时所有需要随机数的操作
// 都返回 APP_VAULT_ERR_ENTROPY，绝不退化成"用可预测的字节当密钥"。
typedef void (*app_vault_random_fn)(void *ctx, void *out, size_t len);
void app_vault_set_random(app_vault_random_fn fn, void *ctx);

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
// 明文、空本、已解锁。用于首次启动与"清除数据"。
void app_vault_init(app_vault_t *v);

// 解析容器。失败时 v 被重置为初始状态，避免留下半个可用的密码本。
app_vault_status_t app_vault_deserialize(app_vault_t *v, const uint8_t *data, size_t len);

// 序列化容器，返回写入字节数；缓冲不足或状态非法（如加密但未解锁且无原文）返回 0。
// 加密且已解锁时会就地重新加密正文并缓存新 MAC，因此接收非 const 指针。
size_t app_vault_serialize(app_vault_t *v, uint8_t *out, size_t cap);

// 加密模式下尚未解锁。
bool app_vault_is_locked(const app_vault_t *v);
// 当前是否处于加密模式。
bool app_vault_is_encrypted(const app_vault_t *v);

// ---------------------------------------------------------------------------
// 条目
// ---------------------------------------------------------------------------
// 三者都返回状态码；label/account/password 允许为 NULL 或空串。
// 加密模式下未解锁时返回 APP_VAULT_ERR_LOCKED。
app_vault_status_t app_vault_add(app_vault_t *v, const char *label, const char *account,
                                 const char *password, int *index_out);
app_vault_status_t app_vault_update(app_vault_t *v, int index, const char *label,
                                    const char *account, const char *password);
app_vault_status_t app_vault_remove(app_vault_t *v, int index);
// 越界或未解锁返回 NULL。
const app_vault_entry_t *app_vault_at(const app_vault_t *v, int index);
// 列表选中项：空本返回 -1。
int app_vault_selected(const app_vault_t *v);
void app_vault_select(app_vault_t *v, int index);
void app_vault_cycle(app_vault_t *v, int delta);

// ---------------------------------------------------------------------------
// 加密
// ---------------------------------------------------------------------------
// 从明文切到加密模式：生成 DEK、两份 salt，用给定手势封装 DEK，并把当前条目加密写入。
// 成功后 v->recovery 里留着新生成的恢复码，界面必须提示用户抄写。
app_vault_status_t app_vault_enable_encryption(app_vault_t *v, const app_vault_knock_t *knock);

// 退回明文模式。要求已解锁（加密模式下）。
app_vault_status_t app_vault_disable_encryption(app_vault_t *v);

// 用敲击手势解锁。成功时 entries 生效、unlocked 置位。
app_vault_status_t app_vault_unlock_knock(app_vault_t *v, const app_vault_knock_t *knock);
// 用恢复码解锁。code 允许带分隔符与大小写差异。
app_vault_status_t app_vault_unlock_recovery(app_vault_t *v, const char *code);

// 立即上锁：清空 DEK 与已解密条目。明文模式下该调用无副作用。
void app_vault_lock(app_vault_t *v);

// 改手势：重新派生并封装 DEK，条目密文不变。要求已解锁。
app_vault_status_t app_vault_change_knock(app_vault_t *v, const app_vault_knock_t *knock);
// 重新生成恢复码并换掉旧的封装。要求已解锁。旧恢复码随即失效。
app_vault_status_t app_vault_new_recovery_code(app_vault_t *v);

// 待展示的恢复码（仅本次运行内有效，重启即丢）；没有时返回空串。
const char *app_vault_pending_recovery(const app_vault_t *v);
// 用户确认已抄写后清除。
void app_vault_clear_pending_recovery(app_vault_t *v);

// ---------------------------------------------------------------------------
// 敲击手势工具
// ---------------------------------------------------------------------------
bool app_vault_knock_push(app_vault_knock_t *k, int tap);
bool app_vault_knock_pop(app_vault_knock_t *k);
void app_vault_knock_clear(app_vault_knock_t *k);
// 长度落在 [MIN, MAX] 且每个敲击都是 UP/DOWN 才算合法。
bool app_vault_knock_valid(const app_vault_knock_t *k);
// 两次输入是否完全一致（改手势时要求连输两遍）。
bool app_vault_knock_same(const app_vault_knock_t *a, const app_vault_knock_t *b);

// 恢复码归一化：去掉 '-' 与空白、转大写、把易混字符折回 Crockford 表。返回是否合法。
bool app_vault_recovery_normalize(const char *code, char *out, size_t cap);

const char *app_vault_status_text(app_vault_status_t status);
const char *app_vault_mode_name(app_vault_mode_t mode);
