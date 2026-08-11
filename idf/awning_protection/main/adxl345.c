/* adxl345.c — ADXL345 driver for ESP-IDF. Ported 1:1 from the Arduino adxl345.cpp.
 * Register map, values, scaling and the window math are IDENTICAL; only the bus layer
 * (IDF i2c_master) and timing (esp_timer / esp_rom_delay_us) differ.
 */

#include "adxl345.h"
#include "awning_config.h"

#include <math.h>
#include <string.h>
#include "esp_check.h"   /* ESP_RETURN_ON_ERROR */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/i2c_master.h"

static const char *TAG = "ADXL";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;

/* --- low-level bus helpers ------------------------------------------------------------- */
static void write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    i2c_master_transmit(s_dev, buf, 2, 100);
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t val = 0xFF;
    i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 100);
    return val;
}

static bool read_sample(int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t reg = ADXL_DATAX0, b[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 6, 100) != ESP_OK) return false;
    *x = (int16_t)((b[1] << 8) | b[0]);
    *y = (int16_t)((b[3] << 8) | b[2]);
    *z = (int16_t)((b[5] << 8) | b[4]);
    return true;
}

/* --- public API ------------------------------------------------------------------------ */
esp_err_t adxl345_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    /* Probe both possible addresses. */
    const uint8_t cands[2] = { ADXL345_ADDR_LOW, ADXL345_ADDR_HIGH };
    s_addr = 0;
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, cands[i], 100) == ESP_OK) { s_addr = cands[i]; break; }
    }
    if (!s_addr) { ESP_LOGE(TAG, "no ADXL345 at 0x53/0x1D"); return ESP_ERR_NOT_FOUND; }
    ESP_LOGI(TAG, "responded at 0x%02X", s_addr);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "i2c dev");

    uint8_t devid = read_reg(ADXL_DEVID);
    ESP_LOGI(TAG, "DEVID=0x%02X (expect 0x%02X)", devid, ADXL_DEVID_EXPECTED);
    if (devid != ADXL_DEVID_EXPECTED) {
        ESP_LOGE(TAG, "DEVID mismatch — wrong part / CS not tied high (§8.8)");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

void adxl345_configure(uint8_t act_thresh_raw)
{
    write_reg(ADXL_DATA_FORMAT,   VAL_DATA_FORMAT);
    write_reg(ADXL_BW_RATE,       VAL_BW_RATE_25HZ);
    write_reg(ADXL_THRESH_ACT,    act_thresh_raw);
    write_reg(ADXL_ACT_INACT_CTL, VAL_ACT_INACT_CTL);
    write_reg(ADXL_INT_MAP,       VAL_INT_MAP);
    write_reg(ADXL_INT_ENABLE,    VAL_INT_ENABLE);
    write_reg(ADXL_POWER_CTL,     VAL_POWER_CTL);
    adxl345_clear_interrupt();
    ESP_LOGI(TAG, "configured, THRESH_ACT raw=%u (~%.0f mg)",
             act_thresh_raw, act_thresh_raw * ADXL_THRESH_ACT_MG_PER_LSB);
}

void adxl345_set_odr50(void) { write_reg(ADXL_BW_RATE, VAL_BW_RATE_50HZ); }
void adxl345_set_odr25(void) { write_reg(ADXL_BW_RATE, VAL_BW_RATE_25HZ); }

/* Set the ACTIVITY threshold live (THRESH_ACT, 62.5 mg/LSB). raw 0 disables detection, so the
 * caller should clamp to >=1. Re-reads INT_SOURCE to re-arm cleanly at the new threshold. */
void adxl345_set_act_thresh(uint8_t raw)
{
    write_reg(ADXL_THRESH_ACT, raw);
    adxl345_clear_interrupt();
}

uint8_t adxl345_clear_interrupt(void) { return read_reg(ADXL_INT_SOURCE); }

/* True if the ACTIVITY interrupt has fired since the last INT_SOURCE read. Reading INT_SOURCE
 * also deasserts INT1 (and thus re-arms it for the next activity edge), so this doubles as the
 * "consume + re-arm" step. VAL_INT_ENABLE (0x10) is the ACTIVITY bit. */
bool adxl345_activity_fired(void)
{
    return (read_reg(ADXL_INT_SOURCE) & 0x10) != 0;
}

void adxl345_rearm_activity(void)
{
    write_reg(ADXL_INT_ENABLE, 0x00);
    write_reg(ADXL_INT_ENABLE, VAL_INT_ENABLE);
    adxl345_clear_interrupt();
}

/* Classification window: capture X/Y/Z over CLASS_WINDOW_MS, then compute AC-coupled
 * motion-character metrics on the dominant axis. Distinguishes smooth motor travel from
 * chaotic wind. See MOTION_CLASSIFIER_DESIGN.md §3. */
void adxl345_classify_window(class_metrics_t *out)
{
    const uint16_t N = CLASS_WINDOW_SAMPLES;
    const uint32_t period_us = 1000000UL / SAMPLE_RATE_HZ;

    static float bx[CLASS_WINDOW_SAMPLES], by[CLASS_WINDOW_SAMPLES], bz[CLASS_WINDOW_SAMPLES];
    double sx = 0, sy = 0, sz = 0;
    uint16_t n = 0;

    /* Pass 1: sample the window, buffer per-axis (mg) and accumulate means. */
    for (uint16_t i = 0; i < N; i++) {
        int64_t t0 = esp_timer_get_time();
        int16_t xr, yr, zr;
        if (read_sample(&xr, &yr, &zr)) {
            bx[n] = xr * ADXL_MG_PER_LSB;
            by[n] = yr * ADXL_MG_PER_LSB;
            bz[n] = zr * ADXL_MG_PER_LSB;
            sx += bx[n]; sy += by[n]; sz += bz[n];
            n++;
        }
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - t0);
        if (elapsed < period_us) esp_rom_delay_us(period_us - elapsed);
    }

    memset(out, 0, sizeof(*out));
    out->samples = n;
    if (n < 2) return;

    /* Per-axis mean (the DC / gravity component to remove). */
    float mx = (float)(sx / n), my = (float)(sy / n), mz = (float)(sz / n);

    /* AC energy per axis → pick the dominant axis (most motion). */
    double ex = 0, ey = 0, ez = 0;
    for (uint16_t i = 0; i < n; i++) {
        float ax = bx[i] - mx, ay = by[i] - my, az = bz[i] - mz;
        ex += (double)ax * ax; ey += (double)ay * ay; ez += (double)az * az;
    }
    double etot = ex + ey + ez;

    uint8_t dom = 0; double edom = ex; float *buf = bx; float mean = mx;
    if (ey > edom) { dom = 1; edom = ey; buf = by; mean = my; }
    if (ez > edom) { dom = 2; edom = ez; buf = bz; mean = mz; }
    out->dom_axis  = dom;
    out->dom_ratio = (etot > 0.0) ? (float)(edom / etot) : 0.0f;

    /* Overall AC RMS (all axes) — motion energy. */
    out->rms = sqrtf((float)(etot / n));

    /* Dominant-axis metrics: reversals, jerk, directionality — on the AC signal. */
    uint16_t reversals = 0;
    double jerk_sum = 0, path = 0, net = 0;
    float prev_ac = buf[0] - mean;
    int prev_sign = (prev_ac > 0) - (prev_ac < 0);
    for (uint16_t i = 1; i < n; i++) {
        float ac = buf[i] - mean;
        float d = ac - prev_ac;         /* sample-to-sample change (jerk contribution) */
        jerk_sum += fabsf(d);
        path     += fabsf(d);           /* total path travelled by the AC signal */
        net      += d;                  /* net displacement of the AC signal */
        int sign = (ac > 0) - (ac < 0);
        if (sign != 0 && prev_sign != 0 && sign != prev_sign) reversals++;
        if (sign != 0) prev_sign = sign;
        prev_ac = ac;
    }
    out->reversals      = reversals;
    out->jerk           = (float)(jerk_sum / (n - 1));
    out->directionality = (path > 0.0) ? (float)(fabs(net) / path) : 0.0f;
}

