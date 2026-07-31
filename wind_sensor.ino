// wind_sensor.ino — Awning wind-protection sensor firmware.
//
// LaskaKit ESP32-H2-DEVKit V2 + ADXL345 (GY-291), Zigbee end device.
// See awning-wind-sensor-spec.md. Read spec §8 (pitfalls) before touching this.
//
// This is a wake-reason-driven state machine (§4). There is NO polling loop():
// the device runs one path to completion on each wake and returns to deep sleep.
//
// Build settings (§7):
//   Board: ESP32-H2 Dev Module (or LaskaKit definition)
//   Tools -> Zigbee mode -> "Zigbee ED (end device)"
//   Partition scheme: one with Zigbee support
// Do NOT use erase_flash when reflashing — it wipes the IEEE address and orphans
// the pairing (§8.1). Plain upload only.
//
// Pins IO9 (RGB LED) and IO12 (uSup load switch) are NEVER driven here. See §3.2
// and §8.10 — driving IO12 would cut the accelerometer's power in deep sleep and
// defeat wake-on-movement; IO9's LED controller leaks current even when "off".

#include <Arduino.h>
#include <Wire.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

#include "config.h"
#include "adxl345.h"
#include "power.h"
#include "zb.h"

static Adxl345 accel;
static AwningConfig cfg;

// INT1 is active-high while DATA_FORMAT INT_INVERT = 0 (VAL_DATA_FORMAT, §4.4).
// The button (IO14) is active-low. Both wake levels are encoded in EXT1_LEVEL_MASK
// in config.h. On the ESP32-H2, deep-sleep GPIO wake is EXT1 (NOT the light-sleep
// gpio_wakeup API), and EXT1 supports per-pin trigger levels in one call — which is
// what lets a high-trigger INT1 and a low-trigger button share one wake config.
// (Verified against the ESP-IDF ESP32-H2 sleep-modes docs.)

static constexpr uint32_t ZB_JOIN_TIMEOUT_MS = 10000; // allow for slow ED rejoin (§8.5)

// ---------------------------------------------------------------------------
// Sleep entry (spec §4.4). The ORDER here matters.
// ---------------------------------------------------------------------------
static void enterDeepSleep(uint32_t timer_wake_s) {
  accel.setOdr25();          // idle ODR (§5.2)
  accel.rearmActivity();     // re-reference AC activity to current rest pos (§4.4)
  accel.clearInterrupt();    // MUST read INT_SOURCE or INT1 stays asserted -> instant-wake loop (§8.7)

  // Hold the button's internal pull-up through deep sleep so IO14 idles HIGH and a
  // press pulls it LOW to wake. Without this the pull is disabled in deep sleep and
  // the pin could float, causing spurious or missed wakes.
  gpio_pullup_en(PIN_BTN);
  gpio_pulldown_dis(PIN_BTN);

  // Arm EXT1 deep-sleep wake on IO13 (high) + IO14 (low) via the level mask (§4.4,
  // §3.3). io_mask selects the pins; level_mask sets each pin's trigger level.
  esp_sleep_enable_ext1_wakeup_with_level_mask(EXT1_IO_MASK, EXT1_LEVEL_MASK);

  if (timer_wake_s > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)timer_wake_s * 1000000ULL);
    DBG("[sleep] ext1(IO13 hi / IO14 lo) + timer %us\n", timer_wake_s);
  } else {
    DBGLN("[sleep] ext1(IO13 hi / IO14 lo) only — PERMANENT (low-voltage lockout)");
  }

  Serial.flush();
  esp_deep_sleep_start();
}

// Which EXT1 pin caused this wake? esp_sleep_get_ext1_wakeup_status() returns a
// bitmask of the pins that were asserted at wake. A button press and an accel INT
// both land as ESP_SLEEP_WAKEUP_EXT1, so this is how we tell them apart (§4.2).
static bool wokenByButton() {
  uint64_t status = esp_sleep_get_ext1_wakeup_status();
  return (status & (1ULL << PIN_BTN)) != 0;
}

