/* awning_protection.c — minimal ESP32-H2 / esp-zigbee-sdk (2.x ezb_* API) sleepy end device.
 *
 * SCOPE (deliberately small): solve exactly four things and nothing else.
 *   1. Join / rejoin a deCONZ network (channel-pinned, native IDF rejoin path).
 *   2. Deep sleep between wakes (timer heartbeat).
 *   3. Measure battery (Power Config cluster, reused ADC driver).
 *   4. Report ONE occupancy value ("awning protected") on the Occupancy Sensing cluster.
 *
 * ONE endpoint (10) carries Basic + Identify + Power Config + Occupancy Sensing. A single
 * endpoint means a single DDF fingerprint, so deCONZ instantiates the sensor cleanly on pair
 * (the multi-endpoint design is what half-instantiated before).
 *
 * Structure mirrors the vendor deep_sleep_end_device example, which is proven to rejoin on
 * this exact hardware after a deep-sleep wake. Built against esp-zigbee-lib 2.0.x.
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sys/time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* Wall-clock across deep sleep, like the vendor example. */
static RTC_DATA_ATTR struct timeval s_sleep_time;
static esp_timer_handle_t s_sleep_timer;
static esp_timer_handle_t s_report_timer;   /* repeating re-report during the settle window */
static esp_timer_handle_t s_steer_timer;    /* one-shot retry of network steering on failure */

/* ------------------------------------------------------------------------------------- */
/* The one reported value                                                                 */
/* ------------------------------------------------------------------------------------- */
/* Set true once the ADXL345 is confirmed present at boot; gates the measurement below. */
static bool s_accel_ok = false;

/* Live occupancy threshold (movement_rms mg). Seeded from NVS on boot, defaulting to
 * OCCUPANCY_MG_THRESHOLD when unset. Writable at runtime via ZCL attr 0x0012 -> the write
 * handler updates this + persists it. RTC_DATA_ATTR so it also survives a deep-sleep wake
 * without an NVS read on the fast path (NVS load still runs on cold boot). */
static RTC_DATA_ATTR uint8_t s_occ_thresh_mg = OCCUPANCY_MG_THRESHOLD;

/* Load persisted threshold from NVS into s_occ_thresh_mg (called once on cold boot). Leaves
 * the default in place if the key doesn't exist yet or NVS can't be opened. */
static void threshold_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(THRESH_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, THRESH_NVS_KEY, &v) == ESP_OK && v >= THRESH_MG_MIN && v <= THRESH_MG_MAX) {
        s_occ_thresh_mg = v;
        ESP_LOGI(TAG, "threshold loaded from NVS: %u mg", v);
    }
    nvs_close(h);
}

/* Persist a new threshold (already range-checked by the caller). Best-effort: a failed write
 * only means it won't survive a *cold* boot; the RAM/RTC copy is still updated. */
