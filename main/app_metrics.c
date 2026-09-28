// main/app_metrics.c —— app_metrics.h 的实现。见头文件里的线程约定。
#include "app_metrics.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "metrics";

static TaskHandle_t s_input_task;

void app_metrics_mem(const char *where)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    // 内部堆而非总堆：C3 无 PSRAM，动态数据全在内部 SRAM，它才是稀缺资源。
    ESP_LOGI(TAG, "[%s] LVGL池 空闲 %u 字节 碎片 %u%% 峰值 %u 字节 | 内部堆 空闲 %u 最大连续块 %u",
             where ? where : "?", (unsigned)mon.free_size, (unsigned)mon.frag_pct,
             (unsigned)mon.max_used,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void app_metrics_stack(const char *where)
{
    ESP_LOGI(TAG, "[%s] 任务 %s 栈高水位 %u 字", where ? where : "?",
             pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void app_metrics_set_input_task(TaskHandle_t task)
{
    s_input_task = task;
}

void app_metrics_report(const char *where)
{
    app_metrics_mem(where);
    if (s_input_task) {
        ESP_LOGI(TAG, "[%s] app_input 栈高水位 %u 字", where ? where : "?",
                 (unsigned)uxTaskGetStackHighWaterMark(s_input_task));
    }
    app_metrics_stack(where);
}