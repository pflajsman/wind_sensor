/* awning_wind_sensor.c — native ESP-IDF / esp-zigbee-sdk (2.x ezb_* API) port.
 *
 * WHY: the arduino-esp32 Zigbee wrapper could not rejoin the deCONZ network after a
 * deep-sleep wake (begin() hung 30 s). The vendor deep_sleep_end_device example PROVED on
 * this exact hardware that native IDF rejoins correctly (wake -> "Device rebooted" -> work
 * -> re-sleep, network closed, no re-pair). This file is that example's structure with our
 * application: four Analog Input endpoints (10..13) reporting movement_rms/peak + tilt
 * pitch/roll, Basic (DIY / awning-wind-sensor) + Identify on the primary, deep sleep between
 * wakes. Built against esp-zigbee-lib 2.0.x (the ezb_* / esp_zigbee_* API).
 *
 * API verified against the local 2.0.3 headers (ezbee/af.h, ezbee/zcl/cluster/
 * analog_input_desc.h, ezbee/zcl/zcl_general_cmd.h, ezbee/zcl/zcl_desc.h).
 *
 * FIRST-BUILD SCOPE: join/rejoin + measure + report + deep sleep. Deferred TODOs (battery,
 * wind-event active loop, NVS config, first-join bind settle) are marked below — add once
 * this compiles and the rejoin is confirmed with our endpoints.
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sys/time.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "awning_config.h"
#include "adxl345.h"
#include "battery.h"

static const char *TAG = "AWNING_ZB";

/* Set FORCE_REJOIN to 1 to make the device factory-reset + re-pair on EVERY boot,
 * regardless of the IO25 button. Use it to force a clean join into deCONZ (open Permit
 * Join, flash this, let it join), then set it back to 0 and re-flash for normal operation.
 * Leaving it at 1 would wipe + re-steer on every wake — do NOT ship with it on. */
#define FORCE_REJOIN 0

/* Wall-clock across deep sleep, like the vendor example. */
static RTC_DATA_ATTR struct timeval s_sleep_time;
static esp_timer_handle_t s_sleep_timer;
static esp_timer_handle_t s_report_timer;   /* repeating re-report during the settle window */
static esp_timer_handle_t s_steer_timer;    /* one-shot retry of network steering on failure */

/* Set at boot if the IO25 pairing button was held: leave the network + re-join fresh. */
static bool s_reset_requested = false;

/* Read the IO25 pairing button (active-low, internal pull-up). Debounced: must read
 * pressed twice ~30 ms apart. Called once at boot before Zigbee starts.
 * Logs the raw level both times so a wiring/polarity problem is visible. */
static bool boot_button_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(20)); /* let the internal pull-up settle */
    int l1 = gpio_get_level(PIN_BOOT_BTN);
    vTaskDelay(pdMS_TO_TICKS(30));
    int l2 = gpio_get_level(PIN_BOOT_BTN);
    ESP_LOGI(TAG, "IO25 button level: %d, %d (pressed reads %d)", l1, l2, BOOT_BTN_PRESSED);
    return (l1 == BOOT_BTN_PRESSED) && (l2 == BOOT_BTN_PRESSED);
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

    /* Timer heartbeat + EXT1 wake on the accel INT1 (IO13, active-high). */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_timer_wakeup((uint64_t)HB_INTERVAL_S * 1000000ULL),
                        TAG, "timer wakeup");
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup(EXT1_IO_MASK, ESP_EXT1_WAKEUP_ANY_HIGH),
                        TAG, "ext1 wakeup");
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------- */
/* Reporting: set each Analog Input present value (0x0055), then report it.               */
/* ------------------------------------------------------------------------------------- */
static void report_one(uint8_t ep_id, float value)
{
    /* Update the LIVE attribute value in the ZCL database (by ep/cluster/attr). This is the
     * canonical call — the same one the compat esp_zb_zcl_set_attribute_val wraps. The
     * earlier ezb_zcl_attr_desc_set_value(attr_desc,...) did NOT update the value a Read/
     * report returns, so deCONZ read 0 on every endpoint. `check_access=false` because we
     * are writing our own read-only attribute internally. */
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_status_t st = ezb_zcl_set_attr_value(
        ep_id, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE, &value, false);

    /* DIAGNOSTIC: read the value straight back to confirm the write landed in the DB. */
    float readback = -1.0f;
    ezb_zcl_attr_desc_t a = ezb_zcl_get_attr_desc(
        ep_id, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE);
    if (a != EZB_INVALID_DESC) ezb_zcl_attr_desc_get_value(a, &readback);
    ESP_LOGI(TAG, "ep %u set=%.1f status=0x%02x readback=%.1f (attr %s)",
             ep_id, value, st, readback, a == EZB_INVALID_DESC ? "MISSING" : "ok");

    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr.addr_mode = EZB_ADDR_MODE_NONE, /* -> coordinator (bound) */
            .src_ep = ep_id,
        },
        .payload.attr_id = EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
    };
    ezb_zcl_report_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
}

