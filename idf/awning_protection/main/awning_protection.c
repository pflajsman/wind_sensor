/* awning_protection.c — ESP32-H2 / esp-zigbee-sdk (2.x ezb_* API) LIGHT-SLEEP end device.
 *
 * SCOPE (deliberately small): solve exactly four things and nothing else.
 *   1. Join / rejoin a deCONZ network (channel-pinned, native IDF path).
 *   2. LIGHT sleep between reports (IDF power management / tickless idle). Because the ZED
 *      long-poll keeps running in light sleep, the coordinator never ages us out and the
 *      parent link never drops — so reports land instantly and there is NO rejoin race and
 *      NO settle/motion-hold window (all of which only existed to survive deep-sleep rejoin).
 *      Exception: after a FRESH join, an INTERVIEW_S window of fast polling lets deCONZ
 *      finish setting the sensor up (see interview_start()).
 *   3. Measure battery (Power Config cluster, reused ADC driver).
 *   4. Report ONE occupancy value ("awning protected") on the Occupancy Sensing cluster,
 *      on motion (INT1 GPIO wake) AND on a periodic heartbeat.
 *
 * ONE endpoint (10) carries Basic + Identify + Power Config + Occupancy Sensing. A single
 * endpoint means a single DDF fingerprint, so deCONZ instantiates the sensor cleanly on pair.
 *
 * Structure mirrors the vendor light_sleep_end_device example (same 2.x SDK, same hardware).
 * NOTE: the 2.x SDK has NO esp_zb_sleep_now()/CAN_SLEEP application hook (1.x only) — light
 * sleep is driven entirely by esp_pm_configure() + the CONFIG_PM_* flags in sdkconfig.
 * Built against esp-zigbee-lib 2.0.x.
 */

#include <stdint.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
#endif

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "awning_config.h"
#include "battery.h"
#include "adxl345.h"
#include "ui.h"

static const char *TAG = "AWNING_PROT";

/* Pairing / reset is controlled by the user button (see ui.c and the "Pairing & reset" section
 * below). The old FORCE_REJOIN build flag is gone: a 5 s button hold does the same thing. */

static esp_timer_handle_t s_steer_timer;    /* one-shot (re)commissioning retry */

/* The report task: woken by the heartbeat timeout OR a motion (INT1) notification; measures
 * occupancy + battery and reports. Nothing else drives reporting anymore. */
static TaskHandle_t s_report_task;
#define REPORT_NOTIFY_MOTION    0x01   /* set by the INT1 ISR */
#define REPORT_NOTIFY_JOINED    0x02   /* set by the signal handler on (re)join — baseline report */

/* "On a network" flag. Owned by the signal handler (runs in the stack context), read by the
 * report task — so the report task never has to query the ZBOSS stack itself (which would need
 * the stack lock and, worse, would fault if polled before esp_zigbee_init() runs). */
static volatile bool s_joined = false;

/* ------------------------------------------------------------------------------------- */
/* The one reported value                                                                 */
/* ------------------------------------------------------------------------------------- */
/* Set true once the ADXL345 is confirmed present at boot; gates the measurement below. */
static bool s_accel_ok = false;

/* Live sensitivity = ADXL345 THRESH_ACT raw units (62.5 mg/LSB). Seeded from NVS on boot,
 * defaulting to ACT_THRESH_DEFAULT when unset. Writable at runtime via ZCL attr 0x0012 -> the
 * write handler applies it to the ADXL live + persists it. RTC_DATA_ATTR is harmless belt-and-
 * braces (light sleep preserves RAM anyway). NVS load runs on cold boot. */
static RTC_DATA_ATTR uint8_t s_act_thresh = ACT_THRESH_DEFAULT;

/* Load persisted sensitivity from NVS into s_act_thresh (called once on cold boot, BEFORE
 * adxl345_configure so the ADXL comes up at the saved threshold). Leaves the default in place
 * if the key doesn't exist yet or NVS can't be opened. */
static void threshold_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(THRESH_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    /* Upper bound is the uint8 ceiling (THRESH_RAW_MAX == 255), so only the lower bound needs a
     * runtime check — a `v <= 255` test would be a dead comparison (-Werror=type-limits). */
    if (nvs_get_u8(h, THRESH_NVS_KEY, &v) == ESP_OK && v >= THRESH_RAW_MIN) {
        s_act_thresh = v;
        ESP_LOGI(TAG, "sensitivity loaded from NVS: THRESH_ACT raw=%u (~%u mg)", v, (unsigned)(v * 62.5f));
    }
    nvs_close(h);
}

/* Persist a new sensitivity (already range-checked by the caller). Best-effort: a failed write
 * only means it won't survive a *cold* boot; the RAM/RTC copy is still updated. */
