/* battery.h — battery voltage + SoC for the ESP-IDF port (ported from Arduino power.cpp).
 * Reads the on-board divider on PIN_VBAT (IO3) via the IDF ADC oneshot driver, applies the
 * divider ratio, and maps to state-of-charge with the same piecewise curve as the Arduino
 * build. */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float   volts;  /* cell voltage */
    uint8_t soc;    /* 0-100 %, piecewise curve */
} battery_reading_t;

/* Cell voltage at/below which the device should stop and permanently sleep (spec §7). */
#define VBAT_LOCKOUT_V 3.25f

/* One-time ADC init (call once at boot). */
esp_err_t battery_init(void);

/* Read + average the divider, return cell volts + SoC. */
battery_reading_t battery_read(float vbat_ratio);