static void threshold_save_to_nvs(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(THRESH_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "threshold NVS open failed — new value not persisted");
        return;
    }
    if (nvs_set_u8(h, THRESH_NVS_KEY, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/* Awning protection state, measured from the ADXL345. Capture one movement window and return
 * occupied when the AC-energy movement (movement_rms) crosses the threshold. ONLY this boolean
 * is reported over Zigbee — the analog metrics stay on-device (the DDF exposes presence only).
 * Re-arms activity detection afterwards so the next movement re-asserts INT1 (EXT1 wake). */
static bool awning_is_protected(void)
{
    if (!s_accel_ok) return false; /* no accelerometer -> report unoccupied */

    accel_metrics_t m;
    adxl345_set_odr50();
    adxl345_measure_window(&m);
    adxl345_set_odr25();
    adxl345_rearm_activity();

    bool occupied = (m.movement_rms >= (float)s_occ_thresh_mg);
    ESP_LOGI(TAG, "occupancy=%d (rms=%.1f mg, thresh=%u mg)",
             occupied, m.movement_rms, s_occ_thresh_mg);
    return occupied;
}

/* ------------------------------------------------------------------------------------- */
/* Deep sleep                                                                            */
/* ------------------------------------------------------------------------------------- */
static void deep_sleep_now_cb(void *arg)
{
    if (s_report_timer) esp_timer_stop(s_report_timer); /* end the settle re-reporting */
    ESP_LOGI(TAG, "Entering deep sleep for %d s", HB_INTERVAL_S);
    gettimeofday(&s_sleep_time, NULL);
    esp_deep_sleep_start();
}

static esp_err_t deep_sleep_wakeup_config(void)
{
    const esp_timer_create_args_t args = { .callback = &deep_sleep_now_cb, .name = "sleep" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_sleep_timer), TAG, "timer create");

    /* Timer heartbeat + EXT1 wake on the accel INT1 (active-high). Movement asserts INT1 and
     * wakes the device to re-measure + report occupancy — this is what makes it "react". */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_timer_wakeup((uint64_t)HB_INTERVAL_S * 1000000ULL),
                        TAG, "timer wakeup");
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup(EXT1_IO_MASK, ESP_EXT1_WAKEUP_ANY_HIGH),
                        TAG, "ext1 wakeup");
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

/* Last occupancy we reported this awake period — lets the settle loop decide whether to hold
 * the window open (motion still active) or let it close (clean stop already reported). */
static bool s_last_occupied;

static void report_all_once(void)
{
    s_last_occupied = awning_is_protected();
    report_occupancy(s_last_occupied);
    battery_reading_t b = battery_read(VBAT_RATIO);
    report_battery(b.soc);
}

/* ------------------------------------------------------------------------------------- */
/* Settle window: keep polling so deCONZ can service its queued reads/binds while awake.   */
/* ------------------------------------------------------------------------------------- */
/* Monotonic-ish seconds since this awake period began, from esp_timer (survives within a wake,
 * unaffected by the RTC wall-clock used for sleep bookkeeping). */
static int64_t s_awake_start_us;

/* True for a routine (SHORT_SETTLE) wake, where an unoccupied reading may cut the settle short.
 * False for a FRESH_JOIN settle, which always runs its full window so deCONZ can bind + report-
 * config regardless of occupancy. */
static bool s_may_early_sleep;

static void periodic_report_cb(void *arg)
{
    if (!ezb_bdb_dev_joined()) return;
    report_all_once();

    uint32_t awake_s = (uint32_t)((esp_timer_get_time() - s_awake_start_us) / 1000000);

    if (s_last_occupied) {
        /* Motion-hold: if movement is still active, keep the window open so we can catch and
         * report the eventual stop. Without this, we'd sleep at occupancy=1 and HA would stay
         * "protected" until the 30-min heartbeat, because motion *stopping* doesn't wake us. */
        if (awake_s + SHORT_SETTLE_S < MOTION_HOLD_MAX_S) {
            esp_timer_stop(s_sleep_timer);
            esp_timer_start_once(s_sleep_timer, (uint64_t)SHORT_SETTLE_S * 1000000ULL);
            ESP_LOGI(TAG, "motion still active (%us awake) — holding window open", awake_s);
        } else {
            ESP_LOGW(TAG, "motion still active at %us hold cap — sleeping anyway", awake_s);
        }
    } else if (s_may_early_sleep && awake_s >= QUIET_GRACE_S) {
        /* Quiet routine wake: reading is below threshold, so there is no motion-stop to wait
         * for. We've given deCONZ one grace cycle (QUIET_GRACE_S) to flush any queued read/
         * write; sleep now instead of holding the radio on for the rest of SHORT_SETTLE_S. */
        esp_timer_stop(s_sleep_timer);
        esp_timer_start_once(s_sleep_timer, 1); /* fire ASAP -> deep_sleep_now_cb */
        ESP_LOGI(TAG, "unoccupied at %us — sleeping early (skipping rest of settle)", awake_s);
    }
}