static void threshold_save_to_nvs(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(THRESH_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "sensitivity NVS open failed — new value not persisted");
        return;
    }
    if (nvs_set_u8(h, THRESH_NVS_KEY, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/* Optional: measure one movement window and log the RMS energy (diagnostic only). The occupancy
 * DECISION is no longer taken from this — it comes from the ADXL ACTIVITY latch (see the report
 * task). We still expose the window so the runtime-tunable threshold stays meaningful for anyone
 * who wants to inspect movement strength. Not called on the fast motion path (too slow — a 4s
 * window averages a brief shake to nothing, which is exactly the bug that made this feel dead). */
static void awning_log_movement(void)
{
    if (!s_accel_ok) return;
    accel_metrics_t m;
    adxl345_set_odr50();
    adxl345_measure_window(&m);
    adxl345_set_odr25();
    ESP_LOGI(TAG, "movement window rms=%.1f mg (act thresh raw=%u ~%u mg)",
             m.movement_rms, s_act_thresh, (unsigned)(s_act_thresh * 62.5f));
}

/* ------------------------------------------------------------------------------------- */
/* Light sleep: IDF power management (tickless idle). The Zigbee stack sleeps + wakes the      */
/* radio automatically around its poll timer; we only enable PM here. See vendor example.      */
/* ------------------------------------------------------------------------------------- */
#ifdef CONFIG_PM_ENABLE
static esp_err_t light_sleep_pm_config(void)
{
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    int cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_pm_config_t pm = {
        .max_freq_mhz = cpu_mhz,
        .min_freq_mhz = cpu_mhz,
        .light_sleep_enable = true,
    };
    return esp_pm_configure(&pm);
#else
    return ESP_OK;
#endif
}
#endif /* CONFIG_PM_ENABLE */

/* ------------------------------------------------------------------------------------- */
/* Motion wake: ADXL345 INT1 (active-high). Two independent paths, both needed:               */
/*   - a normal GPIO edge ISR (POSEDGE) that nudges the report task while awake, and          */
/*   - EXT1 as the light-sleep WAKE source. We must use EXT1 (not esp_sleep_enable_gpio_       */
/*     wakeup) because sdkconfig enables CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP: the   */
/*     GPIO peripheral is powered down in sleep, so the plain GPIO-wakeup path is a silent     */
/*     no-op on the ESP32-H2. EXT1 lives in the always-on RTC domain. GPIO13 is RTC-capable,   */
/*     so it is a legal EXT1 pin. This mirrors the vendor light_sleep_end_device example.      */
/* ------------------------------------------------------------------------------------- */
static void IRAM_ATTR int1_isr(void *arg)
{
    BaseType_t hp_woken = pdFALSE;
    if (s_report_task) {
        xTaskNotifyFromISR(s_report_task, REPORT_NOTIFY_MOTION, eSetBits, &hp_woken);
    }
    if (hp_woken) portYIELD_FROM_ISR();
}

static esp_err_t motion_wake_config(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_INT1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, /* INT1 idles low, pulses high on activity */
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "int1 gpio_config");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_INT1, int1_isr, NULL), TAG, "isr add");

    /* Light-sleep wake: EXT1 on a HIGH level on INT1 (active-high). */
#ifdef CONFIG_PM_ENABLE
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_INT1, ESP_EXT1_WAKEUP_ANY_HIGH),
                        TAG, "ext1 wakeup");
#endif
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------- */
/* Reporting: occupancy (0x0406) + battery (0x0001) on the single endpoint.               */
/* ------------------------------------------------------------------------------------- */
static void report_occupancy(bool occupied)
{
    uint8_t occ = occupied ? EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_OCCUPIED
                           : EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_UNOCCUPIED;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(EP_AWNING, EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                           EZB_ZCL_STD_MANUF_CODE, &occ, false);
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = { .dst_addr.addr_mode = EZB_ADDR_MODE_NONE, .src_ep = EP_AWNING },
        .payload.attr_id = EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
    };
    ezb_zcl_report_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "occupancy=%d reported", occupied);
}

/* Update + report BatteryPercentageRemaining (0x0021, half-percent) on the Power Config
 * cluster. deCONZ does Attr.val/2 to get 0-100 %. */
static void report_battery(uint8_t soc_percent)
{
    uint8_t half_pct = (soc_percent > 100) ? 200 : (uint8_t)(soc_percent * 2);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(EP_AWNING, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                           EZB_ZCL_STD_MANUF_CODE, &half_pct, false);
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = { .dst_addr.addr_mode = EZB_ADDR_MODE_NONE, .src_ep = EP_AWNING },
        .payload.attr_id = EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
    };
    ezb_zcl_report_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "battery=%u%% reported", soc_percent);
}

static void report_battery_now(void)
{
    battery_reading_t b = battery_read(VBAT_RATIO);
    report_battery(b.soc);
}

/* Motion classifier: on a motion event, sample a short window and decide whether the motion
 * looks like a smooth motor open/close (return false — NOT wind) or chaotic wind (return true).
 * ALWAYS logs the raw metrics so they can be captured for tuning (see MOTION_CLASSIFIER_DESIGN.md
 * §6), regardless of whether CLASSIFIER_ENABLE gates the decision. Returns the "is wind" verdict:
 *   CLASSIFIER_ENABLE == 0  -> always true  (current always-latch behaviour; metrics still logged)
 *   CLASSIFIER_ENABLE == 1  -> false if the window looks smooth/one-directional, else true. */
