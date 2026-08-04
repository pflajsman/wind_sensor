/* awning_config.h — tunables + pin map for the ESP-IDF port.
 * Mirrors the Arduino config.h. Keep values in sync until this port is the source of truth.
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

/* --- Zigbee identity (must match the deCONZ DDF) --------------------------------------- */
/* ZCL length-prefixed strings: first byte = length. "DIY" = 3, "awning-wind-sensor" = 18. */
#define ZB_MANUFACTURER_ZCL "\x03""DIY"
#define ZB_MODEL_ZCL        "\x12""awning-wind-sensor"

/* --- Zigbee network channel (deCONZ) --------------------------------------------------- */
#define ZB_CHANNEL       15
#define ZB_CHANNEL_MASK  (1UL << ZB_CHANNEL)

/* --- Zigbee stack config (modeled on the vendor deep_sleep_end_device .h) --------------- */
#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define ESP_ZIGBEE_ZED_CONFIG()                        \
    {                                                  \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE, \
        .install_code_policy = false,                  \
        .zed_config = {                                \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,    \
            .keep_alive = 4000,                        \
        },                                             \
    }

#define ESP_ZIGBEE_PLATFORM_CONFIG()                                 \
    {                                                                \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE }, \
    }

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config = ESP_ZIGBEE_ZED_CONFIG(),        \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    }

/* --- Settle windows (sleepy-ED timing) ------------------------------------------------- */
/* After a FRESH JOIN, stay awake this long so deCONZ can read our simple descriptors and
 * push Bind + Configure-Reporting (a sleepy ED only gets those while awake). Matches the
 * Arduino ZB_SETTLE_MS = 90 s. On a routine wake we sleep after SHORT_SETTLE_S. */
#define FRESH_JOIN_SETTLE_S  240   /* generous: deCONZ descriptor read + bind + report-config */
#define SHORT_SETTLE_S       120   /* routine wake: enough for deCONZ to poll ALL 4 endpoints */

/* --- Endpoint numbers (same as Arduino zb.h) ------------------------------------------- */
#define EP_MOVEMENT_RMS   10
#define EP_MOVEMENT_PEAK  11
#define EP_TILT_PITCH     12
#define EP_TILT_ROLL      13

/* Occupancy Sensing endpoint (Phoscon-visible motion card). deCONZ renders the Occupancy
 * Sensing cluster (0x0406) as a ZHAPresence sensor -> a real Phoscon "Motion" card, which
 * the four Analog Input endpoints above never get (Phoscon has no card for raw analog).
 * The firmware sets Occupancy = (movement_rms > motion_mg) each wake; the raw mg numbers
 * still ride on eps 10-13 for Home Assistant to calibrate against. */
#define EP_OCCUPANCY      14
#define OCCUPANCY_DEVICE_ID 0x0107 /* ZHA "Occupancy Sensor" device id (profile 0x0104) */

/* --- Pin map (LaskaKit ESP32-H2-DEVKit V2) --------------------------------------------- */
#define PIN_SDA   GPIO_NUM_11
#define PIN_SCL   GPIO_NUM_10
#define PIN_INT1  GPIO_NUM_13   /* ADXL345 INT1, active-high, deep-sleep wake */
#define PIN_VBAT  3             /* ADC battery divider (LaskaKit V2 IO3 = ADC1_CH2) */

/* Pairing / factory-reset button. The board silk-screens "IO25" next to this button.
 * Wired to GND, so it reads LOW when pressed (use the internal pull-up; idle = HIGH).
 * Hold it while the device boots to leave the network and re-join fresh (see app_main). */
#define PIN_BOOT_BTN        GPIO_NUM_25
#define BOOT_BTN_PRESSED    0    /* active-low */

/* EXT1 deep-sleep wake mask: accel INT1 only (button removed). */
#define EXT1_IO_MASK (1ULL << PIN_INT1)

/* --- Measurement window (spec §6) ------------------------------------------------------ */
#define SAMPLE_RATE_HZ   50
#define WINDOW_MS        4000
#define WINDOW_SAMPLES   ((SAMPLE_RATE_HZ * WINDOW_MS) / 1000) /* 200 */

/* --- Tunables (spec §9 defaults; NVS-backed config port is a TODO) --------------------- */
#define ACT_THRESH_DEFAULT  2      /* ~125 mg wake threshold */
#define HB_INTERVAL_S       1800   /* 30 min heartbeat */
#define ACT_REPORT_S        30
#define QUIET_MG            60
#define QUIET_WIN           3
#define MAX_ACTIVE_MIN      30
#define VBAT_RATIO          1.769f /* LaskaKit ESP32-H2-DEVKit V2 silkscreen: divider ratio 1.769 (IO3) */

/* Movement threshold (mg) above which the Phoscon Occupancy boolean reads "occupied".
 * This ONLY drives the Phoscon motion card — the real awning-close logic lives in Home
 * Assistant, calibrated against the raw movement_rms number (eps 10-13). Runtime-adjustable
 * without a reflash: stored in NVS namespace "awning" key "motion_mg" (see motion_threshold_mg()
 * in awning_wind_sensor.c). Default is well above the ~5-8 mg bench-noise floor.
 * UPGRADE PATH: expose this as a writable Zigbee attribute so it can be set from deCONZ. */
#define MOTION_MG_DEFAULT   60
