// rejoin_test.ino — MINIMAL deep-sleep rejoin test for the awning wind sensor.
//
// PURPOSE: strip the real firmware down to almost nothing and answer ONE question —
// does an ESP32-H2 Zigbee ED with OUR endpoints/model rejoin OUR deCONZ network after
// a deep-sleep wake, or does Zigbee.begin() hang ~30 s (the failure we see in the field)?
//
// This is deliberately as close as possible to the official arduino-esp32 example
// (libraries/Zigbee/examples/Zigbee_Temp_Hum_Sensor_Sleepy) — same structure: everything
// in setup(), deep sleep every TIME_TO_SLEEP, report-and-sleep in a FreeRTOS task,
// ESP.restart() on begin() failure. The ONLY things changed from the example are the
// application specifics that could plausibly affect binding/rejoin:
//   - four ZigbeeAnalog endpoints (10..13) instead of one temp sensor  (our real EPs)
//   - manufacturer "DIY" / model "awning-wind-sensor"                  (our real IDs)
//   - pinned to channel 15 (our deCONZ network)                        (rejoin-speed fix)
// No accelerometer, no battery, no NVS config, no recovery/erase logic, no button.
//
// If THIS rejoins reliably after sleep, the bug is complexity/ordering in the main
// firmware and we port the working shape back. If THIS also hangs 30 s, deep-sleep
// rejoin is not viable on this library for our setup and we switch strategy (light
// sleep / ESP-IDF).
//
// Build: Board = ESP32-H2 Dev Module, Tools -> Zigbee mode -> "Zigbee ED (end device)",
// a Zigbee partition scheme, Tools -> Core Debug Level -> "Info" (so the ZBOSS/core
// rejoin messages print). Plain Upload (never erase_flash — keeps the join).

// Force core debug logging ON at build time so the Zigbee-core wrapper's log_i/log_e
// (which carry the rejoin state) compile in. This MUST be defined before <Arduino.h>
// pulls in esp32-hal-log.h. NOTE: the Arduino IDE menu "Tools -> Core Debug Level -> Info"
// is the RELIABLE way — a sketch #define can be missed if the header was already included
// by a precompiled unit. Set BOTH: this define + the menu, to be sure. 3 = INFO.
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
#ifndef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL 3
#endif

#include <Arduino.h>
#ifndef ZIGBEE_MODE_ED
#error "Set Tools -> Zigbee mode -> 'Zigbee ED (end device)' and a Zigbee partition scheme."
#endif

#include "Zigbee.h"
#include "esp_log.h"

#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP  60          // seconds between wakes (short, for fast iteration)
#define REPORT_TIMEOUT 1000        // ms to wait for each report's coordinator ack
#define ZB_CHANNEL     15          // deCONZ network channel (must match!)
#define BEGIN_TIMEOUT  90000       // ms for Zigbee.begin() to report the stack STARTED.
                                   // Default is 30 s; we raise it to test whether the
                                   // rejoin is merely SLOW (would succeed given more time)
                                   // vs STUCK (hangs regardless). If begin() returns true
                                   // at, say, 45 s, longer timeout is a real workaround; if
                                   // it still returns false at 90 s, it is stuck, not slow.
#define CONNECT_TIMEOUT 90000      // ms to wait for connected() after begin() succeeds

// Our four Analog Input endpoints (same numbers as the real firmware).
static ZigbeeAnalog analog10(10);
static ZigbeeAnalog analog11(11);
static ZigbeeAnalog analog12(12);
static ZigbeeAnalog analog13(13);

// Ack tracking, exactly like the example: count down as reports are confirmed.
static volatile int dataToSend = 4; // four endpoints
static volatile bool resend = false;

void onGlobalResponse(zb_cmd_type_t command, esp_zb_zcl_status_t status, uint8_t endpoint, uint16_t cluster) {
  Serial.printf("Global response cmd=%d status=%s ep=%u cl=0x%04x\r\n",
                command, esp_zb_zcl_status_to_name(status), endpoint, cluster);
  if (command == ZB_CMD_REPORT_ATTRIBUTE) {
    if (status == ESP_ZB_ZCL_STATUS_SUCCESS) dataToSend--;
    else if (status == ESP_ZB_ZCL_STATUS_FAIL) resend = true;
  }
}