/* Update + report BatteryPercentageRemaining (0x0021, half-percent) on the Power Config
 * cluster of EP_OCCUPANCY (ep 14). deCONZ's DDF reads battery on ep 14 (same endpoint as the
 * presence subdevice, so the ZHAPresence /sensors resource instantiates cleanly) and does
 * Attr.val/2 to get 0-100 %. */
static void report_battery(uint8_t soc_percent)
{
    uint8_t half_pct = (soc_percent > 100) ? 200 : (uint8_t)(soc_percent * 2);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(EP_OCCUPANCY, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                           EZB_ZCL_STD_MANUF_CODE, &half_pct, false);
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = { .dst_addr.addr_mode = EZB_ADDR_MODE_NONE, .src_ep = EP_OCCUPANCY },
        .payload.attr_id = EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
    };
    ezb_zcl_report_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
}

/* Motion threshold (mg) for the Phoscon Occupancy boolean. Runtime-adjustable via NVS
 * (namespace "awning", key "motion_mg", u16) so it can be changed without a reflash; falls
 * back to MOTION_MG_DEFAULT when the key is unset. Cheap to read each wake — the value could
 * have been changed since the last boot. (Full NVS config port is still a TODO; this reads
 * only the one key we need adjustable.) */
static uint16_t motion_threshold_mg(void)
{
    nvs_handle_t h;
    uint16_t mg = MOTION_MG_DEFAULT;
    if (nvs_open("awning", NVS_READONLY, &h) == ESP_OK) {
        uint16_t v;
        if (nvs_get_u16(h, "motion_mg", &v) == ESP_OK) mg = v;
        nvs_close(h);
    }
    return mg;
}

/* Update + report the Occupancy attribute (0x0000, bitmap) on the Occupancy Sensing cluster
 * of EP_OCCUPANCY. deCONZ maps this to ZHAPresence -> a Phoscon "Motion" card. */
static void report_occupancy(bool occupied)
{
    uint8_t occ = occupied ? EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_OCCUPIED
                           : EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_UNOCCUPIED;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(EP_OCCUPANCY, EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                           EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                           EZB_ZCL_STD_MANUF_CODE, &occ, false);
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = { .dst_addr.addr_mode = EZB_ADDR_MODE_NONE, .src_ep = EP_OCCUPANCY },
        .payload.attr_id = EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
    };
    ezb_zcl_report_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
}

static void report_metrics_once(void)
{
    accel_metrics_t m;
    adxl345_measure_window(&m);
    ESP_LOGI(TAG, "report rms=%.1f peak=%.1f pitch=%.1f roll=%.1f",
             m.movement_rms, m.movement_peak, m.tilt_pitch, m.tilt_roll);

    report_one(EP_MOVEMENT_RMS,  m.movement_rms);
    report_one(EP_MOVEMENT_PEAK, m.movement_peak);
    report_one(EP_TILT_PITCH,    m.tilt_pitch);
    report_one(EP_TILT_ROLL,     m.tilt_roll);

    /* Phoscon motion boolean: occupied while the movement RMS is over the (NVS-tunable)
     * threshold. HA still calibrates the real awning-close logic against movement_rms above. */
    uint16_t thresh = motion_threshold_mg();
    bool occupied = m.movement_rms > (float)thresh;
    ESP_LOGI(TAG, "occupancy=%d (rms=%.1f thresh=%u mg)", occupied, m.movement_rms, thresh);
    report_occupancy(occupied);

    battery_reading_t b = battery_read(VBAT_RATIO);
    report_battery(b.soc);
}

