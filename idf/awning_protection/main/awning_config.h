/* awning_config.h — tunables + pin map for awning-protection.
 * Minimal single-endpoint Zigbee sleepy ED: join, sleep, battery, one occupancy value.
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

/* --- Zigbee identity (MUST match the deCONZ DDF) --------------------------------------- */
/* ZCL length-prefixed strings: first byte = length. "DIY" = 3, "awning-protection" = 17. */
#define ZB_MANUFACTURER_ZCL "\x03""DIY"
#define ZB_MODEL_ZCL        "\x11""awning-protection"

/* Plain-text copies for the DDF / logs. */
#define ZB_MANUFACTURER_STR "DIY"
#define ZB_MODEL_STR        "awning-protection"

/* --- Zigbee network channel (deCONZ) --------------------------------------------------- */
/* Pin the ED to the coordinator's single channel so a post-sleep rejoin scans one channel
 * (fast, reliably beats the join timeout). MUST match your deCONZ network channel
 * (Phoscon -> Settings -> Gateway -> Advanced). */
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

#define ESP_ZIGBEE_PLATFORM_CONFIG()                                    \
    {                                                                   \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME,    \
        .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE }, \
    }

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config = ESP_ZIGBEE_ZED_CONFIG(),        \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    }

/* --- Settle windows (sleepy-ED timing) ------------------------------------------------- */
/* After a FRESH JOIN, stay awake this long so deCONZ can read our simple descriptor and push
 * Bind + Configure-Reporting (a sleepy ED only gets those while awake). On a routine wake we
 * sleep after SHORT_SETTLE_S. */
#define FRESH_JOIN_SETTLE_S  180   /* deCONZ descriptor read + bind + report-config */
#define SHORT_SETTLE_S        20   /* routine wake: single endpoint, so short is fine */

/* Motion-hold: if the settle window ends while movement is STILL active, the device would
 * sleep reporting occupancy=1 and — because INT1 only fires on new motion, not on motion
 * stopping — nothing would wake it to report the eventual stop until the 30-min heartbeat.
 * So while occupancy reads 1, we keep re-extending the awake window by SHORT_SETTLE_S each
 * cycle, up to MOTION_HOLD_MAX_S total, so we can observe and report the clean occupancy=0
 * before sleeping. The cap bounds battery cost of a long continuous disturbance. */
#define MOTION_HOLD_MAX_S    300   /* max awake while motion persists (battery guard) */

/* Early-sleep on quiet wakes: if a routine wake reads UNOCCUPIED (movement below threshold),
 * there's no motion-stop to wait for. We still give deCONZ one short grace window to flush any
 * queued read/write (e.g. a config/sensitivity change) before sleeping, then cut the settle
 * short instead of burning the full SHORT_SETTLE_S with the radio on. Saves battery on the
 * common heartbeat-wake-with-no-motion case. Not applied to a FRESH_JOIN settle (bind/report
 * config needs the long window regardless of occupancy). */
#define QUIET_GRACE_S          5   /* deCONZ flush window before early sleep when unoccupied */

/* --- Single endpoint -------------------------------------------------------------------- */
/* One endpoint carries everything: Basic + Identify + Power Config (battery) + Occupancy
 * Sensing (0x0406). deCONZ renders the Occupancy cluster as a ZHAPresence sensor -> a Phoscon
 * "Motion"/presence card, and reads battery from the same endpoint's Power Config. One
 * endpoint = one clean DDF fingerprint, so pairing instantiates reliably. */
#define EP_AWNING           10
#define OCCUPANCY_DEVICE_ID 0x0107 /* ZHA "Occupancy Sensor" device id (profile 0x0104) */

/* --- Pin map (LaskaKit ESP32-H2-DEVKit V2) --------------------------------------------- */
#define PIN_SDA   GPIO_NUM_11   /* ADXL345 SDA */
#define PIN_SCL   GPIO_NUM_10   /* ADXL345 SCL */
#define PIN_INT1  GPIO_NUM_13   /* ADXL345 INT1, active-high, deep-sleep wake */
#define PIN_VBAT  3             /* ADC battery divider (LaskaKit V2 IO3 = ADC1_CH2) */

/* EXT1 deep-sleep wake mask: accel INT1 (active-high, ANY_HIGH). Movement asserts INT1 and
 * wakes the device so it can re-measure occupancy and report — this is what makes the sensor
 * "react" instead of only re-checking on the heartbeat timer. */
#define EXT1_IO_MASK (1ULL << PIN_INT1)

/* --- Heartbeat / sleep ----------------------------------------------------------------- */
#define HB_INTERVAL_S       1800   /* 30 min heartbeat wake (also re-evaluates occupancy) */

/* --- Battery --------------------------------------------------------------------------- */
#define VBAT_RATIO          1.769f /* LaskaKit ESP32-H2-DEVKit V2 silkscreen divider ratio (IO3) */

/* --- Measurement window (drives adxl345_measure_window) --------------------------------- */
#define SAMPLE_RATE_HZ   50
#define WINDOW_MS        4000
#define WINDOW_SAMPLES   ((SAMPLE_RATE_HZ * WINDOW_MS) / 1000) /* 200 */

/* ADXL345 THRESH_ACT raw value (x62.5 mg). Wake sensitivity: raw 2 ~= 125 mg. */
#define ACT_THRESH_DEFAULT  2

/* --- Occupancy (the one reported value) ------------------------------------------------ */
/* movement_rms (mg) at/above which occupancy reads "protected" (occupied). Derived from the
 * accelerometer window; ONLY this boolean is reported — no analog values leave the device.
 * Above the ~5-8 mg bench-noise floor. */
#define OCCUPANCY_MG_THRESHOLD  60   /* power-on default; runtime value is writable (see below) */

/* --- Runtime-tunable threshold --------------------------------------------------------- */
/* The occupancy threshold is exposed as the standard Occupancy Sensing attribute
 * PIRUnoccupiedToOccupiedThreshold (0x0012, uint8). Its value IS the movement_rms threshold
 * in mg — write 1..254 from deCONZ (ZCL write attr, or a DDF-mapped config field) to retune
 * live, no reflash. The written value is persisted in NVS so it survives sleep + power-cycle.
 * On first boot (no NVS entry yet) it seeds from OCCUPANCY_MG_THRESHOLD above. */
#define THRESH_NVS_NAMESPACE  "awning"
#define THRESH_NVS_KEY        "occ_thresh"
#define THRESH_MG_MIN         1u
#define THRESH_MG_MAX         254u   /* uint8 attr ceiling; plenty for a shake threshold */
