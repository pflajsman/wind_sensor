/* adxl345.h — ADXL345 driver for the ESP-IDF port.
 *
 * Same register map, values, and scaling as the Arduino adxl345.h (verify against ADXL345
 * datasheet Rev. G — the two scaling constants drive the whole calibration). Only the bus
 * layer differs: IDF i2c_master instead of Arduino Wire.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* I2C addresses (SDO low -> 0x53, high -> 0x1D). */
#define ADXL345_ADDR_LOW   0x53
#define ADXL345_ADDR_HIGH  0x1D

/* Register map. */
#define ADXL_DEVID         0x00  /* reads 0xE5 */
#define ADXL_THRESH_ACT    0x24  /* 62.5 mg/LSB */
#define ADXL_ACT_INACT_CTL 0x27
#define ADXL_BW_RATE       0x2C
#define ADXL_POWER_CTL     0x2D
#define ADXL_INT_ENABLE    0x2E
#define ADXL_INT_MAP       0x2F
#define ADXL_INT_SOURCE    0x30  /* read clears/deasserts INT1 */
#define ADXL_DATA_FORMAT   0x31
#define ADXL_DATAX0        0x32  /* X0..Z1 6-byte burst */

#define ADXL_DEVID_EXPECTED 0xE5

/* Scaling (verify Rev. G). */
#define ADXL_MG_PER_LSB            3.9f
#define ADXL_THRESH_ACT_MG_PER_LSB 62.5f

/* Register values (spec §5.1/§5.2). */
#define VAL_DATA_FORMAT    0x08  /* FULL_RES, +/-2g, INT_INVERT=0 (active-high) */
#define VAL_BW_RATE_25HZ   0x18  /* LOW_POWER, ODR 25 Hz */
#define VAL_BW_RATE_50HZ   0x19  /* LOW_POWER, ODR 50 Hz */
#define VAL_ACT_INACT_CTL  0xF0  /* ACT_AC + X/Y/Z activity */
#define VAL_INT_MAP        0x00  /* ACTIVITY -> INT1 */
#define VAL_INT_ENABLE     0x10  /* ACTIVITY only */
#define VAL_POWER_CTL      0x08  /* Measure mode */

typedef struct {
    float movement_rms;
    float movement_peak;
    float tilt_pitch;
    float tilt_roll;
} accel_metrics_t;

/* Init the I2C bus, scan for 0x53/0x1D, verify DEVID. */
esp_err_t adxl345_init(void);

/* Full base config (cold boot). */
void adxl345_configure(uint8_t act_thresh_raw);

/* ODR switches. */
void adxl345_set_odr50(void);
void adxl345_set_odr25(void);

/* Re-reference + re-arm AC activity before sleep. */
void adxl345_rearm_activity(void);

/* Read INT_SOURCE to clear INT1. Returns the register value. */
uint8_t adxl345_clear_interrupt(void);

/* True if the ACTIVITY interrupt fired since the last INT_SOURCE read (also clears/re-arms). */
bool adxl345_activity_fired(void);

/* Capture one WINDOW_SAMPLES window and compute metrics. */
void adxl345_measure_window(accel_metrics_t *out);
