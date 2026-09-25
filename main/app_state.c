// main/app_state.c —— 应用运行态与持久化数据的唯一入口实现。
//
// 存储约定：所有数据放在 NVS 命名空间 "app"。部分结构（作息表、赛事缓存）远大于
// 单个 NVS blob 的稳妥上限，因此统一走"分块"读写：数据切成 3000 字节的块写入
// "<key>.0"、"<key>.1"…，再用 "<key>#n" 记录块数、用 "<key>#l" 记录总长度。
// 读取时按记录的长度还原，多余的历史块不会影响结果。
//
// 时间约定：设备没有 RTC 电池，重启后时钟会丢失。这里用"单调时钟 + 基准偏移"表达
// 墙钟：epoch_base 是 uptime 为 0 时对应的 Unix 秒。重启后从 NVS 恢复上次已知时间，
// 但标记为未同步，直到 NTP 或手机/手动再次校准，避免把过期时间伪装成已同步。
#include "app_state.h"

#include "bsp_battery.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "app_state";

#define NVS_NS        "app"
#define BLOB_CHUNK    3000
#define TOTP_STORE_MAX APP_TOTP_MAX_ACCOUNTS

// TOTP 账户表需要整体持久化，用一个定长包装结构承载。
typedef struct {
    int count;
    app_totp_account_t items[TOTP_STORE_MAX];
} totp_store_t;

// 2026-01-01 00:00:00 +08:00 作为"时间未设定"时的占位基准。
#define DEFAULT_EPOCH 1767196800LL

typedef struct {
    app_settings_t      settings;
    app_badge_list_t    badges;
    app_routine_t       routine;
    app_reminder_list_t reminders;
    app_pomodoro_t      pomodoro;
    app_esport_cache_t  esports;
    totp_store_t        totp;

    app_net_state_t net;
    char            ssid[33];
    int             battery;

    int64_t epoch_base;   // uptime 为 0 时对应的 Unix 秒
    bool    loaded;
} app_runtime_t;

static app_runtime_t s;

// ---------------------------------------------------------------------------
// 时间换算（公历与 Unix 秒互转，避免依赖 newlib 的时区表）
// ---------------------------------------------------------------------------

// Howard Hinnant 的 days_from_civil：把公历日期转成 1970-01-01 起的天数。
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// 反向换算：天数 -> 公历。
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mm = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

static int64_t uptime_seconds(void)
{
    return esp_timer_get_time() / 1000000;
}

// ---------------------------------------------------------------------------
// NVS 分块读写
// ---------------------------------------------------------------------------

static void chunk_key(char *out, size_t cap, const char *key, int index)
{
    snprintf(out, cap, "%s.%d", key, index);
}