// ---------------------------------------------------------------------------
// Battery + lockout. Returns true if lockout tripped (caller must stop) (§7).
// ---------------------------------------------------------------------------
static bool reportBatteryAndCheckLockout() {
  BatteryReading b = readBattery(cfg.vbat_ratio);
  if (zbConnected()) zbReportBattery(b.soc, b.volts);

  if (b.volts <= VBAT_LOCKOUT_V) {
    DBG("[batt] LOCKOUT %.3fV <= %.2fV — final report then permanent sleep\n",
        b.volts, VBAT_LOCKOUT_V);
    enterDeepSleep(0); // no timer wake — never comes back on its own (§7)
    return true;       // not reached, but keeps intent explicit
  }
  return false;
}

// ---------------------------------------------------------------------------
// HEARTBEAT path (spec §4.3): one window + battery, keeps HA watchdog fed.
// ---------------------------------------------------------------------------
static void doHeartbeat() {
  DBGLN("[state] HEARTBEAT");
  accel.setOdr50();
  AccelMetrics m = accel.measureWindow();
  if (zbConnected()) zbReportMetrics(m);
  if (reportBatteryAndCheckLockout()) return;
  enterDeepSleep(cfg.hb_interval);
}

// ---------------------------------------------------------------------------
// WIND_EVENT path (spec §4.2): stay awake across gusts, do NOT sleep between
// reports (each cold rejoin costs ~3 s @ 60 mA, §4.2 / §8.5).
// ---------------------------------------------------------------------------
static void doWindEvent() {
  DBGLN("[state] WIND_EVENT");
  accel.clearInterrupt();  // read INT_SOURCE to clear the wake (§4.2 step 1)
  accel.setOdr50();        // active ODR (§5.2)

  const uint32_t max_active_ms = (uint32_t)cfg.max_active * 60UL * 1000UL;
  const uint32_t started = millis();
  uint8_t quiet_count = 0;

  for (;;) {
    AccelMetrics m = accel.measureWindow();
    if (zbConnected()) zbReportMetrics(m);

    if (m.movement_rms < cfg.quiet_mg) {
      if (++quiet_count >= cfg.quiet_win) {
        DBGLN("[wind] quiet windows reached — exiting active loop");
        break;
      }
    } else {
      quiet_count = 0;
    }

    // Runaway guard: a stuck INT1 must not drain the cell (§4.2).
    if (millis() - started >= max_active_ms) {
      DBGLN("[wind] max_active guard tripped — forcing sleep");
      break;
    }

    // Wait active_report_interval between windows (§4.2 step 3).
    delay((uint32_t)cfg.act_report * 1000UL);
  }

  if (reportBatteryAndCheckLockout()) return;
  enterDeepSleep(cfg.hb_interval);
}

// ---------------------------------------------------------------------------
// COLD_BOOT path (spec §4.1): full init, one measurement, then sleep.
// ---------------------------------------------------------------------------
static void doColdBoot() {
  DBGLN("[state] COLD_BOOT");

  loadConfig(cfg);                  // NVS + defaults (§4.1.3, §9)
  accel.configureAccel(cfg.act_thresh); // full base config (§5.1)

  if (!zbBegin(ZB_JOIN_TIMEOUT_MS)) {   // start ED, commission if needed (§4.1.5)
    DBGLN("[boot] Zigbee not connected yet — will report on next successful join");
  }

  accel.setOdr50();
  AccelMetrics m = accel.measureWindow(); // one measurement (§4.1.6)
  if (zbConnected()) zbReportMetrics(m);
  if (reportBatteryAndCheckLockout()) return;

  enterDeepSleep(cfg.hb_interval);
}

