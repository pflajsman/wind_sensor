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
#define DEBUG_SERIAL

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
static constexpr uint8_t    PIN_VBAT = 3;           // IO3  -> on-board battery divider (verified on hardware; LaskaKit ESP32-H2 DEVKit)

// EXT1 wake mask (ESP32-H2 deep-sleep GPIO wake is EXT1, not gpio_wakeup — verified
// against the ESP-IDF ESP32-H2 sleep docs). We wake on a single pin, IO13 (accel INT1),
// which is active-high, so a plain ANY_HIGH wake config covers it.
static constexpr uint64_t EXT1_IO_MASK = (1ULL << PIN_INT1); // INT1 high-trigger

// Pins we must NEVER drive (spec §3.2, §8.10):
//   IO12 = uSup load switch. Driving it would cut power to the ADXL345 during
//          deep sleep, killing the wake-on-movement that this whole design relies on.
//   IO9  = RGB LED. Addressable LED controller draws 0.5-1 mA continuously even when
//          showing "black". Never initialise or drive it.
// They are intentionally absent from this map. Do not add them.

// ---------------------------------------------------------------------------
// Zigbee network channel (deCONZ/ConBee).
// ---------------------------------------------------------------------------
// Pinning the ED to the coordinator's single channel is Espressif's recommended fix
// for a deep-sleep device that must rejoin on every wake: a silent rejoin then scans
// exactly ONE channel instead of all 16, so it is fast and reliably beats the join
// timeout. Set this to the channel your deCONZ network is on (Phoscon -> Settings ->
// Gateway -> Advanced shows it). MUST match, or the device will never find its parent.
// If you ever move the deCONZ network to another channel, change this and re-flash.
static constexpr uint8_t ZB_CHANNEL = 15;               // deCONZ network channel (11..26)
static constexpr uint32_t ZB_CHANNEL_MASK = (1UL << ZB_CHANNEL);

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
  // LaskaKit ESP32-H2-DEVKit V2 silkscreen states the divider ratio is 1.769 on IO3.
  // (Confirmed on hardware: cell 4.14 V -> pin should read ~2.34 V.) If you ever need
  // to re-calibrate, measure the cell with a multimeter, compare against
  // analogReadMilliVolts(PIN_VBAT), and derive ratio = cell / pin.
  static constexpr float    VBAT_RATIO  = 1.769f;
}

// Load config from NVS, applying defaults on first boot. Persists any missing keys.
void loadConfig(AwningConfig &cfg);