void adxl345_measure_window(accel_metrics_t *out)
{
    const uint16_t N = WINDOW_SAMPLES;
    const uint32_t period_us = 1000000UL / SAMPLE_RATE_HZ;

    static float mag[WINDOW_SAMPLES]; /* static: 200 floats off the stack */
    float sx = 0, sy = 0, sz = 0;
    double sum_mag = 0;
    uint16_t n = 0;

    for (uint16_t i = 0; i < N; i++) {
        int64_t t0 = esp_timer_get_time();
        int16_t xr, yr, zr;
        if (read_sample(&xr, &yr, &zr)) {
            float x = xr * ADXL_MG_PER_LSB;
            float y = yr * ADXL_MG_PER_LSB;
            float z = zr * ADXL_MG_PER_LSB;
            float m = sqrtf(x * x + y * y + z * z);
            mag[n] = m;
            sum_mag += m;
            sx += x; sy += y; sz += z;
            n++;
        }
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - t0);
        if (elapsed < period_us) esp_rom_delay_us(period_us - elapsed);
    }

    memset(out, 0, sizeof(*out));
    if (n == 0) return;

    float baseline = (float)(sum_mag / n);
    double sq = 0;
    float peak = 0;
    for (uint16_t i = 0; i < n; i++) {
        float d = mag[i] - baseline;
        sq += (double)d * d;
        float ad = fabsf(d);
        if (ad > peak) peak = ad;
    }
    out->movement_rms  = sqrtf((float)(sq / n));
    out->movement_peak = peak;

    float xb = sx / n, yb = sy / n, zb = sz / n;
    out->tilt_pitch = atan2f(xb, sqrtf(yb * yb + zb * zb)) * 180.0f / (float)M_PI;
    out->tilt_roll  = atan2f(yb, sqrtf(xb * xb + zb * zb)) * 180.0f / (float)M_PI;

    ESP_LOGI(TAG, "window n=%u rms=%.1f peak=%.1f pitch=%.1f roll=%.1f",
             n, out->movement_rms, out->movement_peak, out->tilt_pitch, out->tilt_roll);
}