static bool motion_is_wind(void)
{
    if (!s_accel_ok) return true; /* no accel data -> can't classify; fail safe to "wind" (report) */

    class_metrics_t m;
    adxl345_set_odr50();
    adxl345_classify_window(&m);
    adxl345_set_odr25();

    bool smooth = (m.reversals <= CLASS_REVERSALS_MAX) &&
                  (m.jerk      <= CLASS_JERK_MAX) &&
                  (m.directionality >= CLASS_DIRECTIONALITY_MIN ||
                   m.dom_ratio      >= CLASS_DOM_RATIO_MIN);

    /* Tuning log — capture these across real opens (remote + HA) and real wind to set thresholds. */
    ESP_LOGI(TAG, "classify: n=%u rms=%.1f rev=%u jerk=%.1f dir=%.2f dom=%.2f(ax%u) -> %s%s",
             m.samples, m.rms, m.reversals, m.jerk, m.directionality, m.dom_ratio, m.dom_axis,
             smooth ? "SMOOTH(open)" : "CHAOTIC(wind)",
             CLASSIFIER_ENABLE ? "" : " [classifier OFF: latching anyway]");

#if CLASSIFIER_ENABLE
    return !smooth;   /* smooth -> not wind (deliberate move) */
#else
    (void)smooth;
    return true;      /* classifier disabled -> preserve always-latch behaviour */
#endif
}

/* ------------------------------------------------------------------------------------- */
/* Report task: the single driver of reporting. Blocks on a task notification with a       */
/* HB_INTERVAL_S timeout, so it reports once per heartbeat AND immediately whenever the     */
/* INT1 ISR flags motion. Between notifications the CPU is free to light-sleep; the Zigbee  */
/* poll keeps the parent link alive so a report always finds a live coordinator. No settle  */
/* window / motion-hold is needed — those only existed to survive the deep-sleep rejoin gap. */
/* ------------------------------------------------------------------------------------- */
static void report_task(void *pv)
{
    ESP_LOGI(TAG, "report task live (poll %ds, clear %ds, heartbeat %ds)",
             MOTION_POLL_S, OCCUPANCY_CLEAR_S, HB_INTERVAL_S);

    /* Hang protection: this task wakes every MOTION_POLL_S, so it feeds the task watchdog on
     * every loop. If it ever gets stuck (I2C, stack lock, ...) for CONFIG_ESP_TASK_WDT_TIMEOUT_S
     * the watchdog panics and the chip reboots instead of sitting dead until someone presses RST. */
    esp_task_wdt_add(NULL);

    bool occupied = false;                 /* latched occupancy state we've reported */
    int64_t last_motion_us = 0;            /* esp_timer time of the most recent activity */
    int64_t last_hb_us = esp_timer_get_time();

    /* Move-grace state (classifier): while a burst keeps classifying as a smooth open/close we
     * hold occupancy 0. move_start_us marks when the current smooth burst began; if smooth
     * motion is still present after MOVE_MAX_S, a real open/close would be done -> it's wind. */
    bool move_active = false;
    int64_t move_start_us = 0;

    for (;;) {
        /* Short poll instead of one long block. This is what makes motion reliable under light
         * sleep: EXT1 wakes the silicon on INT1-high, but the GPIO edge ISR can be missed across
         * the sleep GPIO-isolation, so we do NOT depend on the notification — we poll the ADXL
         * activity latch ourselves each cycle. The CPU still light-sleeps between polls. */
        uint32_t note = 0;
        xTaskNotifyWait(0, UINT32_MAX, &note, pdMS_TO_TICKS((uint64_t)MOTION_POLL_S * 1000));
        esp_task_wdt_reset();

        /* User button shares this wakeup (IO25 can't wake light sleep on its own). */
        ui_tick();

        if (!s_joined) continue; /* not on a network yet — join signal re-triggers reporting */

        int64_t now = esp_timer_get_time();

        /* Baseline report right after (re)join so deCONZ instantiates a value immediately. */
        if (note & REPORT_NOTIFY_JOINED) {
            ESP_LOGI(TAG, "joined — baseline occupancy %d + battery", occupied);
            last_hb_us = now;
            report_occupancy(occupied);
            report_battery_now();
        }

        /* Consume the ADXL activity flag (this read also deasserts + re-arms INT1). Either the
         * ISR notified us (awake path) or EXT1 woke us (sleep path) or it's just a poll tick —
         * in every case the truth is in INT_SOURCE. */
        bool motion = s_accel_ok && adxl345_activity_fired();
        if (motion) last_motion_us = now;

        if (motion && !occupied) {
            /* Classify: smooth motor open/close, or chaotic wind? (Metrics always logged.) */
            bool wind = motion_is_wind();

            if (!wind) {
                /* Looks like a deliberate open/close. Hold occupancy 0 — UNLESS this smooth
                 * burst has already run longer than a full travel (MOVE_MAX_S), in which case a
                 * real move would be finished, so a still-present disturbance is wind after all. */
                if (!move_active) { move_active = true; move_start_us = now; }
                if ((now - move_start_us) >= (int64_t)MOVE_MAX_S * 1000000) {
                    ESP_LOGW(TAG, "smooth motion > %ds — treating as wind, occupancy 1", MOVE_MAX_S);
                    occupied = true;
                    report_occupancy(true);
                } else {
                    ESP_LOGI(TAG, "smooth motion — deliberate open/close, occupancy stays 0");
                }
            } else {
                occupied = true;
                move_active = false;
                ESP_LOGI(TAG, "motion — occupancy 1");
                report_occupancy(true);
            }
        } else if (!motion) {
            move_active = false; /* burst ended (clean stop) — reset the move-grace window */
            if (occupied &&
                (now - last_motion_us) >= (int64_t)OCCUPANCY_CLEAR_S * 1000000) {
                occupied = false;
                ESP_LOGI(TAG, "quiet %ds — occupancy 0", OCCUPANCY_CLEAR_S);
                report_occupancy(false);
            }
        }

        /* Heartbeat: re-assert occupancy + refresh battery on the slow cadence. */
        if ((now - last_hb_us) >= (int64_t)HB_INTERVAL_S * 1000000) {
            last_hb_us = now;
            ESP_LOGI(TAG, "heartbeat — occupancy %d + battery", occupied);
            awning_log_movement(); /* diagnostic: current movement RMS in the log */
            report_occupancy(occupied);
            report_battery_now();
        }
    }
}

