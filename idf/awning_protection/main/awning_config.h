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

/* --- Sleep model: Zigbee LIGHT sleep (not deep sleep) ----------------------------------- */
/* This firmware uses IDF power management light sleep (tickless idle), NOT deep sleep. In
 * light sleep the ZED long-poll (keep_alive below) keeps running, so the coordinator never
 * ages the device out and the parent link never drops — no rejoin race, reports land
 * instantly. There is therefore NO settle window / motion-hold / early-sleep state machine
 * anymore (all of which only existed to survive the deep-sleep rejoin gap). The stack sleeps
 * and wakes the radio automatically around its poll timer; the app never gates sleep. See the
 * vendor example esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device.
 *
 * NOTE (2.x ezbee SDK): there is no esp_zb_sleep_now()/CAN_SLEEP signal (1.x only; the compat
 * macros are empty stubs). Light sleep is enabled purely by esp_pm_configure() in app_main
 * plus the CONFIG_PM_* / tickless-idle flags in sdkconfig.defaults. */

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
#define PIN_INT1  GPIO_NUM_13   /* ADXL345 INT1, active-high; EXT1 light-sleep wake (RTC IO) */
#define PIN_VBAT  3             /* ADC battery divider (LaskaKit V2 IO3 = ADC1_CH2) */

/* Light-sleep wake on the accel INT1 (active-high). Movement asserts INT1, which brings the
 * CPU out of light sleep via EXT1 (the always-on RTC domain — required because we power down
 * the GPIO peripheral in sleep, which disables the plain GPIO-wake path on the H2). A normal
 * GPIO edge ISR then notifies the report task, which re-measures + reports immediately — this
 * is what makes the sensor "react" between heartbeats. GPIO13 is RTC-capable, so it is a legal
 * EXT1 pin. */

/* --- Heartbeat --------------------------------------------------------------------------- */
/* Periodic occupancy + battery refresh. Occupancy still reacts instantly to motion via INT1;
 * this is just the keep-fresh cadence. 30 min is fine on battery now that the radio idles in
 * light sleep rather than paying a rejoin every wake. */
#define HB_INTERVAL_S       1800   /* 30 min heartbeat report (occupancy + battery) */

/* --- Motion-latch occupancy ------------------------------------------------------------ */
/* Occupancy is driven by the ADXL345 ACTIVITY interrupt, not a re-measured RMS window: when
 * activity fires (movement crossed THRESH_ACT), occupancy latches OCCUPIED immediately. It
 * clears back to UNOCCUPIED after OCCUPANCY_CLEAR_S with no further activity. The report task
 * polls the ADXL activity flag every MOTION_POLL_S — short enough to notice motion promptly
 * after an EXT1 light-sleep wake (the GPIO edge ISR can be missed across the sleep isolation),
 * long enough that the CPU still light-sleeps between polls. */
#define MOTION_POLL_S         1    /* how often the report task checks the ADXL activity flag */
#define OCCUPANCY_CLEAR_S    30    /* quiet time before occupancy latches back to unoccupied */

/* --- Battery --------------------------------------------------------------------------- */
#define VBAT_RATIO          1.769f /* LaskaKit ESP32-H2-DEVKit V2 silkscreen divider ratio (IO3) */

/* --- Measurement window (drives adxl345_measure_window) --------------------------------- */
#define SAMPLE_RATE_HZ   50
#define WINDOW_MS        4000
#define WINDOW_SAMPLES   ((SAMPLE_RATE_HZ * WINDOW_MS) / 1000) /* 200 */

/* --- Sensitivity: ADXL345 ACTIVITY threshold (the ONE knob that gates occupancy) ------- */
/* THRESH_ACT (0x24) raw units, 62.5 mg/LSB. When acceleration activity crosses this, the ADXL
 * fires INT1 and occupancy latches OCCUPIED. This is the real sensitivity control:
 *   raw  1 ≈   62 mg  (very twitchy)
 *   raw  2 ≈  125 mg  (default — a light nudge)
 *   raw  4 ≈  250 mg
 *   raw  8 ≈  500 mg  (needs a firm shake)
 *   raw 16 ≈ 1000 mg  (a real gust / knock)
 * Runtime-writable over Zigbee (see below); this is only the power-on default before any
 * write / NVS value exists. */
#define ACT_THRESH_DEFAULT  2

/* --- Runtime-tunable sensitivity ------------------------------------------------------- */
/* The ADXL activity threshold is exposed as the Occupancy Sensing attribute
 * PIRUnoccupiedToOccupiedThreshold (0x0012, uint8). The attribute value IS the raw THRESH_ACT
 * setting (62.5 mg/LSB) — write 1..255 from deCONZ (ZCL write attr, or a DDF-mapped config
 * field) to retune sensitivity live, no reflash. It is applied to the ADXL immediately and
 * persisted in NVS so it survives sleep + power-cycle. On first boot (no NVS entry yet) it
 * seeds from ACT_THRESH_DEFAULT. Multiply by 62.5 for the approx mg trigger level. */
#define THRESH_NVS_NAMESPACE  "awning"
#define THRESH_NVS_KEY        "act_thresh"   /* raw THRESH_ACT units (was mg RMS pre-rework) */
#define THRESH_RAW_MIN        1u             /* 0 disables activity detection entirely */
#define THRESH_RAW_MAX        255u           /* uint8 register ceiling (~15.9 g) */