static esp_err_t blob_save(const char *key, const void *data, size_t size)
{
    if (!data || size == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    const int chunks = (int)((size + BLOB_CHUNK - 1) / BLOB_CHUNK);
    const uint8_t *bytes = (const uint8_t *)data;
    char name[24];

    for (int i = 0; i < chunks; i++) {
        size_t offset = (size_t)i * BLOB_CHUNK;
        size_t len = size - offset;
        if (len > BLOB_CHUNK) len = BLOB_CHUNK;
        chunk_key(name, sizeof(name), key, i);
        err = nvs_set_blob(handle, name, bytes + offset, len);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }

    char meta[24];
    snprintf(meta, sizeof(meta), "%s#n", key);
    err = nvs_set_i32(handle, meta, chunks);
    if (err == ESP_OK) {
        snprintf(meta, sizeof(meta), "%s#l", key);
        err = nvs_set_i32(handle, meta, (int32_t)size);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t blob_load(const char *key, void *out, size_t cap, size_t *out_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char meta[24];
    snprintf(meta, sizeof(meta), "%s#l", key);
    int32_t size = 0;
    err = nvs_get_i32(handle, meta, &size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (size <= 0 || (size_t)size > cap) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *bytes = (uint8_t *)out;
    int offset = 0;
    char name[24];
    for (int i = 0; offset < size; i++) {
        chunk_key(name, sizeof(name), key, i);
        size_t len = (size_t)(size - offset);
        if (len > BLOB_CHUNK) len = BLOB_CHUNK;
        size_t got = len;
        err = nvs_get_blob(handle, name, bytes + offset, &got);
        if (err != ESP_OK || got != len) {
            nvs_close(handle);
            return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
        }
        offset += (int)len;
    }
    nvs_close(handle);
    if (out_size) *out_size = (size_t)size;
    return ESP_OK;
}

static void blob_erase(const char *key)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return;

    char meta[24];
    snprintf(meta, sizeof(meta), "%s#n", key);
    int32_t chunks = 0;
    if (nvs_get_i32(handle, meta, &chunks) == ESP_OK) {
        char name[24];
        for (int i = 0; i < chunks; i++) {
            chunk_key(name, sizeof(name), key, i);
            nvs_erase_key(handle, name);
        }
    }
    snprintf(meta, sizeof(meta), "%s#n", key);
    nvs_erase_key(handle, meta);
    snprintf(meta, sizeof(meta), "%s#l", key);
    nvs_erase_key(handle, meta);
    nvs_commit(handle);
    nvs_close(handle);
}

// ---------------------------------------------------------------------------
// 默认值
// ---------------------------------------------------------------------------

static void settings_defaults(app_settings_t *st)
{
    memset(st, 0, sizeof(*st));
    st->theme = APP_THEME_FIXED_DARK;
    st->screen_timeout = APP_TIMEOUT_30S;
    st->backlight = 100;
    st->sound_muted = false;
    st->volume = 70;
    st->power_save = false;
    st->utc_offset_minutes = 480;   // 东八区，默认值可改
    st->time_synced = false;
    strncpy(st->time_source, "未设置", sizeof(st->time_source) - 1);
    st->life_expectancy = 80;
    st->boarding = false;
    st->use_odd_week = false;
    st->home_focus = 0;
}

static void runtime_defaults(void)
{
    settings_defaults(&s.settings);
    app_badge_list_init(&s.badges);
    app_routine_init(&s.routine);
    app_reminder_list_init(&s.reminders);
    app_pomodoro_init(&s.pomodoro);
    memset(&s.esports, 0, sizeof(s.esports));
    memset(&s.totp, 0, sizeof(s.totp));
    s.net = APP_NET_OFF;
    s.ssid[0] = '\0';
    s.battery = -1;
    s.epoch_base = DEFAULT_EPOCH;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

esp_err_t app_state_init(void)
{
    if (s.loaded) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新格式化: %s", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    runtime_defaults();

    if (err == ESP_OK) {
        size_t got = 0;
        if (blob_load("settings", &s.settings, sizeof(s.settings), &got) != ESP_OK ||
            got != sizeof(s.settings)) {
            settings_defaults(&s.settings);
        }
        if (blob_load("badges", &s.badges, sizeof(s.badges), &got) != ESP_OK ||
            got != sizeof(s.badges)) {
            app_badge_list_init(&s.badges);
        }
        if (blob_load("routine", &s.routine, sizeof(s.routine), &got) != ESP_OK ||
            got != sizeof(s.routine)) {
            app_routine_init(&s.routine);
        }
        if (blob_load("reminders", &s.reminders, sizeof(s.reminders), &got) != ESP_OK ||
            got != sizeof(s.reminders)) {
            app_reminder_list_init(&s.reminders);
        }
        if (blob_load("pomodoro", &s.pomodoro, sizeof(s.pomodoro), &got) != ESP_OK ||
            got != sizeof(s.pomodoro)) {
            app_pomodoro_init(&s.pomodoro);
        }
        if (blob_load("esports", &s.esports, sizeof(s.esports), &got) == ESP_OK &&
            got == sizeof(s.esports)) {
            app_esport_apply_follows(&s.esports);
        } else {
            memset(&s.esports, 0, sizeof(s.esports));
        }
        if (blob_load("totp", &s.totp, sizeof(s.totp), &got) != ESP_OK ||
            got != sizeof(s.totp)) {
            memset(&s.totp, 0, sizeof(s.totp));
        }

        // 恢复上次已知的墙钟。重启后时钟不可信，故标记为未同步。
        int64_t stored = 0;
        size_t stored_len = 0;
        if (blob_load("clock", &stored, sizeof(stored), &stored_len) == ESP_OK &&
            stored_len == sizeof(stored) && stored > DEFAULT_EPOCH) {
            s.epoch_base = stored;
            s.settings.time_synced = false;
        }
    } else {
        ESP_LOGE(TAG, "NVS 初始化失败，以默认值运行: %s", esp_err_to_name(err));
    }

    s.loaded = true;
    ESP_LOGI(TAG, "状态载入完成: 主题=%d 工牌=%d 提醒=%d 口令=%d 赛事缓存=%d",
             s.settings.theme, s.badges.count, s.reminders.count, s.totp.count,
             s.esports.valid);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------

uint64_t app_state_now_unix(void)
{
    int64_t now = s.epoch_base + uptime_seconds();
    return now > 0 ? (uint64_t)now : 0;
}

app_datetime_t app_state_now(void)
{
    int64_t local = (int64_t)app_state_now_unix() + (int64_t)s.settings.utc_offset_minutes * 60;
    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    app_datetime_t dt = {0};
    civil_from_days(days, &dt.year, &dt.month, &dt.day);
    dt.hour = (int)(rem / 3600);
    dt.minute = (int)((rem / 60) % 60);
    dt.second = (int)(rem % 60);
    return dt;
}

void app_state_set_time(const app_datetime_t *dt, const char *source)
{
    if (!dt || !app_time_valid(dt)) return;

    int64_t days = days_from_civil(dt->year, (unsigned)dt->month, (unsigned)dt->day);
    int64_t unix_local = days * 86400 + dt->hour * 3600 + dt->minute * 60 + dt->second;
    int64_t unix_utc = unix_local - (int64_t)s.settings.utc_offset_minutes * 60;

    s.epoch_base = unix_utc - uptime_seconds();
    s.settings.time_synced = true;
    strncpy(s.settings.time_source, source ? source : "手动",
            sizeof(s.settings.time_source) - 1);
    s.settings.time_source[sizeof(s.settings.time_source) - 1] = '\0';

    int64_t stored = s.epoch_base + uptime_seconds();
    blob_save("clock", &stored, sizeof(stored));
    app_state_save_settings();
}

// ---------------------------------------------------------------------------
// 电量与网络
// ---------------------------------------------------------------------------

int app_state_battery_soc(void)
{
    return s.battery;
}

void app_state_battery_refresh(void)
{
    int soc = bsp_battery_soc();
    if (soc >= 0 && soc <= 100) s.battery = soc;
}

void app_state_set_net(app_net_state_t state, const char *ssid)
{
    s.net = state;
    if (ssid) {
        strncpy(s.ssid, ssid, sizeof(s.ssid) - 1);
        s.ssid[sizeof(s.ssid) - 1] = '\0';
    } else if (state == APP_NET_OFF) {
        s.ssid[0] = '\0';
    }
}

app_net_state_t app_state_net(void)
{
    return s.net;
}

const char *app_state_net_ssid(void)
{
    return s.ssid;
}

// ---------------------------------------------------------------------------
// 数据访问
// ---------------------------------------------------------------------------

app_settings_t     *app_state_settings(void)   { return &s.settings; }
app_badge_list_t   *app_state_badges(void)     { return &s.badges; }
app_routine_t      *app_state_routine(void)    { return &s.routine; }
app_reminder_list_t *app_state_reminders(void) { return &s.reminders; }
app_pomodoro_t     *app_state_pomodoro(void)   { return &s.pomodoro; }
app_esport_cache_t *app_state_esports(void)    { return &s.esports; }

int app_state_routine_slot(void)
{
    if (!s.settings.use_odd_week) return 0;
    app_datetime_t now = app_state_now();
    return app_routine_week_slot(app_time_iso_week(now.year, now.month, now.day));
}

app_routine_day_t *app_state_routine_day_slot(int weekday, int slot)
{
    return app_routine_day_mut(&s.routine, weekday, slot);
}

app_routine_day_t *app_state_routine_day(int weekday)
{
    return app_routine_day_mut(&s.routine, weekday, app_state_routine_slot());
}

int app_state_totp_count(void)
{
    return s.totp.count;
}

app_totp_account_t *app_state_totp_at(int index)
{
    if (index < 0 || index >= s.totp.count) return NULL;
    return &s.totp.items[index];
}

int app_state_totp_add(const app_totp_account_t *account)
{
    if (!account || s.totp.count >= TOTP_STORE_MAX) return -1;
    s.totp.items[s.totp.count] = *account;
    s.totp.count++;
    app_state_save_totp();
    return s.totp.count - 1;
}

bool app_state_totp_remove(int index)
{
    if (index < 0 || index >= s.totp.count) return false;
    for (int i = index; i < s.totp.count - 1; i++) {
        s.totp.items[i] = s.totp.items[i + 1];
    }
    s.totp.count--;
    memset(&s.totp.items[s.totp.count], 0, sizeof(s.totp.items[0]));
    app_state_save_totp();
    return true;
}

int app_state_badge_selected(void)
{
    if (s.badges.count <= 0) return -1;
    if (s.badges.selected < 0 || s.badges.selected >= s.badges.count) return 0;
    return s.badges.selected;
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------

void app_state_save_settings(void) { blob_save("settings", &s.settings, sizeof(s.settings)); }
void app_state_save_badges(void)   { blob_save("badges", &s.badges, sizeof(s.badges)); }
void app_state_save_routine(void)  { blob_save("routine", &s.routine, sizeof(s.routine)); }
void app_state_save_totp(void)     { blob_save("totp", &s.totp, sizeof(s.totp)); }
void app_state_save_reminders(void){ blob_save("reminders", &s.reminders, sizeof(s.reminders)); }
void app_state_save_pomodoro(void) { blob_save("pomodoro", &s.pomodoro, sizeof(s.pomodoro)); }
void app_state_save_esports(void)  { blob_save("esports", &s.esports, sizeof(s.esports)); }

void app_state_save_all(void)
{
    app_state_save_settings();
    app_state_save_badges();
    app_state_save_routine();
    app_state_save_totp();
    app_state_save_reminders();
    app_state_save_pomodoro();
    app_state_save_esports();
}

void app_state_clear(app_data_kind_t kind)
{
    switch (kind) {
    case APP_DATA_BADGES:
        app_badge_list_init(&s.badges);
        blob_erase("badges");
        break;
    case APP_DATA_ROUTINE:
        app_routine_init(&s.routine);
        blob_erase("routine");
        break;
    case APP_DATA_TOTP:
        memset(&s.totp, 0, sizeof(s.totp));
        blob_erase("totp");
        break;
    case APP_DATA_REMINDERS:
        app_reminder_list_init(&s.reminders);
        blob_erase("reminders");
        break;
    case APP_DATA_ESPORTS:
        memset(&s.esports, 0, sizeof(s.esports));
        blob_erase("esports");
        break;
    case APP_DATA_SETTINGS:
        settings_defaults(&s.settings);
        app_pomodoro_init(&s.pomodoro);
        blob_erase("settings");
        blob_erase("pomodoro");
        blob_erase("clock");
        s.epoch_base = DEFAULT_EPOCH;
        break;
    case APP_DATA_ALL:
    default:
        runtime_defaults();
        blob_erase("settings");
        blob_erase("badges");
        blob_erase("routine");
        blob_erase("totp");
        blob_erase("reminders");
        blob_erase("pomodoro");
        blob_erase("esports");
        blob_erase("clock");
        break;
    }
}