static void start_report_task(void)
{
    if (!s_report_task) {
        xTaskCreate(report_task, "report", 4096, NULL, 4, &s_report_task);
    }
}

/* Kick a baseline report (used on fresh join so deCONZ sees a value right away). Safe before
 * the task exists — it just no-ops until start_report_task() has run. */
static void report_now(void)
{
    if (s_report_task) xTaskNotify(s_report_task, REPORT_NOTIFY_JOINED, eSetBits);
}

/* ------------------------------------------------------------------------------------- */
/* Commissioning retry (delayed, off the signal-handler stack — inline retry spins).      */
/* ------------------------------------------------------------------------------------- */
static uint8_t s_retry_mode = EZB_BDB_MODE_INITIALIZATION;

/* Rejoin backoff (already-paired device whose coordinator doesn't answer): fast at first,
 * then stretch out and cap so a battery device doesn't scan itself flat while the coordinator
 * is down. Reset to REJOIN_BACKOFF_MIN on a successful (re)join or a short button press.
 * Pairing of an UNPAIRED device does not use this; it runs in a time-limited window below. */
#define REJOIN_BACKOFF_MIN_S 3u
#define REJOIN_BACKOFF_MAX_S 60u
static uint32_t s_rejoin_backoff_s = REJOIN_BACKOFF_MIN_S;

static void commissioning_retry_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

/* Start `mode` commissioning after delay_s, off the signal-handler stack (inline retry spins). */
static void schedule_commissioning(uint8_t mode, uint32_t delay_s)
{
    s_retry_mode = mode;
    if (!s_steer_timer) {
        const esp_timer_create_args_t a = { .callback = commissioning_retry_cb, .name = "comm" };
        esp_timer_create(&a, &s_steer_timer);
    }
    esp_timer_stop(s_steer_timer);
    esp_timer_start_once(s_steer_timer, (uint64_t)delay_s * 1000000ULL);
}

/* Rejoin retry with backoff. Returns the delay (seconds) it scheduled, so callers can log it. */
static uint32_t schedule_commissioning_retry(uint8_t mode)
{
    uint32_t delay_s = s_rejoin_backoff_s;
    schedule_commissioning(mode, delay_s);

    /* Advance backoff: 3 → 6 → 12 → 24 → 48 → 60 (capped). */
    if (s_rejoin_backoff_s < REJOIN_BACKOFF_MAX_S) {
        s_rejoin_backoff_s = (s_rejoin_backoff_s < 6u) ? 6u : s_rejoin_backoff_s * 2u;
        if (s_rejoin_backoff_s > REJOIN_BACKOFF_MAX_S) s_rejoin_backoff_s = REJOIN_BACKOFF_MAX_S;
    }
    return delay_s;
}

/* ------------------------------------------------------------------------------------- */
/* Pairing & reset                                                                        */
/*                                                                                        */
/*  - Unpaired (factory-new) sensor: on power-up / RST it opens a PAIR_WINDOW_S pairing     */
/*    window (blue blink), steering every PAIR_RETRY_S on all channels. Joined -> green.    */
/*    Window runs out -> red, then idle (no scanning) until a short press or RST.          */
/*  - Short press: joined -> green flash + immediate report; unpaired -> (re)open window;  */
/*    paired but coordinator unreachable -> rejoin attempt now.                            */
/*  - Hold 5 s (LED fast red), release: factory reset. Sends a Zigbee leave if on a network,*/
/*    erases network data + sensitivity, reboots into a fresh pairing window.              */
/*  - Coordinator removes the device (leave without rejoin): wipe network data, reboot into */
/*    a fresh pairing window. Sensitivity is kept.                                         */
/*                                                                                        */
/* Functions marked [stack] run in the Zigbee stack context (signal handler, task-queue    */
/* callbacks, or the commissioning timer under the stack lock).                            */
/* ------------------------------------------------------------------------------------- */
static volatile bool s_stack_ready = false;   /* set at SKIP_STARTUP; gates UI -> stack posts */
static bool s_pair_open = false;              /* [stack] pairing window active */
static int64_t s_pair_deadline_us = 0;        /* [stack] esp_timer time the window closes */
static esp_timer_handle_t s_reset_fallback_timer;