static void reportAll() {
  // Dummy but plausible values, distinct per endpoint so you can see them in deCONZ.
  analog10.setAnalogInput(10.0f); analog10.reportAnalogInput();
  analog11.setAnalogInput(20.0f); analog11.reportAnalogInput();
  analog12.setAnalogInput(30.0f); analog12.reportAnalogInput();
  analog13.setAnalogInput(40.0f); analog13.reportAnalogInput();
}

// Report, wait for acks (with resend), then deep sleep — mirrors the example's task.
static void measureAndSleep(void *arg) {
  dataToSend = 4;
  reportAll();
  Serial.println("Reported 4 analog values; waiting for confirmation...");

  unsigned long startTime = millis();
  int tries = 0;
  const int maxTries = 3;
  while (dataToSend != 0 && tries < maxTries) {
    if (resend) {
      Serial.println("Resend on failure!");
      resend = false;
      dataToSend = 4;
      reportAll();
    }
    if (millis() - startTime >= REPORT_TIMEOUT) {
      Serial.println("Report timeout! Reporting again");
      dataToSend = 4;
      reportAll();
      startTime = millis();
      tries++;
    }
    Serial.print(".");
    delay(50);
  }
  Serial.println();
  Serial.printf("Confirmation done (remaining=%d). Going to sleep for %ds.\r\n",
                dataToSend, TIME_TO_SLEEP);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(50);

  // Surface the ZBOSS / Zigbee-core log lines so a failed rejoin prints its real reason.
  esp_log_level_set("*", ESP_LOG_INFO);

  Serial.println();
  Serial.printf("=== rejoin_test boot, wake cause = %d ===\r\n", esp_sleep_get_wakeup_cause());

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);

  // Basic identity on the primary endpoint (10), matching the real firmware + DDF.
  analog10.setManufacturerAndModel("DIY", "awning-wind-sensor");
  analog10.setPowerSource(ZB_POWER_SOURCE_BATTERY);

  // Expose an Analog Input on each endpoint and register it.
  analog10.addAnalogInput(); Zigbee.addEndpoint(&analog10);
  analog11.addAnalogInput(); Zigbee.addEndpoint(&analog11);
  analog12.addAnalogInput(); Zigbee.addEndpoint(&analog12);
  analog13.addAnalogInput(); Zigbee.addEndpoint(&analog13);

  Zigbee.onGlobalDefaultResponse(onGlobalResponse);

  // Sleepy ED. Pin to our channel so the rejoin scans ONE channel, not all 16.
  Zigbee.setRxOnWhenIdle(false);
  Zigbee.setPrimaryChannelMask(1UL << ZB_CHANNEL);
  Zigbee.setScanDuration(2);

  // Give begin() a LONG window (90 s vs the 30 s default) so we can distinguish a rejoin
  // that is slow-but-eventually-works from one that is genuinely stuck.
  Zigbee.setTimeout(BEGIN_TIMEOUT);

  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000; // example's value

  // Time begin() so we can see fast-fail (init) vs a long hang (semaphore) — same as the
  // diagnostic we added to the real firmware.
  Serial.println("Calling Zigbee.begin()...");
  uint32_t t0 = millis();
  bool ok = Zigbee.begin(&zigbeeConfig, false /* keep stored network */);
  Serial.printf("Zigbee.begin() = %s after %lums\r\n", ok ? "true" : "false",
                (unsigned long)(millis() - t0));

  if (!ok) {
    // The example's behaviour: reboot and retry. (In the real firmware we DON'T do this
    // because ESP.restart() crashed the H2; here we keep it to match the example exactly
    // and see whether a plain retry ever succeeds.)
    Serial.println("begin() failed — rebooting to retry (example behaviour)");
    Serial.flush();
    ESP.restart();
  }

  Serial.println("Connecting to network...");
  t0 = millis();
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
    if (millis() - t0 > CONNECT_TIMEOUT) { // don't spin forever if connect never completes
      Serial.printf("\nconnect() still false after %lums — sleeping, retry next wake\r\n",
                    (unsigned long)(millis() - t0));
      esp_deep_sleep_start();
    }
  }
  Serial.printf("\nConnected! channel=%u PAN=0x%04x\r\n",
                esp_zb_get_current_channel(), esp_zb_get_pan_id());

  // Report + sleep in a task, exactly like the example.
  xTaskCreate(measureAndSleep, "report_sleep", 4096, NULL, 10, NULL);
}

void loop() {
  // Never used; setup() runs on every wake and ends in deep sleep.
}
