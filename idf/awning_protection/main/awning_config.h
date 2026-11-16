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

/* --- Zigbee channels ------------------------------------------------------------------ */
/* Pairing scans ALL Zigbee channels (11-26), so the sensor joins any coordinator without a
 * reflash. After the first join the stack stores the network's channel and every rejoin goes
 * straight to it, so the wider mask only costs time during pairing. */
#define ZB_CHANNEL_MASK  0x07FFF800UL   /* channels 11..26 */

/* --- Link loss -------------------------------------------------------------------------- */
/* ed_timeout in ESP_ZIGBEE_ZED_CONFIG() below (4 min) is, on an end device, how long keep-alive polls may keep failing
 * before the stack declares its parent lost (it was 64 min: the sensor sat deaf for an hour).
 * When the parent is lost the firmware reboots after LINK_LOSS_REBOOT_DELAY_S; the boot path
 * then rejoins (with backoff), finding a new parent if the old router is gone. */
#define LINK_LOSS_REBOOT_DELAY_S   2

/* --- Pairing window -------------------------------------------------------------------- */
/* An unpaired sensor searches for an open network (deCONZ "Permit join") for this long, then
 * stops to save the battery. Reopen it with a short press of the user button or with RST. */
#define PAIR_WINDOW_S        180   /* how long the sensor searches for a network */
#define PAIR_RETRY_S           3   /* pause between steering attempts inside the window */

/* Interview window after a FRESH join. deCONZ then reads the node/simple descriptors and basic
 * attributes, binds the clusters and configures reporting: a few dozen requests. Each one waits
 * at the parent until our next poll, so at the normal ZED_KEEP_ALIVE_MS (4 s) the interview is
 * slow enough to time out half done (sensor found, but not set up correctly). For this window the
 * sensor polls every INTERVIEW_POLL_MS and stays out of light sleep; the LED keeps blinking blue
 * and turns green when the window ends. Not used on a rejoin of an already paired sensor. */
#define INTERVIEW_S          180
#define INTERVIEW_POLL_MS    250

/* --- Zigbee stack config (modeled on the vendor deep_sleep_end_device .h) --------------- */
#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define ZED_KEEP_ALIVE_MS   4000   /* normal poll interval (ms) to the parent */

#define ESP_ZIGBEE_ZED_CONFIG()                        \
    {                                                  \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE, \
        .install_code_policy = false,                  \
        .zed_config = {                                \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_4MIN,     \
            .keep_alive = ZED_KEEP_ALIVE_MS,           \
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
 * and wakes the radio automatically around its poll timer; the app only gates sleep for the
 * LED/button and for the INTERVIEW_S window after a fresh join. See the
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
#define PIN_LED   GPIO_NUM_22   /* SK6812 RGB status LED (LaskaKit V2: moved from IO9 to IO22) */
#define PIN_BUTTON GPIO_NUM_25  /* on-board user button (BOM: "user button, IO25") */
/* IO12 (IO12_USUP) enables the RT9080 LDO IC1 that makes VSENSOR (10k pull-down on EN). On the
 * V2 schematic VSENSOR feeds the SK6812 VDD, the uSup J3/J5 VCC pins and the I2C pull-ups
 * R13/R14. ui.c switches it on only while the LED shows something: an idle SK6812 draws
 * about 1 mA even when dark, which would cost far more than the rest of the sleep budget. */
#define PIN_VSENSOR_EN GPIO_NUM_12

/* --- User button + status LED (ui.c) ---------------------------------------------------- */
/* IO25 is not an LP/RTC GPIO, so it cannot wake light sleep; it is sampled on every report
 * tick (MOTION_POLL_S). Hold ~1 s for a short press; hold UI_LONG_PRESS_S, then release, for a
 * factory reset. Holds longer than UI_STUCK_PRESS_S are ignored (stuck plunger protection). */
#define BUTTON_ACTIVE_LEVEL    0     /* button pulls IO25 to GND (internal pull-up enabled).
                                        VERIFY on the board schematic; set 1 if it pulls high. */
#define BUTTON_DEBOUNCE_MS    30
#define BUTTON_SAMPLE_MS      20
#define UI_LONG_PRESS_S        5
#define UI_STUCK_PRESS_S      20
#define LED_BRIGHTNESS        24     /* 0-255 per colour channel; low to save battery */
#define LED_JOINED_MS       2000
#define LED_FAILED_MS       2000
#define LED_ACK_MS           300

/* Factory reset fallback: after a button reset the device first tries a proper Zigbee leave
 * (tells the coordinator it is gone). If the coordinator is unreachable and no leave
 * confirmation arrives within this time, it wipes the network data locally and reboots. */
#define FACTORY_RESET_FALLBACK_S  10

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

/* --- Measurement window (drives adxl345_measure_window, heartbeat diagnostic only) ------- */
#define SAMPLE_RATE_HZ   50
#define WINDOW_MS        4000
#define WINDOW_SAMPLES   ((SAMPLE_RATE_HZ * WINDOW_MS) / 1000) /* 200 */

/* --- Motion classifier: open/close vs wind (see MOTION_CLASSIFIER_DESIGN.md) ------------- */
/* On a motion trigger we sample a SHORT window and classify the MOTION CHARACTER: a motor
 * open/close is smooth / one-directional / low-frequency; wind is chaotic / oscillating /
 * high-frequency. Smooth => keep occupancy 0 (deliberate move); chaotic => latch 1 (wind).
 * NOTE: the *_THRESH values below are PLACEHOLDERS — they must be tuned from real data (the
 * firmware logs every metric each window; see the design doc §6). Until tuned, leave
 * CLASSIFIER_ENABLE at 0 to keep the proven always-latch behaviour while you collect logs. */
#define CLASSIFIER_ENABLE       0   /* 0 = always latch on motion (current behaviour + metric logs);
                                       1 = use the smooth-vs-chaotic decision below */

#define CLASS_WINDOW_MS       1500  /* classification window length (short, not the 4s diag one) */
#define CLASS_WINDOW_SAMPLES  ((SAMPLE_RATE_HZ * CLASS_WINDOW_MS) / 1000) /* 75 */

/* Duration bound: a motor open/close is ~36 s here, then stops dead. A "smooth" burst is
 * treated as a deliberate move for at most this long; if motion is STILL present after it, a
 * real move would have finished, so we reclassify as wind and latch 1 (a sustained disturbance
 * is wind whatever its texture). 36 s travel + margin. */
#define MOVE_MAX_S             45

/* Decision thresholds (PLACEHOLDERS — tune per MOTION_CLASSIFIER_DESIGN.md §6). "Smooth" =
 * few direction reversals AND low jerk AND (strongly one-directional OR one dominant axis). */
#define CLASS_REVERSALS_MAX     6     /* max dominant-axis AC sign-changes to still be "smooth" */
#define CLASS_JERK_MAX          40.0f /* max mean |Δaccel| per sample (mg) to still be "smooth" */
#define CLASS_DIRECTIONALITY_MIN 0.55f /* min |net|/path on dominant axis (1=pure one-way, 0=oscillation) */
#define CLASS_DOM_RATIO_MIN      0.60f /* min dominant-axis AC-energy fraction (1=single axis) */

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