/* [stack] Open (or extend) the pairing window. start_now = steer immediately, else after
 * PAIR_RETRY_S (used from a failure signal, where an inline restart would spin). */
static void pairing_open(bool start_now)
{
    s_pair_open = true;
    s_pair_deadline_us = esp_timer_get_time() + (int64_t)PAIR_WINDOW_S * 1000000;
    ui_led(UI_LED_PAIRING, 0);
    ESP_LOGI(TAG, "Pairing window open for %ds: enable Permit Join on the coordinator", PAIR_WINDOW_S);
    if (start_now) {
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    } else {
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, PAIR_RETRY_S);
    }
}

/* [stack] A steering attempt found no open network: retry inside the window, else close it. */
static void pairing_retry_or_close(uint8_t status)
{
    if (s_pair_open && esp_timer_get_time() < s_pair_deadline_us) {
        ESP_LOGI(TAG, "No open network yet (0x%02x); retry in %ds", status, PAIR_RETRY_S);
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, PAIR_RETRY_S);
        return;
    }
    s_pair_open = false;
    ui_led(UI_LED_FAILED, LED_FAILED_MS);
    ESP_LOGW(TAG, "Pairing window closed without a network. Press the button (or RST) to retry.");
}

/* ------------------------------------------------------------------------------------- */
/* Interview window after a fresh join (see INTERVIEW_S in awning_config.h): fast polling + */
/* no light sleep, so deCONZ's descriptor reads, binds and report config get answered fast. */
/* ------------------------------------------------------------------------------------- */
static esp_timer_handle_t s_interview_timer;
static bool s_interviewing = false;           /* [stack] */
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_interview_pm_lock;
#endif

/* [stack] */
static void interview_end_stack(void *ctx)
{
    if (!s_interviewing) return;
    s_interviewing = false;
    ezb_nwk_set_keepalive_interval(ZED_KEEP_ALIVE_MS);
#ifdef CONFIG_PM_ENABLE
    if (s_interview_pm_lock) esp_pm_lock_release(s_interview_pm_lock);
#endif
    if (s_joined) ui_led(UI_LED_JOINED, LED_JOINED_MS);
    ESP_LOGI(TAG, "Interview window over: back to %d ms polling + light sleep", ZED_KEEP_ALIVE_MS);
}

static void interview_timer_cb(void *arg)
{
    /* esp_timer context: the stack API must run in the Zigbee task. */
    esp_zigbee_task_queue_post(interview_end_stack, NULL);
}

/* [stack] */
static void interview_start(void)
{
    if (!s_interview_timer) {
        const esp_timer_create_args_t a = { .callback = interview_timer_cb, .name = "interview" };
        if (esp_timer_create(&a, &s_interview_timer) != ESP_OK) return;
    }
#ifdef CONFIG_PM_ENABLE
    if (!s_interview_pm_lock) {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "interview", &s_interview_pm_lock);
    }
    if (!s_interviewing && s_interview_pm_lock) esp_pm_lock_acquire(s_interview_pm_lock);
#endif
    s_interviewing = true;
    ezb_nwk_set_keepalive_interval(INTERVIEW_POLL_MS);
    ui_led(UI_LED_PAIRING, 0);
    esp_timer_stop(s_interview_timer);
    esp_timer_start_once(s_interview_timer, (uint64_t)INTERVIEW_S * 1000000ULL);
    ESP_LOGI(TAG, "Interview window: polling every %d ms for %ds so deCONZ can set the sensor up",
             INTERVIEW_POLL_MS, INTERVIEW_S);
}

/* [stack] Short press. */
static void short_press_stack(void *ctx)
{
    if (s_joined) {
        if (!s_interviewing) ui_led(UI_LED_ACK, LED_ACK_MS); /* keep the blue blink while interviewing */
        report_now(); /* fresh occupancy + battery: handy to check the link from the coordinator */
    } else if (ezb_bdb_is_factory_new()) {
        if (s_pair_open) {
            s_pair_deadline_us = esp_timer_get_time() + (int64_t)PAIR_WINDOW_S * 1000000;
            ui_led(UI_LED_PAIRING, 0);
            ESP_LOGI(TAG, "Pairing window extended to %ds", PAIR_WINDOW_S);
        } else {
            pairing_open(true);
        }
    } else {
        /* Paired, but the coordinator isn't answering: try a rejoin right now. */
        ESP_LOGI(TAG, "Rejoin requested by button");
        s_rejoin_backoff_s = REJOIN_BACKOFF_MIN_S;
        schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 1);
    }
}

/* [stack] Wipe all Zigbee data and reboot (does not return). */
static void factory_reset_stack(void *ctx)
{
    ESP_LOGW(TAG, "Factory reset: erasing Zigbee data and rebooting");
    esp_zigbee_factory_reset();
}

/* [stack] Proper leave: tells the coordinator, clears data, then signals LEAVE (type RESET),
 * whose handler finishes with factory_reset_stack(). */
static void local_reset_stack(void *ctx)
{
    ezb_bdb_reset_via_local_action();
}