// ---------------------------------------------------------------------------
// BUTTON path (IO14 press). Short press = wake + one immediate report. If the
// device is not on a network yet, force fresh commissioning first so the button
// doubles as the pairing trigger the user asked for.
// ---------------------------------------------------------------------------
static void doButtonPress() {
  DBGLN("[state] BUTTON");

  // Simple debounce: require the pin to still read pressed a moment later, so a
  // contact bounce or EMI spike does not count as a press.
  delay(30);
  if (digitalRead(PIN_BTN) != BTN_ACTIVE_LEVEL) {
    DBGLN("[btn] bounce/no-press — going back to sleep");
    enterDeepSleep(cfg.hb_interval);
    return;
  }

  loadConfig(cfg);
  accel.configureAccel(cfg.act_thresh);

  // Bring Zigbee up. If it can't connect (never joined, or dropped), force a fresh
  // commissioning attempt — this is the "put it in pairing mode" action. Open the
  // network in the coordinator (Phoscon permit-join / deCONZ Permit Join) first.
  if (!zbBegin(ZB_JOIN_TIMEOUT_MS)) {
    DBGLN("[btn] not connected — forcing commissioning (open the coordinator network now)");
    zbForceCommission(ZB_JOIN_TIMEOUT_MS);
  }

  accel.setOdr50();
  AccelMetrics m = accel.measureWindow(); // on-demand report — good for bench test
  if (zbConnected()) zbReportMetrics(m);
  if (reportBatteryAndCheckLockout()) return;

  enterDeepSleep(cfg.hb_interval);
}

// ---------------------------------------------------------------------------
// Arduino entry points. All work happens here; loop() is never used.
// ---------------------------------------------------------------------------
void setup() {
  DBG_BEGIN(115200);

  // I2C init (§4.1.2). ADXL345 CS must be tied HIGH in hardware to select I2C (§8.8).
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  // INT1 as a plain input; the RTC/LP wake is armed at sleep time (§4.4).
  pinMode(PIN_INT1, INPUT);
  // Button on IO14: internal pull-up, active-low. Idles HIGH, press pulls LOW.
  pinMode(PIN_BTN, INPUT_PULLUP);

  // Bring up (or re-detect) the accelerometer on every wake so a hot-path wake
  // still has a valid handle. Verifies 0xE5 (§4.1.2).
  if (!accel.begin()) {
    // Fail loudly (§1): can't measure, so feed the watchdog with battery only,
    // then sleep for one heartbeat and try again on the next wake.
    DBGLN("[boot] ADXL345 init FAILED — reporting battery only");
    loadConfig(cfg);
    zbBegin(ZB_JOIN_TIMEOUT_MS);
    if (reportBatteryAndCheckLockout()) return;
    // Cannot rearm activity (no device); rely on timer wake only.
    esp_sleep_enable_timer_wakeup((uint64_t)cfg.hb_interval * 1000000ULL);
    Serial.flush();
    esp_deep_sleep_start();
    return;
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT1:       // IO13 accel INT (§4.2) OR IO14 button
      if (wokenByButton()) {
        // Button dispatch loads config / brings up Zigbee itself.
        doButtonPress();
      } else {
        loadConfig(cfg);
        accel.configureAccel(cfg.act_thresh); // ensure regs valid after wake
        zbBegin(ZB_JOIN_TIMEOUT_MS);
        doWindEvent();
      }
      break;

    case ESP_SLEEP_WAKEUP_TIMER:      // heartbeat (§4.3)
      loadConfig(cfg);
      accel.configureAccel(cfg.act_thresh);
      zbBegin(ZB_JOIN_TIMEOUT_MS);
      doHeartbeat();
      break;

    default:                          // power-on / reset -> cold boot (§4.1)
      doColdBoot();
      break;
  }

  // Every path above ends in esp_deep_sleep_start(); we never reach here.
}

void loop() {
  // Intentionally empty. The device is a run-to-completion state machine that
  // deep-sleeps rather than looping (§4). If execution ever reaches here, sleep.
  enterDeepSleep(cfg.hb_interval);
}
