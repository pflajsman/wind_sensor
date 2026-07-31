// adxl345.h — Hand-rolled ADXL345 driver (spec §5, §6).
//
// Written directly against the register map rather than a vendor library, so that
// nothing silently reconfigures BW_RATE or POWER_CTL behind our back (§13).
//
// !!! VERIFY EVERY REGISTER ADDRESS, BIT POSITION AND SCALING CONSTANT AGAINST
// !!! THE ADXL345 DATASHEET (Analog Devices Rev. G) BEFORE TRUSTING THIS (§5.3).
// !!! In particular the two scaling constants below drive the entire calibration.

#pragma once

#include <Arduino.h>

// --- I2C addresses (spec §3.1) --------------------------------------------
// SDO/ALT ADDRESS low -> 0x53, high -> 0x1D. Do not hardcode; scan for both.
static constexpr uint8_t ADXL345_ADDR_LOW  = 0x53;
static constexpr uint8_t ADXL345_ADDR_HIGH = 0x1D;

// --- Register map (spec §5). TODO(datasheet): confirm each address, Rev. G. ---
enum : uint8_t {
  ADXL_DEVID        = 0x00, // reads 0xE5
  ADXL_THRESH_ACT   = 0x24, // activity threshold, 62.5 mg/LSB  (TODO: verify scale)
  ADXL_ACT_INACT_CTL= 0x27,
  ADXL_BW_RATE      = 0x2C,
  ADXL_POWER_CTL    = 0x2D,
  ADXL_INT_ENABLE   = 0x2E,
  ADXL_INT_MAP      = 0x2F,
  ADXL_INT_SOURCE   = 0x30, // reading this clears/deasserts INT1 (§4.4, §8.7)
  ADXL_DATA_FORMAT  = 0x31,
  ADXL_DATAX0       = 0x32, // X0,X1,Y0,Y1,Z0,Z1 — 6-byte burst (§6)
};

static constexpr uint8_t ADXL_DEVID_EXPECTED = 0xE5;

// --- Scaling constants (spec §5, §6). TODO(datasheet): verify against Rev. G. ---
// FULL_RES gives a fixed 3.9 mg/LSB at every range (§5). THRESH_ACT is 62.5 mg/LSB.
static constexpr float ADXL_MG_PER_LSB          = 3.9f;   // FULL_RES data (§5, §6)
static constexpr float ADXL_THRESH_ACT_MG_PER_LSB = 62.5f; // THRESH_ACT (§5.1)

// Register values (spec §5.1 / §5.2). Named so the state machine never writes
// raw registers inline (§13).
static constexpr uint8_t VAL_DATA_FORMAT   = 0x08; // FULL_RES, +/-2g, INT_INVERT=0 (active-high)
static constexpr uint8_t VAL_BW_RATE_25HZ  = 0x18; // LOW_POWER, ODR 25 Hz  (sleep/idle)
static constexpr uint8_t VAL_BW_RATE_50HZ  = 0x19; // LOW_POWER, ODR 50 Hz  (active window)
static constexpr uint8_t VAL_ACT_INACT_CTL = 0xF0; // ACT_AC + X/Y/Z activity enabled
static constexpr uint8_t VAL_INT_MAP       = 0x00; // ACTIVITY -> INT1 pin
static constexpr uint8_t VAL_INT_ENABLE    = 0x10; // ACTIVITY only (bit 4)
static constexpr uint8_t VAL_POWER_CTL     = 0x08; // Measure mode (bit 3)

// One measurement window's worth of derived metrics (spec §6).
struct AccelMetrics {
  float movement_rms;   // mg — primary AC-energy metric
  float movement_peak;  // mg
  float tilt_pitch;     // degrees
  float tilt_roll;      // degrees
};

class Adxl345 {
 public:
  // Scan the bus for 0x53/0x1D and verify DEVID == 0xE5. Returns false on failure.
  bool begin();

  // Full base configuration, applied once at cold boot (§5.1).
  void configureAccel(uint8_t act_thresh_raw);

  // Output-data-rate switches for active windows (§5.2). Resolution unchanged.
  void setOdr50();
  void setOdr25();

  // Re-reference and re-arm AC-coupled activity detection before sleeping (§4.4).
  // Clearing then re-setting the ACTIVITY bit makes the ADXL345 re-latch the
  // current resting acceleration as its reference.
  void rearmActivity();

  // Read INT_SOURCE to clear the interrupt / deassert INT1 (§4.4, §8.7).
  uint8_t clearInterrupt();

  // Capture one WINDOW_SAMPLES window at the current ODR and compute metrics (§6).
  AccelMetrics measureWindow();

  uint8_t address() const { return addr_; }

 private:
  uint8_t addr_ = 0;

  void    writeReg(uint8_t reg, uint8_t val);
  uint8_t readReg(uint8_t reg);
  bool    readSample(int16_t &x, int16_t &y, int16_t &z); // 6-byte burst
  bool    probe(uint8_t addr);
};
