// main/main.c —— Passport 随身工具箱固件入口。
//
// 本文件只做三件事：初始化 BSP 外设、初始化应用运行态与联网基础设施、把按键事件
// 交给界面控制器。页面导航、息屏与全局节拍都在 ui/ui_app.c，离线数据在 app_state.c，
// 联网服务在 net/app_net.c——入口保持"接线"角色，不放业务逻辑。
//
// 按键约定（全局统一，详见 ui/ui_app.c）：
//   主页   UP/DOWN 移动模块焦点，OK 进入，长按 UP 快捷面板，长按 OK 熄屏
//   模块页 由各页自定义，长按 OK 在页面根视图返回主页
//   熄屏   任意键先唤醒并回到熄屏前页面
//
// 线程约定：button 回调运行在 esp_timer 任务上，只入队立即返回；真正的处理放在
// 独立的输入任务里，避免阻塞按键驱动的定时器。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "app_assets.h"
#include "app_state.h"
#include "logic/app_vault.h"
#include "net/app_net.h"
#include "ui/ui_app.h"
#include "ui/ui_theme.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

// 按键回调运行在共享的 esp_timer 任务上，只入队不处理。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            ui_app_handle_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "app_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 密码本的随机源：硬件 RNG。C3 的 RNG 由射频子系统提供熵，Wi-Fi/BT 未开启时也
// 有足够熵用于生成 DEK 与恢复码。
static void vault_random(void *ctx, void *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
}

// 按设置决定开机主题：固定明/暗直接用，自动模式按本地时段判断。
static void apply_theme(void)
{
    app_settings_t *st = app_state_settings();
    switch (st->theme) {
    case APP_THEME_FIXED_LIGHT:
        ui_theme_set(UI_THEME_LIGHT);
        break;
    case APP_THEME_AUTO: {
        app_datetime_t now = app_state_now();
        ui_theme_apply_auto(true, now.hour);
        break;
    }
    case APP_THEME_FIXED_DARK:
    default:
        ui_theme_set(UI_THEME_DARK);
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Passport 随身工具箱启动");

    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是唯一界面载体，初始化失败就没有可用的产品界面：打清楚日志后退出，
    // 不做"降级成串口控制台"的处理。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 运行态与持久化：先于界面创建，页面构建时直接读取数据。
    esp_err_t state_err = app_state_init();
    if (state_err != ESP_OK) {
        ESP_LOGW(TAG, "数据载入失败(%s)，以默认值运行", esp_err_to_name(state_err));
    }

    // 密码本的密钥与随机数只能来自硬件 RNG：注入失败就不允许加密，绝不退化成用
    // 可预测的字节当密钥（app_vault 在未注入时会让所有需要随机数的操作报错）。
    app_vault_set_random(vault_random, NULL);

    // 动图资源分区：挂在界面之前，页面创建时才能判断某个槽位是否已有动图。
    // 失败不影响离线功能，只是动图不可用。
    if (app_assets_init() != ESP_OK) {
        ESP_LOGW(TAG, "动图资源分区不可用，名片将只显示文字");
    }

    // 联网基础设施（默认事件循环与 netif）只准备一次，不打开射频。
    esp_err_t net_err = app_net_init();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "联网基础设施初始化失败(%s)，离线功能不受影响",
                 esp_err_to_name(net_err));
    }

    // 音频与电量失败不阻塞界面：提示音静默跳过，电量显示为未知。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败，提示音将不可用");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败，电量显示为未知");
    }
    app_state_battery_refresh();

    esp_err_t input_err = input_dispatch_init();
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        vTaskDelete(s_input_task);
        s_input_task = NULL;
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }

    if (bsp_lvgl_lock(1000)) {
        ui_fonts_init();
        apply_theme();
        ui_app_start();
        bsp_lvgl_unlock();
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "LVGL 加锁失败，界面未启动");
    }

    ESP_LOGI(TAG, "就绪：显示与界面已启动");
}