/* Report, then schedule deep sleep after `settle_seconds`. NON-BLOCKING: we just arm the
 * one-shot sleep timer for `settle_seconds` and return, so the Zigbee stack keeps running
 * (and polling its parent) the whole time — we must NOT block the signal-handler/Zigbee
 * task with a long delay.
 *
 * settle_seconds is the KEY to deCONZ integration for a sleepy ED. On a FRESH JOIN, deCONZ
 * sends Active-Endpoint + Simple-Descriptor requests (to learn our 4 analog endpoints) and
 * then Bind + Configure-Reporting — and it can only do that while the device is AWAKE and
 * polling. If we sleep 2 s after joining, deCONZ sees an EMPTY node. So: fresh join -> ~90 s
 * awake; routine wake -> short. A periodic re-report during the window gives deCONZ live
 * traffic; that is driven by a separate repeating timer armed here. */
static void periodic_report_cb(void *arg)
{
    if (ezb_bdb_dev_joined()) report_metrics_once();
}

/* Re-kick commissioning after a failure, on a DELAY (one-shot timer, off the signal-handler
 * stack). CRITICAL: the retry MUST be delayed — re-calling commissioning inline from the
 * failure signal spins a tight infinite loop (observed: 0x03 retrying every ~170 ms). The
 * mode to retry is stashed in s_retry_mode. */
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
    /* Restart the timer; if a retry is already pending this coalesces to one — prevents the
     * retry storm even if many failure signals arrive. 3 s backoff. */
    esp_timer_stop(s_steer_timer);
    esp_timer_start_once(s_steer_timer, 3 * 1000000ULL);
}

static void report_and_sleep(uint32_t settle_seconds)
{
    report_metrics_once();
    /* TODO: battery report + low-voltage lockout (port power.c).
     * TODO: WIND_EVENT active loop when woken by accel INT1 (EXT1). */

    /* Re-report every 5 s for the WHOLE settle window (fresh join OR routine wake). This
     * keeps the device awake and polling so deCONZ can service its queued reads for ALL
     * four endpoints, not just the first one — the reason 0x0B/0C/0D read 0 before was that
     * the device slept before deCONZ polled past endpoint 0x0A. Non-blocking timer. */
    ESP_LOGI(TAG, "settling awake %us (deCONZ reads all 4 endpoints)", settle_seconds);
    if (!s_report_timer) {
        const esp_timer_create_args_t ra = { .callback = periodic_report_cb, .name = "rep" };
        esp_timer_create(&ra, &s_report_timer);
    }
    esp_timer_start_periodic(s_report_timer, 5 * 1000000ULL);

    /* Arm deep sleep after the full settle window (the sleep cb stops the report timer). */
    ESP_ERROR_CHECK(esp_timer_start_once(s_sleep_timer, (uint64_t)settle_seconds * 1000000ULL));
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
            } else if (s_reset_requested || FORCE_REJOIN) {
                /* IO25 button held at boot (or FORCE_REJOIN build flag): leave the network +
                 * wipe stored data so the device becomes factory-new and re-joins fresh. The
                 * LEAVE(RESET) triggers a reboot; on the next boot it is factory-new and
                 * steers into the open network. Open Permit Join in deCONZ first. */
                ESP_LOGW(TAG, "%s — factory reset, will re-join fresh",
                         FORCE_REJOIN ? "FORCE_REJOIN build flag" : "pairing button held");
                ezb_bdb_reset_via_local_action();
            } else {
                /* Post-deep-sleep wake of a provisioned device: ALREADY on the network.
                 * Routine wake -> short settle (deCONZ already knows this device). */
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
            /* FRESH JOIN: stay awake ~90 s so deCONZ can read our simple descriptors
             * (learn the 4 analog endpoints) and write bindings. Without this the node
             * shows EMPTY in deCONZ. */
            report_and_sleep(FRESH_JOIN_SETTLE_S);
        } else {
            /* Steering failed (e.g. 0x03 = no network found / not joined). Retry after a
             * short delay via a timer — deCONZ's Permit Join window may not have been open
             * at the exact instant, or the first attempt raced the coordinator. Keep trying
             * so a fresh join eventually lands (bounded backoff would be nicer; simple retry
             * for now). */
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
/* Device construction: 4 Analog Input endpoints, Basic+Identify on the primary.          */
/* ------------------------------------------------------------------------------------- */
static ezb_af_ep_desc_t make_analog_endpoint(uint8_t ep_id, bool primary)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id = ep_id,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = 0x000C, /* Simple Sensor / analog — matches the DDF device id */
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);

    /* Analog Input (0x000C) server, present value 0. */
    ezb_zcl_analog_input_cluster_server_config_t ai_cfg = {
        .out_of_service = false,
        .present_value = 0.0f,
        .status_flags = 0,
    };
    ezb_zcl_cluster_desc_t ai =
        ezb_zcl_analog_input_create_cluster_desc(&ai_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_af_endpoint_add_cluster_desc(ep, ai);

    if (primary) {
        /* Basic with manufacturer/model for the deCONZ DDF. */
        ezb_zcl_cluster_desc_t basic =
            ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
        ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                            (void *)ZB_MANUFACTURER_ZCL);
        ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                            (void *)ZB_MODEL_ZCL);
        ezb_af_endpoint_add_cluster_desc(ep, basic);

        /* Identify (coordinators expect it for commissioning). */
        ezb_zcl_cluster_desc_t identify =
            ezb_zcl_identify_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
        ezb_af_endpoint_add_cluster_desc(ep, identify);
    }

    /* Battery (Power Configuration 0x0001) now lives on the Occupancy endpoint (14), not here
     * — the DDF reads it there so the ZHAPresence sensor + battery instantiate on one endpoint.
     * See make_occupancy_endpoint(). */
    return ep;
}

