// power.h — Battery measurement, state-of-charge, low-voltage lockout (spec §7).

#pragma once

#include <Arduino.h>

struct BatteryReading {
  float   volts;    // cell voltage
  uint8_t soc;      // 0-100 %, from the piecewise curve
};

// Read IO2 (on-board divider, §3.1): average 16 samples of analogReadMilliVolts,
// apply vbat_ratio. Returns cell volts + SoC.
BatteryReading readBattery(float vbat_ratio);

// Piecewise Li-Ion SoC lookup (§7). NOT linear — the discharge curve is flat
// in the middle.
uint8_t socFromVoltage(float volts);

// Below the lockout threshold, the caller must send one final report and then
// enter PERMANENT deep sleep (no timer wake). Over-discharge damages the cell (§7).
static constexpr float VBAT_LOCKOUT_V = 3.25f;