static void report_and_sleep(uint32_t settle_seconds)
{
    report_all_once();

    s_awake_start_us = esp_timer_get_time();
    s_may_early_sleep = (settle_seconds < FRESH_JOIN_SETTLE_S); /* routine wake only */
    ESP_LOGI(TAG, "settling awake %us (deCONZ read/bind/report-config)", settle_seconds);
    if (!s_report_timer) {
        const esp_timer_create_args_t ra = { .callback = periodic_report_cb, .name = "rep" };
        esp_timer_create(&ra, &s_report_timer);
    }
    esp_timer_start_periodic(s_report_timer, 5 * 1000000ULL);

    ESP_ERROR_CHECK(esp_timer_start_once(s_sleep_timer, (uint64_t)settle_seconds * 1000000ULL));
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
/* ZCL attribute-write handler — lets deCONZ/HA retune the occupancy threshold live.       */
/* The threshold rides on Occupancy Sensing attr 0x0012 (PIRUnoccupiedToOccupiedThreshold,  */
/* uint8); its value IS the movement_rms mg threshold. On write we clamp, update the live   */
/* copy, and persist to NVS so it survives sleep + power-cycle.                             */
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
    if (v < THRESH_MG_MIN) v = THRESH_MG_MIN;
    if (v > THRESH_MG_MAX) v = THRESH_MG_MAX;

    s_occ_thresh_mg = v;
    threshold_save_to_nvs(v);
    ESP_LOGI(TAG, "occupancy threshold set to %u mg (persisted)", v);
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
                ESP_LOGI(TAG, "Device rebooted — already joined, reporting + sleeping");
                report_and_sleep(SHORT_SETTLE_S);
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
            /* FRESH JOIN: stay awake so deCONZ can read our simple descriptor + write bindings. */
            report_and_sleep(FRESH_JOIN_SETTLE_S);
        } else {
            uint32_t d = schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING);
            ESP_LOGW(TAG, "Steering failed (0x%02x=NO_NETWORK) — no open network on scanned "
                          "channels (mask 0x%08lx); retry in %us. Open deCONZ Permit Join.",
                     status, (unsigned long)ZB_CHANNEL_MASK, (unsigned)d);
        }
    } break;

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
    /* Runtime-tunable threshold: PIRUnoccupiedToOccupiedThreshold (0x0012, uint8) carries the
     * movement_rms mg threshold. Seed from the live value (already loaded from NVS on boot) so
     * a reader sees the persisted setting. Writes land in zcl_action_handler(). */
    ezb_zcl_occupancy_sensing_cluster_desc_add_attr(
        occ, EZB_ZCL_ATTR_OCCUPANCY_SENSING_PIR_UNOCCUPIED_TO_OCCUPIED_THRESHOLD_ID,
        &s_occ_thresh_mg);
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

    /* Occupancy threshold: NVS is the source of truth. Load it before the endpoint is built
     * so attr 0x0012 seeds with the persisted value and awning_is_protected() uses it. */
    threshold_load_from_nvs();

    /* Accelerometer up on every boot (I2C). Non-fatal: if it fails, occupancy just reads
     * "not protected" rather than aborting the whole boot. */
    if (adxl345_init() == ESP_OK) {
        adxl345_configure(ACT_THRESH_DEFAULT);
        s_accel_ok = true;
    } else {
        ESP_LOGW(TAG, "ADXL345 init failed — occupancy will read unoccupied");
    }

    /* Battery ADC (non-fatal: if it fails we just report 0% rather than aborting boot). */
    if (battery_init() != ESP_OK) ESP_LOGW(TAG, "battery ADC init failed");

    ESP_ERROR_CHECK(deep_sleep_wakeup_config());

    ESP_LOGI(TAG, "Start ESP Zigbee stack (channel %d)", ZB_CHANNEL);
    xTaskCreate(zigbee_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
