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

/* --- Single endpoint -------------------------------------------------------------------- */
/* One endpoint carries everything: Basic + Identify + Power Config (battery) + Occupancy
 * Sensing (0x0406). deCONZ renders the Occupancy cluster as a ZHAPresence sensor -> a Phoscon
 * "Motion"/presence card, and reads battery from the same endpoint's Power Config. One
 * endpoint = one clean DDF fingerprint, so pairing instantiates reliably. */
#define EP_AWNING           10
#define OCCUPANCY_DEVICE_ID 0x0107 /* ZHA "Occupancy Sensor" device id (profile 0x0104) */

/* --- Pin map (LaskaKit ESP32-H2-DEVKit V2) --------------------------------------------- */
#define PIN_VBAT  3             /* ADC battery divider (LaskaKit V2 IO3 = ADC1_CH2) */

/* --- Heartbeat / sleep ----------------------------------------------------------------- */
#define HB_INTERVAL_S       1800   /* 30 min heartbeat wake */

/* --- Battery --------------------------------------------------------------------------- */
#define VBAT_RATIO          1.769f /* LaskaKit ESP32-H2-DEVKit V2 silkscreen divider ratio (IO3) */

/* --- Occupancy (the one reported value) ------------------------------------------------ */
/* Placeholder occupancy source until real sensing is wired in: this build reports a fixed
 * boolean so the whole Zigbee/sleep/battery pipeline can be validated end to end. Replace
 * awning_is_protected() in awning_protection.c with the real measurement. */
#define AWNING_OCCUPANCY_DEFAULT  false
