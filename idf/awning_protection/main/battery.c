/* battery.c — battery voltage + SoC (IDF port of Arduino power.cpp).
 * Uses the ADC oneshot driver + calibration; PIN_VBAT (IO3) is ADC1. */

#include "battery.h"
#include "awning_config.h"

#include <math.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "BATT";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

/* LaskaKit ESP32-H2-DEVKit V2 routes the battery divider (V_BAT-R8-node-R9-GND,
 * node = C8 + ADC) to IO3. On the ESP32-H2, GPIO3 = ADC1 channel 2. */
#define VBAT_ADC_UNIT    ADC_UNIT_1
#define VBAT_ADC_CHANNEL ADC_CHANNEL_2

/* Piecewise SoC curve (spec §7), high -> low. Same as Arduino power.cpp. */
typedef struct { float v; uint8_t soc; } soc_point_t;
static const soc_point_t kSoc[] = {
    {4.20f, 100}, {4.00f, 75}, {3.85f, 50}, {3.70f, 30},
    {3.50f, 15},  {3.35f, 5},  {3.30f, 0},
};

static uint8_t soc_from_voltage(float v)
{
    const int n = sizeof(kSoc) / sizeof(kSoc[0]);
    if (v >= kSoc[0].v) return 100;
    if (v <= kSoc[n - 1].v) return 0;
    for (int i = 0; i + 1 < n; i++) {
        float vhi = kSoc[i].v, vlo = kSoc[i + 1].v;
        if (v <= vhi && v > vlo) {
            float shi = kSoc[i].soc, slo = kSoc[i + 1].soc;
            float frac = (v - vlo) / (vhi - vlo);
            return (uint8_t)lroundf(slo + frac * (shi - slo));
        }
    }
    return 0;
}

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = VBAT_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,     /* full ~0..3.3V range at the pin */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) return err;

    /* Curve-fitting calibration (H2). If it fails we fall back to raw*ref math. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = VBAT_ADC_UNIT,
        .chan = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK);
    ESP_LOGI(TAG, "ADC init, calibration %s", s_cali_ok ? "ok" : "unavailable");
    return ESP_OK;
}

battery_reading_t battery_read(float vbat_ratio)
{
    const int kSamples = 16;
    uint32_t acc_mv = 0;
    for (int i = 0; i < kSamples; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw);
        if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            acc_mv += mv;
        } else {
            /* Fallback: 12-bit full scale ~3300 mV at 12 dB. */
            acc_mv += (uint32_t)(raw * 3300 / 4095);
        }
    }
    float pin_v = (acc_mv / (float)kSamples) / 1000.0f;
    float cell_v = pin_v * vbat_ratio;

    battery_reading_t r = { .volts = cell_v, .soc = soc_from_voltage(cell_v) };
    ESP_LOGI(TAG, "pin=%.3fV cell=%.3fV soc=%u%%", pin_v, cell_v, r.soc);
    return r;
}