/* Any context: wipe Zigbee data without a leave and reboot. */
static void factory_reset_now(void)
{
    if (esp_zigbee_task_queue_post(factory_reset_stack, NULL) != ESP_OK) {
        /* Stack not running: erase its storage directly. */
        nvs_flash_erase_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME);
        esp_restart();
    }
}

static void reset_fallback_cb(void *arg)
{
    ESP_LOGW(TAG, "No leave confirmation in %ds; resetting locally", FACTORY_RESET_FALLBACK_S);
    factory_reset_now();
}

/* Erase app settings (sensitivity) so a factory-reset sensor starts at the defaults. */
static void app_settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(THRESH_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* UI task context: button callbacks. Hand stack work to the Zigbee task queue. */
static void on_short_press(void)
{
    if (!s_stack_ready) return;
    esp_zigbee_task_queue_post(short_press_stack, NULL);
}

static void on_long_press(void)
{
    ui_led(UI_LED_RESETTING, 0);
    app_settings_erase();

    if (s_stack_ready && s_joined) {
        const esp_timer_create_args_t a = { .callback = reset_fallback_cb, .name = "rst" };
        if (!s_reset_fallback_timer) esp_timer_create(&a, &s_reset_fallback_timer);
        esp_timer_start_once(s_reset_fallback_timer, (uint64_t)FACTORY_RESET_FALLBACK_S * 1000000ULL);
        if (esp_zigbee_task_queue_post(local_reset_stack, NULL) == ESP_OK) return;
        esp_timer_stop(s_reset_fallback_timer);
    }
    /* Not on a network (or the post failed): no leave to send, reset straight away. */
    factory_reset_now();
}

/* ------------------------------------------------------------------------------------- */
/* Link loss: the parent stopped answering keep-alive polls (router unplugged / rebooted,     */
/* coordinator restarted, RF blocked). The stack does NOT find a new parent by itself, so     */
/* without this the sensor stayed "unavailable" until someone pressed RST. We do exactly what */
/* RST did: reboot. The boot path rejoins with stored keys (any parent on the channel) and     */
/* retries with backoff if the network is still down. Network data is NOT erased.             */
/* ------------------------------------------------------------------------------------- */
static esp_timer_handle_t s_link_loss_timer;

static void link_loss_reboot_cb(void *arg)
{
    ESP_LOGW(TAG, "Rebooting to rejoin after link loss");
    esp_restart();
}

/* [stack] */
static void link_lost(const char *why)
{
    if (s_link_loss_timer) return; /* reboot already pending */
    s_joined = false;
    ESP_LOGW(TAG, "Link to parent lost (%s): reboot + rejoin in %ds", why, LINK_LOSS_REBOOT_DELAY_S);
    const esp_timer_create_args_t a = { .callback = link_loss_reboot_cb, .name = "linkloss" };
    if (esp_timer_create(&a, &s_link_loss_timer) == ESP_OK) {
        esp_timer_start_once(s_link_loss_timer, (uint64_t)LINK_LOSS_REBOOT_DELAY_S * 1000000ULL);
    } else {
        esp_restart();
    }
}

/* ------------------------------------------------------------------------------------- */
/* ZCL attribute-write handler — lets deCONZ/HA retune sensitivity live.                    */
/* Sensitivity rides on Occupancy Sensing attr 0x0012 (PIRUnoccupiedToOccupiedThreshold,     */
/* uint8); its value IS the ADXL THRESH_ACT raw setting (62.5 mg/LSB). On write we clamp,     */
/* apply it to the ADXL immediately, and persist to NVS so it survives sleep + power-cycle.   */
/* ------------------------------------------------------------------------------------- */
static void zcl_action_handler(ezb_zcl_core_action_callback_id_t cb_id, void *message)
{
    if (!message) return;

    /* Identify "effect" command (e.g. deCONZ/Phoscon "identify"): short white blink. */
    if (cb_id == EZB_ZCL_CORE_IDENTIFY_EFFECT_CB_ID) {
        ui_led(UI_LED_IDENTIFY, 3000);
        return;
    }
    if (cb_id != EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID) return;

    const ezb_zcl_set_attr_value_message_t *m = message;

    /* Identify command sets IdentifyTime (seconds); 0 stops identifying. */
    if (m->info.cluster_id == EZB_ZCL_CLUSTER_ID_IDENTIFY &&
        m->in.attribute.id == EZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID &&
        m->in.attribute.data.value) {
        uint16_t t = *(const uint16_t *)m->in.attribute.data.value;
        ui_led(t ? UI_LED_IDENTIFY : UI_LED_OFF, (uint32_t)t * 1000);
        return;
    }

    if (m->info.cluster_id != EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING ||
        m->in.attribute.id != EZB_ZCL_ATTR_OCCUPANCY_SENSING_PIR_UNOCCUPIED_TO_OCCUPIED_THRESHOLD_ID ||
        !m->in.attribute.data.value) {
        return; /* not our knob — leave the stack's default handling in place */
    }

    uint8_t v = *(const uint8_t *)m->in.attribute.data.value;
    if (v < THRESH_RAW_MIN) v = THRESH_RAW_MIN; /* upper clamp unneeded: v is uint8, max == 255 */

    s_act_thresh = v;
    if (s_accel_ok) adxl345_set_act_thresh(v);   /* apply to hardware right now */
    threshold_save_to_nvs(v);
    ESP_LOGI(TAG, "sensitivity set to THRESH_ACT raw=%u (~%u mg, persisted)",
             v, (unsigned)(v * 62.5f));
}

/* ------------------------------------------------------------------------------------- */
/* Commissioning signal handler — rejoin lives in DEVICE_REBOOT (see vendor example).     */
/* ------------------------------------------------------------------------------------- */
static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t sig = ezb_app_signal_get_type(app_signal);

    switch (sig) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        s_stack_ready = true;
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Unpaired device: opening pairing window");
                pairing_open(true);
            } else {
                ESP_LOGI(TAG, "Device rebooted — already joined; report task will report + light-sleep");
                s_rejoin_backoff_s = REJOIN_BACKOFF_MIN_S;
                s_joined = true;
                ui_led(UI_LED_JOINED, LED_JOINED_MS); /* e.g. after a battery swap: "I'm back" */
                report_now();
            }
        } else if (!ezb_bdb_is_factory_new()) {
            /* ALREADY JOINED but INITIALIZATION failed (0x03). This is the cold-power-on
             * case: the device is a member of a network (keys are in zb_storage) but the
             * first rejoin attempt didn't reach the coordinator — typical for a sleepy ED
             * whose parent aged it out during deep sleep. The recovery is to retry
             * INITIALIZATION (a proper rejoin using the stored keys), NOT steering.
             * Escalating to STEERING here would loop forever, because the device is already
             * commissioned and there is no open Permit Join to steer into. */
            uint32_t d = schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION);
            ESP_LOGW(TAG, "Rejoin failed (0x%02x) — already joined, coordinator didn't answer; "
                          "retry rejoin in %us (channel mask 0x%08lx)",
                     status, (unsigned)d, (unsigned long)ZB_CHANNEL_MASK);
        } else {
            /* FACTORY-NEW and init failed: no network yet. Pair inside the time-limited window
             * (steering is scheduled, not started inline, so a failure can't spin). */
            ESP_LOGW(TAG, "Init failed (0x%02x) on an unpaired device", status);
            if (s_pair_open) pairing_retry_or_close(status);
            else             pairing_open(false);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Joined: PAN 0x%04hx, channel %d, short 0x%04hx",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            s_rejoin_backoff_s = REJOIN_BACKOFF_MIN_S;
            s_pair_open = false;
            s_joined = true;
            /* FRESH JOIN: poll fast and stay awake for INTERVIEW_S so deCONZ can finish its
             * descriptor reads, binds and report config (at the normal 4 s poll it can time out
             * and leave the sensor half set up). LED stays blue, then green. Report right away
             * so deCONZ instantiates a value. */
            interview_start();
            report_now();
        } else {
            pairing_retry_or_close(status);
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        s_joined = false;
        const ezb_zdo_signal_leave_params_t *lp = ezb_app_signal_get_params(app_signal);
        if (lp && lp->leave_type == EZB_ZDO_LEAVE_TYPE_REJOIN) {
            /* Leave-with-rejoin: the stack rejoins on its own and re-signals on success. */
            ESP_LOGW(TAG, "Left network (rejoin requested) — pausing reports until rejoin");
            break;
        }
        /* Leave without rejoin: either our own button reset finished, or the coordinator
         * removed us (deCONZ delete). Wipe all network data and reboot; the sensor comes back
         * factory-new with a fresh pairing window. */
        if (s_reset_fallback_timer) esp_timer_stop(s_reset_fallback_timer);
        ESP_LOGW(TAG, "Left network (no rejoin) — resetting to unpaired");
        ui_led(UI_LED_RESETTING, 0);
        factory_reset_stack(NULL);
    } break;

    case EZB_NWK_SIGNAL_NETWORK_STATUS: {
        const ezb_nwk_signal_network_status_params_t *ns = ezb_app_signal_get_params(app_signal);
        uint8_t st = ns ? ns->status : 0xff;
        ESP_LOGW(TAG, "NWK status 0x%02x (addr 0x%04hx)", st, ns ? ns->network_addr : 0);
        if (s_joined && st == EZB_NWK_NETWORK_STATUS_PARENT_LINK_FAILURE) link_lost("parent link failure");
    } break;

    case EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        if (s_joined) link_lost("no active links left");
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%02x)", ezb_app_signal_to_string(sig), sig);
        break;
    }
    return true;
}

