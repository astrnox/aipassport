// Execute the real BSP with public-interface stubs and a fake ADC (no SDK needed).
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_button.c"

struct button_dev_t { button_driver_t *driver; bool live; };
static struct button_dev_t buttons[BSP_BTN_COUNT];
static int adc_token, cal_token, adc_live, cal_live, live_buttons;
static int create_calls, callback_calls, fail_create, fail_callback;
static int fail_adc, fail_channel, fail_cal, fail_read, fail_convert, fail_delete;
static int raw_mv, reads, events;
static int64_t clock_us;

// 长按定时器的替身：BSP 只要求 create / start_once / stop / delete 四个动作，
// 这里记录参数与激活状态，测试据此判断阈值与取消时机，并手动触发"到点"。
typedef struct { const esp_timer_create_args_t *args; bool active; bool live; } fake_timer_t;
#define FAKE_TIMER_MAX 8
static fake_timer_t timers[FAKE_TIMER_MAX];
static esp_timer_create_args_t timer_args[FAKE_TIMER_MAX];
static int timer_calls, fail_timer;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *h) {
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && !adc_live);
    if (fail_adc) return ESP_ERR_NO_MEM;
    adc_live = 1; *h = &adc_token; return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) {
    assert(h == &adc_token && adc_live && !live_buttons);
    adc_live = 0; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, int channel, const adc_oneshot_chan_cfg_t *cfg) {
    assert(h == &adc_token && channel == BSP_BTN_ADC_CHANNEL && cfg->atten == ADC_ATTEN_DB_12);
    return fail_channel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, int channel, int *raw) {
    assert(h == &adc_token && adc_live && channel == BSP_BTN_ADC_CHANNEL);
    ++reads;
    if (fail_read) return ESP_FAIL;
    *raw = raw_mv; return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *h) {
    assert(adc_live && !cal_live && cfg->atten == ADC_ATTEN_DB_12);
    if (fail_cal) return ESP_ERR_NO_MEM;
    cal_live = 1; *h = &cal_token; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) {
    assert(h == &cal_token && cal_live && !live_buttons);
    cal_live = 0; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) {
    assert(h == &cal_token && cal_live);
    if (fail_convert) { *mv = 0; return ESP_FAIL; }
    *mv = raw; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return clock_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) {
    assert(a && a->callback && a->dispatch_method == ESP_TIMER_TASK);
    if (++timer_calls == fail_timer) return ESP_ERR_NO_MEM;
    for (int i = 0; i < FAKE_TIMER_MAX; ++i) {
        if (timers[i].live) continue;
        timer_args[i] = *a;   // 组件按值保存参数，测试也不保留入参指针
        timers[i] = (fake_timer_t){ .args = &timer_args[i], .active = false, .live = true };
        *h = (esp_timer_handle_t)&timers[i];
        return ESP_OK;
    }
    assert(false); return ESP_ERR_NO_MEM;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) {
    fake_timer_t *t = (fake_timer_t *)h;
    assert(t && t->live && !t->active);
    assert(us == (uint64_t)BSP_BTN_LONG_PRESS_MS * 1000);
    t->active = true; return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h) {
    fake_timer_t *t = (fake_timer_t *)h;
    assert(t && t->live);
    if (!t->active) return ESP_ERR_INVALID_STATE;
    t->active = false; return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t h) {
    fake_timer_t *t = (fake_timer_t *)h;
    assert(t && t->live && !t->active);   // 删除前必须先停
    t->live = false; t->args = NULL; return ESP_OK;
}
// 模拟"按住时间到阈值"：一次性定时器到点后自动失活。
static void fire_long(bsp_btn_t btn) {
    fake_timer_t *t = (fake_timer_t *)s_long_timer[btn];
    assert(t && t->live && t->active);
    t->active = false;
    t->args->callback(t->args->arg);
}
// 模拟"回调已排队、抬起却先到"的晚到定时器。
static void fire_long_late(bsp_btn_t btn) {
    const fake_timer_t *t = (const fake_timer_t *)s_long_timer[btn];
    assert(t && t->live && t->args);
    t->args->callback(t->args->arg);
}
static void press(bsp_btn_t btn)   { cb_press(NULL, (void *)(intptr_t)btn); }
static void release(bsp_btn_t btn) { cb_release(NULL, (void *)(intptr_t)btn); }
esp_err_t iot_button_create(const button_config_t *cfg, const button_driver_t *driver, button_handle_t *h) {
    (void)cfg;
    if (++create_calls == fail_create) return ESP_ERR_NO_MEM;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (buttons[i].live) continue;
        buttons[i] = (struct button_dev_t){ .driver = (button_driver_t *)driver, .live = true };
        *h = &buttons[i]; ++live_buttons;
        assert(driver->get_key_level((button_driver_t *)driver) == BUTTON_INACTIVE);
        return ESP_OK;
    }
    assert(false); return ESP_FAIL;
}
esp_err_t iot_button_delete(button_handle_t h) {
    assert(h && h->live && adc_live && cal_live);
    if (fail_delete) return ESP_FAIL;
    assert(h->driver->del(h->driver) == ESP_OK);
    h->live = false; --live_buttons; return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *u) {
    (void)args; (void)ev;
    assert(h->live);
    cb(h, u); // No user callbacks may escape a partial initialization.
    return ++callback_calls == fail_callback ? ESP_ERR_NO_MEM : ESP_OK;
}
static int presses, clicks, longs;
static void event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    assert(btn >= BSP_BTN_UP && btn <= BSP_BTN_OK && u == &events);
    if (ev == BSP_BTN_PRESS) { ++presses; return; }
    if (ev == BSP_BTN_LONG)  { ++longs;   return; }
    assert(ev == BSP_BTN_CLICK);
    ++clicks;
    ++events;
}
static void reset_faults(void) {
    fail_adc = fail_channel = fail_cal = fail_read = fail_convert = fail_delete = 0;
    fail_create = fail_callback = fail_timer = create_calls = callback_calls = timer_calls = 0;
}
static void assert_clean(void) {
    assert(!adc_live && !cal_live && !live_buttons && !s_ready && !s_adc && !s_cali);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!s_btn[i]);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!s_long_timer[i] && !s_pressed[i] && !s_long_fired[i]);
    for (int i = 0; i < FAKE_TIMER_MAX; ++i) assert(!timers[i].live);
}
static void retry_success(void) {
    assert_clean(); reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT);
    button_cleanup(); assert_clean();
}
static void check_voltage(int mv, int expected) {
    raw_mv = mv; clock_us += 2000;
    const int before = reads;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_drivers[i].base.get_key_level(&s_drivers[i].base) == (i == expected));
    }
    assert(reads - before == CONFIG_ADC_BUTTON_SAMPLE_TIMES);
}
int main(void) {
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults(); fail_create = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    // 每个按键注册两个回调（按下、抬起），失败注入覆盖到这两个为止。
    for (int i = 1; i <= BSP_BTN_COUNT * 2; ++i) {
        reset_faults(); fail_callback = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults(); fail_timer = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    reset_faults(); fail_adc = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_channel = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_cal = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    assert(events == 0 && presses == 0 && clicks == 0 && longs == 0);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(s_long_timer[i]);

    // 短按：一次按下 + 一次抬起 == 一个 CLICK，抬起即发，不等待任何判定窗口。
    press(BSP_BTN_OK);
    assert(presses == 1 && clicks == 0 && longs == 0);
    release(BSP_BTN_OK);
    assert(clicks == 1 && longs == 0);

    // 长按：按住到阈值只报一次 LONG，之后抬起不再补 CLICK。
    press(BSP_BTN_OK);
    fire_long(BSP_BTN_OK);
    assert(longs == 1 && clicks == 1);
    release(BSP_BTN_OK);
    assert(clicks == 1 && longs == 1);

    // 抬起的取消必须先于定时器到点：晚到的回调不得补发 LONG。
    press(BSP_BTN_OK);
    release(BSP_BTN_OK);
    assert(clicks == 2 && longs == 1);
    fire_long_late(BSP_BTN_OK);
    assert(clicks == 2 && longs == 1);

    // 连续快按不会被合并成"双击"而丢掉动作：每次抬起都是一个 CLICK。
    for (int i = 0; i < 3; ++i) { press(BSP_BTN_OK); release(BSP_BTN_OK); }
    assert(clicks == 5 && longs == 1);

    // 三个按键各有一条独立的长按定时器与状态。
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_long_timer[i] && !s_pressed[i]);
        cb_press(NULL, (void *)(intptr_t)i);
        assert(s_pressed[i] && !s_pressed[(i + 1) % BSP_BTN_COUNT]);
        assert(((fake_timer_t *)s_long_timer[i])->active);   // 按下即起算
        cb_release(NULL, (void *)(intptr_t)i);
        assert(!((fake_timer_t *)s_long_timer[i])->active);  // 抬起即取消
    }
    assert(presses == 9 && clicks == 8 && longs == 1);

    check_voltage(0, BSP_BTN_UP); check_voltage(149, BSP_BTN_UP);
    check_voltage(150, BSP_BTN_DOWN); check_voltage(446, BSP_BTN_DOWN);
    check_voltage(447, BSP_BTN_OK); check_voltage(1899, BSP_BTN_OK);
    check_voltage(1900, -1); check_voltage(3300, -1);
    assert(bsp_button_read_mv() == 3300);
    fail_read = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_read = 0; fail_convert = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_delete = 1; button_cleanup();
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_ERR_INVALID_STATE);
    fail_delete = 0; button_cleanup(); retry_success();
    puts("BSP button fault-injection tests: PASS");
}
