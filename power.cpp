// power.cpp — Battery measurement + SoC (spec §7).

#include "power.h"
#include "config.h"

// Piecewise SoC breakpoints (spec §7 table). Ordered high -> low voltage.
struct SocPoint { float v; uint8_t soc; };
static const SocPoint kSocCurve[] = {
  {4.20f, 100},
  {4.00f,  75},
  {3.85f,  50},
  {3.70f,  30},
  {3.50f,  15},
  {3.35f,   5},
  {3.30f,   0},
};

uint8_t socFromVoltage(float v) {
  if (v >= kSocCurve[0].v) return 100;
  const size_t n = sizeof(kSocCurve) / sizeof(kSocCurve[0]);
  if (v <= kSocCurve[n - 1].v) return 0;

  // Linear interpolation *within* each segment; the curve as a whole is piecewise.
  for (size_t i = 0; i + 1 < n; i++) {
    float vhi = kSocCurve[i].v,     vlo = kSocCurve[i + 1].v;
    if (v <= vhi && v > vlo) {
      float shi = kSocCurve[i].soc, slo = kSocCurve[i + 1].soc;
      float frac = (v - vlo) / (vhi - vlo);
      return (uint8_t)lroundf(slo + frac * (shi - slo));
    }
  }
  return 0;
}

BatteryReading readBattery(float vbat_ratio) {
  // Average 16 samples of the calibrated ADC (§7). analogReadMilliVolts applies
  // the ESP32-H2's own calibration; vbat_ratio undoes the on-board divider.
  uint32_t acc_mv = 0;
  const int kSamples = 16;
  for (int i = 0; i < kSamples; i++) {
    acc_mv += analogReadMilliVolts(PIN_VBAT);
  }
  float pin_v = (acc_mv / (float)kSamples) / 1000.0f;
  float cell_v = pin_v * vbat_ratio;

  BatteryReading r;
  r.volts = cell_v;
  r.soc   = socFromVoltage(cell_v);
  DBG("[batt] pin=%.3fV cell=%.3fV soc=%u%%\n", pin_v, cell_v, r.soc);
  return r;
}