/* ------------------------------------------------------------------------------------- */
/* Device construction: ONE endpoint, Basic + Identify + Power Config + Occupancy.        */
/* ------------------------------------------------------------------------------------- */
static ezb_af_ep_desc_t make_awning_endpoint(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id = EP_AWNING,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = OCCUPANCY_DEVICE_ID, /* 0x0107 occupancy sensor — matches the DDF */
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);

    /* Basic (manufacturer / model — the DDF fingerprint matches on cluster 0x0000). */
    ezb_zcl_cluster_desc_t basic =
        ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)ZB_MANUFACTURER_ZCL);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)ZB_MODEL_ZCL);
    ezb_af_endpoint_add_cluster_desc(ep, basic);

    /* Identify (0x0003) — standard for a commissionable HA device. */
    ezb_zcl_cluster_desc_t identify =
        ezb_zcl_identify_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    ezb_af_endpoint_add_cluster_desc(ep, identify);

    /* Power Config (0x0001) — battery percentage. */
    ezb_zcl_cluster_desc_t pwr =
        ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    uint8_t batt0 = 0;
    ezb_zcl_power_config_cluster_desc_add_attr(
        pwr, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &batt0);
    ezb_af_endpoint_add_cluster_desc(ep, pwr);

    /* Occupancy Sensing (0x0406) — the ONE reported value -> deCONZ ZHAPresence. */
    ezb_zcl_occupancy_sensing_cluster_server_config_t occ_cfg = {
        .occupancy = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_UNOCCUPIED,
        .occupancy_sensor_type = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PHYSICAL_CONTACT,
        .occupancy_sensor_type_bitmap = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_PHYSICAL_CONTACT,
    };
    ezb_zcl_cluster_desc_t occ =
        ezb_zcl_occupancy_sensing_create_cluster_desc(&occ_cfg, EZB_ZCL_CLUSTER_SERVER);
    /* Runtime-tunable sensitivity: PIRUnoccupiedToOccupiedThreshold (0x0012, uint8) carries the
     * ADXL THRESH_ACT raw value (62.5 mg/LSB). Seed from the live value (already loaded from NVS
     * on boot) so a reader sees the persisted setting. Writes land in zcl_action_handler(). */
    ezb_zcl_occupancy_sensing_cluster_desc_add_attr(
        occ, EZB_ZCL_ATTR_OCCUPANCY_SENSING_PIR_UNOCCUPIED_TO_OCCUPIED_THRESHOLD_ID,
        &s_act_thresh);
    ezb_af_endpoint_add_cluster_desc(ep, occ);

    return ep;
}

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, make_awning_endpoint()),
                        TAG, "add awning ep %u", EP_AWNING);
    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev), TAG, "register device");
    return ESP_OK;
}

