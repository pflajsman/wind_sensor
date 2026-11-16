/* ui.c: user button + SK6812 status LED. See ui.h for the model. */
#include "ui.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "led_strip.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "awning_config.h"

static const char *TAG = "UI";

static ui_callbacks_t s_cb;

/* ------------------------------------------------------------------------------------- */
/* LED                                                                                    */
/* ------------------------------------------------------------------------------------- */
typedef struct {
    ui_led_mode_t mode;
    uint32_t duration_ms;
} led_cmd_t;

static led_strip_handle_t s_strip;
static QueueHandle_t s_led_q;

#ifdef CONFIG_PM_ENABLE
/* Held while the LED is animating: blinking needs the CPU + RMT awake. Released when the LED
 * is off, so idle light sleep is unaffected. */
static esp_pm_lock_handle_t s_led_pm_lock;
static esp_pm_lock_handle_t s_btn_pm_lock;
#endif

static void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    if (r | g | b) {
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
}

/* One pattern step: sets the colour for this phase and returns how long to hold it (ms). */
static uint32_t led_step(ui_led_mode_t mode, uint32_t phase)
{
    const uint8_t L = LED_BRIGHTNESS;
    switch (mode) {
    case UI_LED_PAIRING:     led_rgb(0, 0, (phase & 1) ? 0 : L); return (phase & 1) ? 800 : 200;
    case UI_LED_JOINED:      led_rgb(0, L, 0);                   return 1000;
    case UI_LED_ACK:         led_rgb(0, L, 0);                   return 1000;
    case UI_LED_FAILED:      led_rgb(L, 0, 0);                   return 1000;
    case UI_LED_RESET_ARMED: led_rgb((phase & 1) ? 0 : L, 0, 0); return 100;
    case UI_LED_RESETTING:   led_rgb(L, 0, 0);                   return 1000;
    case UI_LED_IDENTIFY:    { uint8_t v = (phase & 1) ? 0 : L; led_rgb(v, v, v); return 500; }
    case UI_LED_OFF:
    default:                 led_rgb(0, 0, 0);                   return 0;
    }
}

static void led_task(void *pv)
{
    ui_led_mode_t mode = UI_LED_OFF;
    int64_t until_us = 0;      /* 0 = no expiry */
    uint32_t phase = 0;
    bool awake = false;

    for (;;) {
        uint32_t hold_ms = (mode == UI_LED_OFF) ? 0 : led_step(mode, phase++);

        if (mode != UI_LED_OFF && until_us) {
            int64_t left_ms = (until_us - esp_timer_get_time()) / 1000;
            if (left_ms < 1) left_ms = 1;
            if ((int64_t)hold_ms > left_ms) hold_ms = (uint32_t)left_ms; /* don't overshoot expiry */
        }

        led_cmd_t cmd;
        TickType_t wait = (mode == UI_LED_OFF) ? portMAX_DELAY : pdMS_TO_TICKS(hold_ms);
        if (xQueueReceive(s_led_q, &cmd, wait) == pdTRUE) {
            mode = cmd.mode;
            phase = 0;
            until_us = cmd.duration_ms ? esp_timer_get_time() + (int64_t)cmd.duration_ms * 1000 : 0;
            if (mode == UI_LED_OFF) led_rgb(0, 0, 0);
        } else if (until_us && esp_timer_get_time() >= until_us) {
            mode = UI_LED_OFF;
            led_rgb(0, 0, 0);
        }

#ifdef CONFIG_PM_ENABLE
        bool want_awake = (mode != UI_LED_OFF);
        if (want_awake != awake && s_led_pm_lock) {
            if (want_awake) esp_pm_lock_acquire(s_led_pm_lock);
            else            esp_pm_lock_release(s_led_pm_lock);
            awake = want_awake;
        }
#else
        (void)awake;
#endif
    }
}

void ui_led(ui_led_mode_t mode, uint32_t duration_ms)
{
    if (!s_led_q) return;
    led_cmd_t cmd = { .mode = mode, .duration_ms = duration_ms };
    if (xQueueSend(s_led_q, &cmd, 0) != pdTRUE) {
        /* Queue full: drop the oldest pending command so the latest state always wins. */
        led_cmd_t junk;
        xQueueReceive(s_led_q, &junk, 0);
        xQueueSend(s_led_q, &cmd, 0);
    }
}

static esp_err_t led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        s_strip = NULL;
        return err;
    }
    led_strip_clear(s_strip);
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------- */
/* Button                                                                                 */
/* ------------------------------------------------------------------------------------- */
static TaskHandle_t s_btn_task;

static inline bool btn_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == BUTTON_ACTIVE_LEVEL;
}

void ui_tick(void)
{
    /* Cheap check on the report task's existing wakeup; only wake the button task if pressed. */
    if (s_btn_task && btn_pressed()) xTaskNotifyGive(s_btn_task);
}

static void btn_task(void *pv)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!btn_pressed()) continue;

#ifdef CONFIG_PM_ENABLE
        if (s_btn_pm_lock) esp_pm_lock_acquire(s_btn_pm_lock);
#endif
        /* Debounce, then time the press. Timing starts when we first SAW it (it may already
         * have been held up to one tick), which is fine for a 5 s threshold. */
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        int64_t t0 = esp_timer_get_time();
        bool armed = false, cancelled = false;

        while (btn_pressed()) {
            int64_t held_ms = (esp_timer_get_time() - t0) / 1000;
            if (!armed && held_ms >= (int64_t)UI_LONG_PRESS_S * 1000) {
                armed = true;
                ESP_LOGW(TAG, "long press: release now to factory reset");
                ui_led(UI_LED_RESET_ARMED, 0);
            }
            if (held_ms >= (int64_t)UI_STUCK_PRESS_S * 1000) {
                /* Stuck button / something pressing on the plunger: never reset on this. */
                cancelled = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
        }

        if (cancelled) {
            ESP_LOGW(TAG, "button held > %ds: ignored (stuck?)", UI_STUCK_PRESS_S);
            ui_led(UI_LED_OFF, 0);
            while (btn_pressed()) vTaskDelay(pdMS_TO_TICKS(500)); /* wait for release */
        } else if (armed) {
            ESP_LOGW(TAG, "long press released: factory reset");
            if (s_cb.on_long_press) s_cb.on_long_press();
        } else {
            ESP_LOGI(TAG, "short press");
            if (s_cb.on_short_press) s_cb.on_short_press();
        }

#ifdef CONFIG_PM_ENABLE
        if (s_btn_pm_lock) esp_pm_lock_release(s_btn_pm_lock);
#endif
        ulTaskNotifyTake(pdTRUE, 0); /* drop ticks that arrived during this press */
    }
}

/* ------------------------------------------------------------------------------------- */
esp_err_t ui_init(const ui_callbacks_t *cb)
{
    if (cb) s_cb = *cb;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

#ifdef CONFIG_PM_ENABLE
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui_led", &s_led_pm_lock);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui_btn", &s_btn_pm_lock);
#endif

    s_led_q = xQueueCreate(4, sizeof(led_cmd_t));
    if (led_init() != ESP_OK) ESP_LOGW(TAG, "status LED init failed: no LED feedback");
    xTaskCreate(led_task, "ui_led", 3072, NULL, 3, NULL);
    xTaskCreate(btn_task, "ui_btn", 3072, NULL, 3, &s_btn_task);
    return ESP_OK;
}
