// adxl345.cpp — ADXL345 register-level driver (spec §5, §6).

#include "adxl345.h"
#include "config.h"
#include <Wire.h>
#include <math.h>

void Adxl345::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t Adxl345::readReg(uint8_t reg) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.endTransmission(false); // repeated start, keep bus
  Wire.requestFrom(addr_, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

bool Adxl345::probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool Adxl345::begin() {
  // Scan both possible addresses rather than hardcoding (§3.1).
  uint8_t candidates[2] = { ADXL345_ADDR_LOW, ADXL345_ADDR_HIGH };
  for (uint8_t c : candidates) {
    if (probe(c)) {
      addr_ = c;
      DBG("[adxl] responded at 0x%02X\n", addr_);
      break;
    }
  }
  if (addr_ == 0) {
    DBGLN("[adxl] no device on bus (checked 0x53, 0x1D)");
    return false;
  }

  uint8_t devid = readReg(ADXL_DEVID);
  DBG("[adxl] DEVID=0x%02X (expect 0x%02X)\n", devid, ADXL_DEVID_EXPECTED);
  if (devid != ADXL_DEVID_EXPECTED) {
    DBGLN("[adxl] DEVID mismatch — wrong part or bad wiring (check CS tied high, §8.8)");
    return false;
  }
  return true;
}

void Adxl345::configureAccel(uint8_t act_thresh_raw) {
  // Order per §5.1. Configure while in standby-ish state, then enable measure last.
  writeReg(ADXL_DATA_FORMAT,   VAL_DATA_FORMAT);   // FULL_RES, +/-2g, active-high INT
  writeReg(ADXL_BW_RATE,       VAL_BW_RATE_25HZ);  // low-power, 25 Hz idle
  writeReg(ADXL_THRESH_ACT,    act_thresh_raw);    // wake sensitivity (62.5 mg/LSB)
  writeReg(ADXL_ACT_INACT_CTL, VAL_ACT_INACT_CTL); // AC-coupled X/Y/Z activity
  writeReg(ADXL_INT_MAP,       VAL_INT_MAP);       // ACTIVITY -> INT1
  writeReg(ADXL_INT_ENABLE,    VAL_INT_ENABLE);    // ACTIVITY only
  writeReg(ADXL_POWER_CTL,     VAL_POWER_CTL);     // measure mode

  clearInterrupt(); // start from a clean INT1 state

  DBG("[adxl] configured, THRESH_ACT raw=%u (~%.0f mg)\n",
      act_thresh_raw, act_thresh_raw * ADXL_THRESH_ACT_MG_PER_LSB);
}

void Adxl345::setOdr50() { writeReg(ADXL_BW_RATE, VAL_BW_RATE_50HZ); }
void Adxl345::setOdr25() { writeReg(ADXL_BW_RATE, VAL_BW_RATE_25HZ); }

uint8_t Adxl345::clearInterrupt() {
  return readReg(ADXL_INT_SOURCE); // reading deasserts INT1 (§4.4, §8.7)
}

void Adxl345::rearmActivity() {
  // Re-reference AC-coupled activity to the current resting position (§4.4):
  // clear then re-set the ACTIVITY enable bit.
  writeReg(ADXL_INT_ENABLE, 0x00);
  writeReg(ADXL_INT_ENABLE, VAL_INT_ENABLE);
  clearInterrupt();
}

bool Adxl345::readSample(int16_t &x, int16_t &y, int16_t &z) {
  // Single 6-byte burst so all three axes come from the same sample (§6).
  Wire.beginTransmission(addr_);
  Wire.write(ADXL_DATAX0);
  Wire.endTransmission(false);
  uint8_t got = Wire.requestFrom(addr_, (uint8_t)6);
  if (got != 6) return false;

  uint8_t b[6];
  for (uint8_t i = 0; i < 6; i++) b[i] = Wire.read();

  // 16-bit two's complement, little-endian (§6).
  x = (int16_t)((b[1] << 8) | b[0]);
  y = (int16_t)((b[3] << 8) | b[2]);
  z = (int16_t)((b[5] << 8) | b[4]);
  return true;
}

AccelMetrics Adxl345::measureWindow() {
  // Software float is fine at this data rate even without an FPU (§6).
  const uint16_t N = WINDOW_SAMPLES;
  const uint32_t period_us = 1000000UL / SAMPLE_RATE_HZ;

  float mag[WINDOW_SAMPLES];
  float sx = 0, sy = 0, sz = 0;   // for the mean tilt vector
  double sum_mag = 0;
  uint16_t n = 0;

  for (uint16_t i = 0; i < N; i++) {
    uint32_t t0 = micros();
    int16_t xr, yr, zr;
    if (readSample(xr, yr, zr)) {
      float x = xr * ADXL_MG_PER_LSB;
      float y = yr * ADXL_MG_PER_LSB;
      float z = zr * ADXL_MG_PER_LSB;
      float m = sqrtf(x * x + y * y + z * z);
      mag[n] = m;
      sum_mag += m;
      sx += x; sy += y; sz += z;
      n++;
    }
    // pace the loop to the sample rate
    uint32_t elapsed = micros() - t0;
    if (elapsed < period_us) delayMicroseconds(period_us - elapsed);
  }

  AccelMetrics out{0, 0, 0, 0};
  if (n == 0) return out; // no samples read — caller can treat as fault

  float baseline = (float)(sum_mag / n);

  double sq = 0;
  float peak = 0;
  for (uint16_t i = 0; i < n; i++) {
    float d = mag[i] - baseline;
    sq += (double)d * d;
    float ad = fabsf(d);
    if (ad > peak) peak = ad;
  }
  out.movement_rms  = sqrtf((float)(sq / n));
  out.movement_peak = peak;

  // Tilt from the mean vector, degrees (§6).
  float xb = sx / n, yb = sy / n, zb = sz / n;
  out.tilt_pitch = atan2f(xb, sqrtf(yb * yb + zb * zb)) * 180.0f / (float)M_PI;
  out.tilt_roll  = atan2f(yb, sqrtf(xb * xb + zb * zb)) * 180.0f / (float)M_PI;

  DBG("[adxl] window n=%u rms=%.1f peak=%.1f pitch=%.1f roll=%.1f (mg/deg)\n",
      n, out.movement_rms, out.movement_peak, out.tilt_pitch, out.tilt_roll);
  return out;
}
