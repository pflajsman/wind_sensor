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
#include "sys/time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "awning_config.h"
#include "battery.h"

static const char *TAG = "AWNING_PROT";

/* Set FORCE_REJOIN to 1 to make the device factory-reset + re-pair on EVERY boot, to force a
 * clean join into deCONZ (open Permit Join, flash this, let it join), then set back to 0 and
 * re-flash for normal operation. Do NOT ship with it on. */
#define FORCE_REJOIN 1

/* Wall-clock across deep sleep, like the vendor example. */
static RTC_DATA_ATTR struct timeval s_sleep_time;
static esp_timer_handle_t s_sleep_timer;
static esp_timer_handle_t s_report_timer;   /* repeating re-report during the settle window */
static esp_timer_handle_t s_steer_timer;    /* one-shot retry of network steering on failure */

/* ------------------------------------------------------------------------------------- */
/* The one reported value                                                                 */
/* ------------------------------------------------------------------------------------- */
/* Awning protection state. Replace this stub with the real measurement (e.g. accel/tilt
 * threshold). Kept trivial so the join/sleep/battery/report pipeline can be validated first. */
static bool awning_is_protected(void)
{
    return AWNING_OCCUPANCY_DEFAULT;
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

    /* Timer heartbeat only — no GPIO wake source in this minimal build. */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_timer_wakeup((uint64_t)HB_INTERVAL_S * 1000000ULL),
                        TAG, "timer wakeup");
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

static void report_all_once(void)
{
    report_occupancy(awning_is_protected());
    battery_reading_t b = battery_read(VBAT_RATIO);
    report_battery(b.soc);
}

/* ------------------------------------------------------------------------------------- */
/* Settle window: keep polling so deCONZ can service its queued reads/binds while awake.   */
/* ------------------------------------------------------------------------------------- */
static void periodic_report_cb(void *arg)
{
    if (ezb_bdb_dev_joined()) report_all_once();
}

static void report_and_sleep(uint32_t settle_seconds)
{
    report_all_once();

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

static void commissioning_retry_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

static void schedule_commissioning_retry(uint8_t mode)
{
    s_retry_mode = mode;
    if (!s_steer_timer) {
        const esp_timer_create_args_t a = { .callback = commissioning_retry_cb, .name = "comm" };
        esp_timer_create(&a, &s_steer_timer);
    }
    esp_timer_stop(s_steer_timer);
    esp_timer_start_once(s_steer_timer, 3 * 1000000ULL);
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
        } else {
            ESP_LOGW(TAG, "Commissioning init failed (0x%02x) — retrying in 3s", status);
            schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Joined: PAN 0x%04hx, channel %d, short 0x%04hx",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            /* FRESH JOIN: stay awake so deCONZ can read our simple descriptor + write bindings. */
            report_and_sleep(FRESH_JOIN_SETTLE_S);
        } else {
            ESP_LOGW(TAG, "Steering failed (0x%02x) — retrying in 3s", status);
            schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING);
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

    /* Battery ADC (non-fatal: if it fails we just report 0% rather than aborting boot). */
    if (battery_init() != ESP_OK) ESP_LOGW(TAG, "battery ADC init failed");

    ESP_ERROR_CHECK(deep_sleep_wakeup_config());

    ESP_LOGI(TAG, "Start ESP Zigbee stack (channel %d)", ZB_CHANNEL);
    xTaskCreate(zigbee_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