/* Occupancy Sensing endpoint (0x0406) -> ZHAPresence in deCONZ -> Phoscon "Motion" card.
 * A single boolean Occupancy attribute (0x0000), no IAS Zone enrollment (unlike a vibration
 * sensor), so nothing extra has to survive deep sleep. Physical-contact sensor type: this is
 * a movement-derived contact, not a PIR. */
static ezb_af_ep_desc_t make_occupancy_endpoint(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id = EP_OCCUPANCY,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = OCCUPANCY_DEVICE_ID, /* 0x0107 occupancy sensor — matches the DDF */
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);

    ezb_zcl_occupancy_sensing_cluster_server_config_t occ_cfg = {
        .occupancy = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_UNOCCUPIED,
        .occupancy_sensor_type = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PHYSICAL_CONTACT,
        .occupancy_sensor_type_bitmap = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_PHYSICAL_CONTACT,
    };
    ezb_zcl_cluster_desc_t occ =
        ezb_zcl_occupancy_sensing_create_cluster_desc(&occ_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_af_endpoint_add_cluster_desc(ep, occ);

    /* deCONZ instantiates the ZHAPresence /sensors resource cleanly only when the presence
     * subdevice's items live on ITS OWN endpoint. So mirror Basic (manufacturer/model, so the
     * fingerprint's 0x0000 matches) and Power Config (battery, read on ep 14 by the DDF) here.
     * The analog primary (ep 10) still carries its own Basic for the analog subdevices. */
    ezb_zcl_cluster_desc_t basic =
        ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)ZB_MANUFACTURER_ZCL);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)ZB_MODEL_ZCL);
    ezb_af_endpoint_add_cluster_desc(ep, basic);

    ezb_zcl_cluster_desc_t pwr =
        ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    uint8_t batt0 = 0;
    ezb_zcl_power_config_cluster_desc_add_attr(
        pwr, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &batt0);
    ezb_af_endpoint_add_cluster_desc(ep, pwr);
    return ep;
}

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    const uint8_t eps[4] = { EP_MOVEMENT_RMS, EP_MOVEMENT_PEAK, EP_TILT_PITCH, EP_TILT_ROLL };
    for (int i = 0; i < 4; i++) {
        ezb_af_ep_desc_t ep = make_analog_endpoint(eps[i], i == 0);
        ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, ep), TAG, "add ep %u", eps[i]);
    }
    /* Fifth endpoint: Occupancy Sensing, for the Phoscon motion card. */
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev, make_occupancy_endpoint()),
                        TAG, "add occupancy ep %u", EP_OCCUPANCY);
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

    /* Check the IO25 pairing button FIRST. If held, request a factory reset so the device
     * leaves any stored network and re-joins fresh (handled once Zigbee is up). */
    s_reset_requested = boot_button_held();
    if (s_reset_requested) {
        ESP_LOGW(TAG, "IO25 pairing button held at boot -> will factory-reset + re-pair");
    }

    /* Accelerometer up on every boot (I2C). */
    ESP_ERROR_CHECK(adxl345_init());
    adxl345_configure(ACT_THRESH_DEFAULT);

    /* Battery ADC (non-fatal: if it fails we just report 0% rather than aborting boot). */
    if (battery_init() != ESP_OK) ESP_LOGW(TAG, "battery ADC init failed");

    ESP_ERROR_CHECK(deep_sleep_wakeup_config());

    ESP_LOGI(TAG, "Start ESP Zigbee stack (channel %d)", ZB_CHANNEL);
    xTaskCreate(zigbee_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
