// config.h — All tunables in one place (spec §9, §13).
//
// Every runtime-tunable value from spec §9 is stored in NVS (namespace "awning")
// and loaded at boot with the defaults below. Nothing in the rest of the firmware
// should contain a magic number that belongs here.
//
// The compile-time pin/register constants that are NOT user-tunable live here too,
// so there is exactly one place to look.

#pragma once

#include <Arduino.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
// Serial on a sleeping battery device is a power leak and adds boot delay (§13).
// Leave DEBUG_SERIAL undefined for battery builds; define it for bench work.
// #define DEBUG_SERIAL

#ifdef DEBUG_SERIAL
  #define DBG_BEGIN(baud) do { Serial.begin(baud); delay(50); } while (0)
  #define DBG(...)        do { Serial.printf(__VA_ARGS__); } while (0)
  #define DBGLN(s)        do { Serial.println(s); } while (0)
#else
  #define DBG_BEGIN(baud) do {} while (0)
  #define DBG(...)        do {} while (0)
  #define DBGLN(s)        do {} while (0)
#endif

// ---------------------------------------------------------------------------
// Pin map (spec §3.1). LaskaKit ESP32-H2-DEVKit V2.
// ---------------------------------------------------------------------------
static constexpr gpio_num_t PIN_SDA  = GPIO_NUM_11; // IO11 -> ADXL345 SDA
static constexpr gpio_num_t PIN_SCL  = GPIO_NUM_10; // IO10 -> ADXL345 SCL
static constexpr gpio_num_t PIN_INT1 = GPIO_NUM_13; // IO13 -> ADXL345 INT1 (LP wake pin, §3.3)
static constexpr gpio_num_t PIN_BTN  = GPIO_NUM_14; // IO14 -> pairing button (spec's spare LP pin, §3.3)
static constexpr uint8_t    PIN_VBAT = 2;           // IO2  -> on-board battery divider (§3.1)

// Button wiring: momentary switch from IO14 to GND, using the internal pull-up.
// Idle = HIGH, pressed = LOW (active-low). It wakes the device from deep sleep;
// on the ESP32-H2 that is done with EXT1, which supports per-pin trigger levels,
// so IO13 (active-high accel INT) and IO14 (active-low button) can share one wake
// config. See the state machine in the .ino.
static constexpr int BTN_ACTIVE_LEVEL = 0; // pressed reads LOW

// EXT1 wake masks (ESP32-H2 deep-sleep GPIO wake is EXT1, not gpio_wakeup — verified
// against the ESP-IDF ESP32-H2 sleep docs). io_mask = both pins we wake on; the
// level_mask bit is 1 for a HIGH-trigger pin, 0 for LOW-trigger.
static constexpr uint64_t EXT1_IO_MASK    = (1ULL << PIN_INT1) | (1ULL << PIN_BTN);
static constexpr uint64_t EXT1_LEVEL_MASK = (1ULL << PIN_INT1); // INT1 high-trigger; BTN low-trigger (bit left 0)

// Pins we must NEVER drive (spec §3.2, §8.10):
//   IO12 = uSup load switch. Driving it would cut power to the ADXL345 during
//          deep sleep, killing the wake-on-movement that this whole design relies on.
//   IO9  = RGB LED. Addressable LED controller draws 0.5-1 mA continuously even when
//          showing "black". Never initialise or drive it.
// They are intentionally absent from this map. Do not add them.

// ---------------------------------------------------------------------------
// Measurement window (spec §6). Not user-tunable; changing these changes the
// meaning of the reported numbers, so they are fixed constants, not NVS keys.
// ---------------------------------------------------------------------------
static constexpr uint16_t SAMPLE_RATE_HZ = 50;                          // active ODR (§5.2)
static constexpr uint16_t WINDOW_MS      = 4000;                        // §6
static constexpr uint16_t WINDOW_SAMPLES = (SAMPLE_RATE_HZ * WINDOW_MS) / 1000; // = 200

// ---------------------------------------------------------------------------
// NVS-backed configuration (spec §9). Loaded once at boot into this struct.
// ---------------------------------------------------------------------------
struct AwningConfig {
  uint8_t  act_thresh;   // ADXL345 THRESH_ACT raw value (x62.5 mg). Wake sensitivity.
  uint32_t hb_interval;  // Heartbeat interval, seconds.
  uint16_t act_report;   // Report interval during a wind event, seconds.
  uint16_t quiet_mg;     // movement_rms below this counts as a "quiet" window.
  uint8_t  quiet_win;    // Consecutive quiet windows before returning to sleep.
  uint16_t max_active;   // Runaway guard for the wind-event loop, minutes.
  float    vbat_ratio;   // On-board divider ratio. MUST be calibrated (§7).
};

// Defaults, exactly per spec §9 table.
namespace cfgdefault {
  static constexpr uint8_t  ACT_THRESH  = 2;      // -> 125 mg wake threshold
  static constexpr uint32_t HB_INTERVAL = 1800;   // 30 min (§10 sweet spot)
  static constexpr uint16_t ACT_REPORT  = 30;
  static constexpr uint16_t QUIET_MG    = 60;
  static constexpr uint8_t  QUIET_WIN   = 3;
  static constexpr uint16_t MAX_ACTIVE  = 30;
  // TODO(hardware): vbat_ratio 2.0 is a placeholder. Calibrate per §7 / acceptance
  // test 4: measure the cell with a multimeter at two states of charge, compare
  // against analogReadMilliVolts(IO2), and derive the real constant. Cross-check
  // against the schematic in the LaskaKit repo HW/ folder.
  static constexpr float    VBAT_RATIO  = 2.0f;
}

// Load config from NVS, applying defaults on first boot. Persists any missing keys.
void loadConfig(AwningConfig &cfg);
