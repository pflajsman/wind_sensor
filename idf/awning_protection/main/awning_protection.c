/* awning_protection.c — ESP32-H2 / esp-zigbee-sdk (2.x ezb_* API) LIGHT-SLEEP end device.
 *
 * SCOPE (deliberately small): solve exactly four things and nothing else.
 *   1. Join / rejoin a deCONZ network (channel-pinned, native IDF path).
 *   2. LIGHT sleep between reports (IDF power management / tickless idle). Because the ZED
 *      long-poll keeps running in light sleep, the coordinator never ages us out and the
 *      parent link never drops — so reports land instantly and there is NO rejoin race and
 *      NO settle/motion-hold window (all of which only existed to survive deep-sleep rejoin).
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

static const char *TAG = "AWNING_PROT";

/* Set FORCE_REJOIN to 1 to make the device factory-reset + re-pair on EVERY boot, to force a
 * clean join into deCONZ (open Permit Join, flash this, let it join), then set back to 0 and
 * re-flash for normal operation. Do NOT ship with it on. */
#define FORCE_REJOIN 0

static esp_timer_handle_t s_steer_timer;    /* one-shot retry of network steering on failure */

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

    bool occupied = false;                 /* latched occupancy state we've reported */
    int64_t last_motion_us = 0;            /* esp_timer time of the most recent activity */
    int64_t last_hb_us = esp_timer_get_time();

    for (;;) {
        /* Short poll instead of one long block. This is what makes motion reliable under light
         * sleep: EXT1 wakes the silicon on INT1-high, but the GPIO edge ISR can be missed across
         * the sleep GPIO-isolation, so we do NOT depend on the notification — we poll the ADXL
         * activity latch ourselves each cycle. The CPU still light-sleeps between polls. */
        uint32_t note = 0;
        xTaskNotifyWait(0, UINT32_MAX, &note, pdMS_TO_TICKS((uint64_t)MOTION_POLL_S * 1000));

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
            occupied = true;
            ESP_LOGI(TAG, "motion — occupancy 1");
            report_occupancy(true);
        } else if (occupied && !motion &&
                   (now - last_motion_us) >= (int64_t)OCCUPANCY_CLEAR_S * 1000000) {
            occupied = false;
            ESP_LOGI(TAG, "quiet %ds — occupancy 0", OCCUPANCY_CLEAR_S);
            report_occupancy(false);
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

/* Steer-retry backoff: fast at first so a freshly-opened Permit Join lands within a few
 * seconds, then stretch out and cap so an un-joined *battery* device doesn't scan itself
 * flat while it waits for a network to open. Reset to STEER_BACKOFF_MIN on a real join. */
#define STEER_BACKOFF_MIN_S 3u
#define STEER_BACKOFF_MAX_S 60u
static uint32_t s_steer_backoff_s = STEER_BACKOFF_MIN_S;

static void commissioning_retry_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

/* Returns the delay (seconds) it scheduled, so callers can log it. */
static uint32_t schedule_commissioning_retry(uint8_t mode)
{
    s_retry_mode = mode;
    if (!s_steer_timer) {
        const esp_timer_create_args_t a = { .callback = commissioning_retry_cb, .name = "comm" };
        esp_timer_create(&a, &s_steer_timer);
    }
    uint32_t delay_s = s_steer_backoff_s;
    esp_timer_stop(s_steer_timer);
    esp_timer_start_once(s_steer_timer, (uint64_t)delay_s * 1000000ULL);

    /* Advance backoff: 3 → 6 → 12 → 24 → 48 → 60 (capped). */
    if (s_steer_backoff_s < STEER_BACKOFF_MAX_S) {
        s_steer_backoff_s = (s_steer_backoff_s < 6u) ? 6u : s_steer_backoff_s * 2u;
        if (s_steer_backoff_s > STEER_BACKOFF_MAX_S) s_steer_backoff_s = STEER_BACKOFF_MAX_S;
    }
    return delay_s;
}

/* ------------------------------------------------------------------------------------- */
/* ZCL attribute-write handler — lets deCONZ/HA retune sensitivity live.                    */
/* Sensitivity rides on Occupancy Sensing attr 0x0012 (PIRUnoccupiedToOccupiedThreshold,     */
/* uint8); its value IS the ADXL THRESH_ACT raw setting (62.5 mg/LSB). On write we clamp,     */
/* apply it to the ADXL immediately, and persist to NVS so it survives sleep + power-cycle.   */
/* ------------------------------------------------------------------------------------- */
static void zcl_action_handler(ezb_zcl_core_action_callback_id_t cb_id, void *message)
{
    if (cb_id != EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID || !message) return;

    const ezb_zcl_set_attr_value_message_t *m = message;
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
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Fresh device: network steering (open Permit Join)");
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else if (FORCE_REJOIN) {
                ESP_LOGW(TAG, "FORCE_REJOIN build flag — factory reset, will re-join fresh");
                ezb_bdb_reset_via_local_action();
            } else {
                ESP_LOGI(TAG, "Device rebooted — already joined; report task will report + light-sleep");
                s_joined = true;
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
            /* FACTORY-NEW and init failed: genuinely no network to join yet. Keep STEERING
             * until deCONZ opens Permit Join on our channel. */
            uint32_t d = schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING);
            ESP_LOGW(TAG, "Init/join failed (0x%02x=NO_NETWORK) — no open network on scanned "
                          "channels (mask 0x%08lx); retry steering in %us. Check deCONZ "
                          "Permit Join + channel.",
                     status, (unsigned long)ZB_CHANNEL_MASK, (unsigned)d);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Joined: PAN 0x%04hx, channel %d, short 0x%04hx",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            s_steer_backoff_s = STEER_BACKOFF_MIN_S; /* joined — next un-join starts fast again */
            s_joined = true;
            /* FRESH JOIN: report right away so deCONZ instantiates a value. No settle window is
             * needed — in light sleep the link stays up, so deCONZ's descriptor read + bind +
             * report-config all land against a reachable device on its own schedule. */
            report_now();
        } else {
            uint32_t d = schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING);
            ESP_LOGW(TAG, "Steering failed (0x%02x=NO_NETWORK) — no open network on scanned "
                          "channels (mask 0x%08lx); retry in %us. Open deCONZ Permit Join.",
                     status, (unsigned long)ZB_CHANNEL_MASK, (unsigned)d);
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE:
        /* Removed from the network (e.g. deCONZ delete). Stop reporting until we rejoin;
         * the stack will re-run steering/init and re-set s_joined on success. */
        s_joined = false;
        ESP_LOGW(TAG, "Left network — pausing reports until rejoin");
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

    /* Report task: heartbeat + motion-driven reporting. Starts blocked until joined. */
    start_report_task();

    ESP_LOGI(TAG, "Start ESP Zigbee stack (channel %d, light sleep)", ZB_CHANNEL);
    xTaskCreate(zigbee_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