static esp_err_t setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_RETURN_ON_ERROR(ezb_bdb_set_primary_channel_set(ZB_CHANNEL_MASK), TAG, "primary ch");
    ESP_RETURN_ON_ERROR(ezb_app_signal_add_handler(app_signal_handler), TAG, "signal handler");
    ezb_zcl_core_action_handler_register(zcl_action_handler); /* threshold writes from deCONZ/HA */
    ezb_nwk_set_rx_on_when_idle(false); /* sleepy ED */
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------- */
/* Zigbee task + app_main                                                                 */
/* ------------------------------------------------------------------------------------- */
static void zigbee_main_task(void *pv)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(setup_commissioning());
    ESP_ERROR_CHECK(create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false)); /* false = keep stored network */
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* Why did we (re)boot? Field diagnostics: PANIC / TASK_WDT = firmware fault, SW = our own
     * reboot (link loss / reset), POWERON / EXT = battery insert / RST. */
    ESP_LOGW(TAG, "boot: reset reason %d", (int)esp_reset_reason());

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME));

    /* Sensitivity: NVS is the source of truth. Load it before the ADXL is configured (so it
     * comes up at the saved THRESH_ACT) and before the endpoint is built (so attr 0x0012 seeds
     * with the persisted value). */
    threshold_load_from_nvs();

    /* Accelerometer up on every boot (I2C). Non-fatal: if it fails, occupancy just reads
     * "not protected" rather than aborting the whole boot. Configure at the persisted (or
     * default) sensitivity. */
    if (adxl345_init() == ESP_OK) {
        adxl345_configure(s_act_thresh);
        s_accel_ok = true;
    } else {
        ESP_LOGW(TAG, "ADXL345 init failed — occupancy will read unoccupied");
    }

    /* Battery ADC (non-fatal: if it fails we just report 0% rather than aborting boot). */
    if (battery_init() != ESP_OK) ESP_LOGW(TAG, "battery ADC init failed");

    /* Motion wake: INT1 -> GPIO ISR (nudges the report task) + light-sleep GPIO wakeup. */
    ESP_ERROR_CHECK(motion_wake_config());

    /* Enable IDF light sleep. From here the CPU tickless-idles between work; the Zigbee stack
     * parks/wakes the radio around its poll timer, keeping the parent link alive. */
#ifdef CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(light_sleep_pm_config());
#endif

    /* User button + status LED (pairing / reset control). Non-fatal if the LED fails. */
    const ui_callbacks_t ui_cb = { .on_short_press = on_short_press, .on_long_press = on_long_press };
    if (ui_init(&ui_cb) != ESP_OK) ESP_LOGW(TAG, "button/LED init failed");

    /* Report task: heartbeat + motion-driven reporting. Starts blocked until joined. */
    start_report_task();

    ESP_LOGI(TAG, "Start ESP Zigbee stack (channel mask 0x%08lx, light sleep)",
             (unsigned long)ZB_CHANNEL_MASK);
    xTaskCreate(zigbee_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
