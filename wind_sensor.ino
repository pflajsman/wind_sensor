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

// TEMPORARY DIAGNOSTIC: force core debug logging on at build time so the Zigbee-core
// wrapper's log_i/log_e (carrying the post-sleep rejoin state) compile in. Must precede
// <Arduino.h>. The Arduino IDE menu "Tools -> Core Debug Level -> Info" is the reliable
// path; set both. Remove (and set the menu back to None) for battery builds — logging
// costs power and boot time. 3 = INFO.
#ifndef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL 3
#endif

#include <Arduino.h>
#include <Wire.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_log.h"     // runtime log-level control to surface ZBOSS rejoin messages

#include "config.h"
#include "adxl345.h"
#include "power.h"
#include "zb.h"

static Adxl345 accel;
static AwningConfig cfg;

// INT1 is active-high while DATA_FORMAT INT_INVERT = 0 (VAL_DATA_FORMAT, §4.4).
// On the ESP32-H2, deep-sleep GPIO wake is EXT1 (NOT the light-sleep gpio_wakeup API).
// We wake on the single accel INT1 pin (IO13, high-trigger), configured via EXT1_IO_MASK
// in config.h. (Verified against the ESP-IDF ESP32-H2 sleep-modes docs.)

// Join timeout. The heartbeat path can be short (it either rejoins fast or waits for
// the next wake), but cold-boot *commissioning* regularly needs ~9-10 s of network
// steering on the ESP32-H2 — a 10 s cap raced the clock and lost most joins
// (observed: successes at 9.6-9.7 s). Give commissioning a comfortable margin.
static constexpr uint32_t ZB_JOIN_TIMEOUT_MS    = 25000; // routine rejoin (heartbeat/wind).
// A silent NWK rejoin after deep sleep can need an all-channel scan (>10 s), so give it a
// realistic window. On timeout the firmware does NOT erase/re-steer — it just sleeps and
// retries next wake, with the stored network intact (see zbBegin recovery logic).
static constexpr uint32_t ZB_COMMISSION_TIMEOUT_MS = 40000; // fresh join (cold boot)

// After a fresh join, stay awake this long so deCONZ can push its Bind +
// Configure-Reporting to this sleepy ED — otherwise the binding table stays empty
// and values never populate (see zb.cpp zbSettleForBindings / integration README).
static constexpr uint32_t ZB_SETTLE_MS = 90000;

// ---------------------------------------------------------------------------
// Sleep entry (spec §4.4). The ORDER here matters.
// ---------------------------------------------------------------------------
static void enterDeepSleep(uint32_t timer_wake_s) {
  accel.setOdr25();          // idle ODR (§5.2)
  accel.rearmActivity();     // re-reference AC activity to current rest pos (§4.4)
  accel.clearInterrupt();    // MUST read INT_SOURCE or INT1 stays asserted -> instant-wake loop (§8.7)

  // Arm EXT1 deep-sleep wake on IO13 (accel INT1, active-high) (§4.4, §3.3). With a
  // single high-trigger pin we use the non-deprecated esp_sleep_enable_ext1_wakeup_io()
  // in ANY_HIGH mode. (Verified against the ESP-IDF ESP32-H2 sleep-modes docs.)
  esp_sleep_enable_ext1_wakeup_io(EXT1_IO_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);

  if (timer_wake_s > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)timer_wake_s * 1000000ULL);
    DBG("[sleep] ext1(IO13 hi) + timer %us\n", timer_wake_s);
  } else {
    DBGLN("[sleep] ext1(IO13 hi) only — PERMANENT (low-voltage lockout)");
  }

  Serial.flush();
  esp_deep_sleep_start();
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

  if (!zbBegin(ZB_COMMISSION_TIMEOUT_MS)) { // start ED, commission if needed (§4.1.5)
    DBGLN("[boot] Zigbee not connected yet — will report on next successful join");
  }

  accel.setOdr50();
  AccelMetrics m = accel.measureWindow(); // one measurement (§4.1.6)
  if (zbConnected()) {
    zbReportMetrics(m);
    // Give deCONZ its window to write bindings + reporting config before we sleep.
    zbSettleForBindings(m, ZB_SETTLE_MS);
  }
  if (reportBatteryAndCheckLockout()) return;

  enterDeepSleep(cfg.hb_interval);
}

// ---------------------------------------------------------------------------
// Arduino entry points. All work happens here; loop() is never used.
// ---------------------------------------------------------------------------
void setup() {
  DBG_BEGIN(115200);

#ifdef DEBUG_SERIAL
  // TEMPORARY DIAGNOSTIC: surface the ZBOSS / Zigbee-core log messages at runtime so a
  // failed post-sleep rejoin prints its real reason (e.g. "Rejoin failure: no dev with
  // xpanid", "can't choose best parent"). These come from the precompiled stack via
  // esp_log; without this they are silent and we only see our own [zb] lines. The core
  // wrapper's own log_i/log_e still need Core Debug Level >= Info at BUILD time to appear
  // (Arduino IDE: Tools -> Core Debug Level -> Info), but this at least unmasks esp_log.
  // Remove once the rejoin issue is understood — verbose logging costs power/time.
  esp_log_level_set("*", ESP_LOG_INFO);
#endif

  // I2C init (§4.1.2). ADXL345 CS must be tied HIGH in hardware to select I2C (§8.8).
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  // INT1 as a plain input; the RTC/LP wake is armed at sleep time (§4.4).
  pinMode(PIN_INT1, INPUT);

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
    case ESP_SLEEP_WAKEUP_EXT1:       // IO13 accel INT -> wind event (§4.2)
      loadConfig(cfg);
      accel.configureAccel(cfg.act_thresh); // ensure regs valid after wake
      zbBegin(ZB_JOIN_TIMEOUT_MS);
      doWindEvent();
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
