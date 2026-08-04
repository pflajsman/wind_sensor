#include <Zigbee.h>
#include <ZigbeeCore.h>
#include <ZigbeeEP.h>
#include <ZigbeeTypes.h>
// Direct esp-zigbee-sdk API: esp_zb_cfg_t / ESP_ZB_ED_AGING_TIMEOUT_* (custom ED aging
// config for begin()) and esp_zb_get_long_address (IEEE readback). The Arduino Zigbee
// headers pull these in transitively, but include the SDK header explicitly so the
// symbols do not depend on wrapper include order.
#include "esp_zigbee_core.h"

// zb.cpp — Zigbee end-device implementation (spec §7).
//
// Guarded so the sketch refuses to compile in the wrong Zigbee mode (§7): the
// Arduino IDE must be set to "Zigbee ED (end device)" with a Zigbee-capable
// partition scheme.
#ifndef ZIGBEE_MODE_ED
  #error "Set Tools -> Zigbee mode -> 'Zigbee ED (end device)' and a Zigbee partition scheme (spec §7)."
#endif

#include "zb.h"
#include "config.h"
#include "Zigbee.h"
#include <Preferences.h> // one-shot zb_storage recovery flag (separate `nvs` partition)
#include <atomic>        // s_pending_reports is touched by the Zigbee task + main loop

// One ZigbeeAnalog endpoint per reported number (§7). Analog Input cluster 0x000C.
// API verified against arduino-esp32 master (libraries/Zigbee/src/ep/ZigbeeAnalog.h):
// ctor(uint8_t endpoint), addAnalogInput(), setAnalogInput(float), reportAnalogInput().
static ZigbeeAnalog analogRms  (EP_MOVEMENT_RMS);
static ZigbeeAnalog analogPeak (EP_MOVEMENT_PEAK);
static ZigbeeAnalog analogPitch(EP_TILT_PITCH);
static ZigbeeAnalog analogRoll (EP_TILT_ROLL);

static void addAnalog(ZigbeeAnalog &ep) {
  // Expose an Analog Input so the value is reportable/readable by the coordinator.
  ep.addAnalogInput();
  Zigbee.addEndpoint(&ep);
}

// ---------------------------------------------------------------------------
// Report-confirmation tracking (the key to reliable rejoin after deep sleep).
//
// A sleepy ED that fires-and-forgets its reports has no way to know the parent
// actually received them. If the parent misses our traffic, its neighbor-table
// aging timer is never reset and it EVICTS us mid-sleep — so the next wake can no
// longer silently *rejoin* and must do a full network *steering* (which needs an
// open coordinator network), i.e. the "join TIMEOUT 40 s with Permit Join closed"
// symptom. The official arduino-esp32 sleepy example (Zigbee_Temp_Hum_Sensor_Sleepy)
// avoids this by waiting for the coordinator's ZCL Default Response to each report
// and only sleeping once the reports are confirmed delivered. We do the same.
//
// The stack delivers that confirmation on the Zigbee task via the global
// default-response callback (Zigbee.onGlobalDefaultResponse). We just count acks.
// atomic, not bare volatile: the callback runs on the Zigbee FreeRTOS task while the
// report loop polls it on the main task, and a plain volatile-- is neither atomic nor
// allowed under C++20 (-Wvolatile). std::atomic gives both a safe RMW and no warning.
static std::atomic<uint8_t> s_pending_reports{0}; // reports awaiting a coordinator ack

static void onGlobalDefaultResponse(zb_cmd_type_t command, esp_zb_zcl_status_t status,
                                    uint8_t endpoint, uint16_t cluster) {
  // We only care about acks for our attribute reports. Any successful default
  // response to a REPORT_ATTRIBUTE means the parent received that report.
  if (command == ZB_CMD_REPORT_ATTRIBUTE && status == ESP_ZB_ZCL_STATUS_SUCCESS) {
    // Decrement only if >0, without underflowing, in a CAS loop (acks could in principle
    // arrive concurrently). load/compare/exchange keeps it race-free.
    uint8_t cur = s_pending_reports.load();
    while (cur > 0 && !s_pending_reports.compare_exchange_weak(cur, cur - 1)) { /* retry */ }
    DBG("[zb] report ack ep=%u cl=0x%04x (%u pending)\n", endpoint, cluster,
        s_pending_reports.load());
  }
}

// Basic-cluster identifiers advertised on join. These MUST match the deCONZ DDF
// (integration/deconz/awning_wind_sensor.ddf: manufacturername / modelid) and the
// Z2M converter, or the host-side mapping will not bind to the device. Without an
// explicit call the arduino-esp32 stack advertises a generic/blank model, which
// matches nothing — so set it deterministically here.
static constexpr char ZB_MANUFACTURER[] = "DIY";
static constexpr char ZB_MODEL[]        = "awning-wind-sensor";

bool zbBegin(uint32_t timeout_ms) {
  // Radio-stabilisation settle after a deep-sleep wake. Coming out of deep sleep the
  // ESP32-H2's 802.15.4 radio needs a moment to come up before we start the stack.
  // (200 ms alone did NOT fix the post-sleep begin() failure — the fix is the rejoin-speed
  // tuning below plus retry-next-wake — but a short settle is still cheap insurance.)
  delay(300);

  // Sleepy battery end device (§7).
  Zigbee.setRxOnWhenIdle(false);

  // Rejoin-speed tuning, matching a known-working arduino-esp32 deep-sleep ED
  // (discussion #12247, DFR1117 door sensor). A provisioned device that lost its parent
  // must re-scan to find it; the DEFAULTS make that scan slow enough that begin()'s
  // 30 s start-semaphore (or our connect poll) can fire first and report a false failure.
  //   - setScanDuration(2): shorter per-channel dwell -> faster rejoin scan.
  //   - The wrapper already narrows the scan to the LAST-joined channel via the NVRAM
  //     channel mask it saved on the previous successful join (setNVRAMChannelMask), so a
  //     healthy rejoin only scans one channel and is fast; the short dwell compounds that.
  // setScanDuration takes 1..4 (1 fastest). 2 is the working example's value.
  Zigbee.setScanDuration(2);

  // Pin the scan to the coordinator's single channel (config.h ZB_CHANNEL). This is the
  // Espressif-recommended fix for deep-sleep rejoin (esp-zigbee-sdk H2 FAQ): a device that
  // lost its parent across sleep otherwise scans all 16 channels to find it, which is slow
  // and races the join timeout. Scanning ONE known channel makes the rejoin fast and
  // reliable. Must be set before begin() (zigbeeInit reads it into the primary channel set).
  // The wrapper also saves the joined channel to NVRAM on success, but pinning it here means
  // even the first post-sleep wake — and any wake after a channel-mask reset — scans only ch15.
  Zigbee.setPrimaryChannelMask(ZB_CHANNEL_MASK);

  // begin()'s internal start semaphore is given on the DEVICE_REBOOT signal (stack
  // STARTED), which fires before the NWK rejoin completes — so this timeout only needs to
  // cover stack startup, not the join (our connect-poll below covers the join). Keep the
  // 30 s default; do NOT shorten it, or a slow post-sleep startup makes begin() give up
  // and (per the wrapper) reset the channel mask to all-channels, making the NEXT rejoin
  // even slower. Left unset intentionally.

  // Watch for coordinator acks to our reports so the caller can confirm delivery
  // before sleeping (keeps the parent relationship alive -> fast silent rejoin).
  Zigbee.onGlobalDefaultResponse(onGlobalDefaultResponse);

  addAnalog(analogRms);
  addAnalog(analogPeak);
  addAnalog(analogPitch);
  addAnalog(analogRoll);

  // Advertise a stable manufacturer/model on the Basic cluster (0x0000) so the DDF
  // binds. setManufacturerAndModel() is a ZigbeeEP method (inherited by ZigbeeAnalog);
  // the DDF reads Basic from endpoint 10 (analogRms), so set it there.
  analogRms.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);

  // Battery reporting is per-endpoint: setPowerSource / setBatteryPercentage /
  // reportBatteryPercentage are ZigbeeEP methods, NOT on the Zigbee core singleton
  // (verified against libraries/Zigbee/src/ZigbeeEP.h). Attach them to the same
  // primary endpoint the DDF reads Basic from (analogRms).
  analogRms.setPowerSource(ZB_POWER_SOURCE_BATTERY);

  // End-device aging timeout MUST outlast our deep-sleep interval, or the parent
  // (ConBee/deCONZ) evicts us from its neighbor table while we sleep. Once evicted,
  // the wake can no longer do a silent NWK *rejoin* (which needs no permit-join) and
  // falls back to a full network *steering* (association) — which DOES require an
  // open network, so a closed-network wake times out (observed: join TIMEOUT 40 s).
  //
  // The parent's aging timer is reset by any Data/Timeout Request the child sends, and
  // we transmit our sensor report on every wake — so ed_timeout just has to cover one
  // worst-case sleep-plus-margin gap. hb_interval is 30 min; 1024 min (~17 h) gives a
  // huge margin for jitter, a missed wake, or a doubled interval.
  //
  // keep_alive governs how often the ED polls its parent WHILE AWAKE. It is NOT
  // irrelevant to a deep-sleeper: every wake we come up, poll, report, and sleep, and
  // that awake-window poll is what refreshes the parent's view of us. The official
  // sleepy example raises it from the 3000 ms default to 10000 ms "to avoid interference
  // with reporting data" (a too-frequent poll competes with our report/confirm traffic),
  // so we match that here.
  //
  // The arduino-esp32 ZigbeeCore wrapper exposes NO setter for ed_timeout, so we pass a
  // full esp_zb_cfg_t to the begin(esp_zb_cfg_t*, bool) overload. Verified against core
  // 3.3.11: begin() forwards this pointer straight into esp_zb_init() with the nested
  // zed_cfg intact. erase_nvs=false so the stored network survives (enables the rejoin).
  esp_zb_cfg_t zbCfg = {
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
    .install_code_policy = false,
    .nwk_cfg = {
      .zed_cfg = {
        .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_1024MIN, // ~17 h >> 30 min sleep
        .keep_alive = 60000,                           // ms awake-poll; matches working deep-sleep ED (#12247)
      },
    },
  };
  // begin() can return false on a warm (post-deep-sleep) wake for two very different
  // reasons, and the WRONG recovery for each was breaking silent rejoin:
  //
  //   (a) transient slow start — the stack just needs more time / another wake. This is
  //       the common case for a healthy provisioned device. The stored network is fine.
  //   (b) genuinely corrupt zb_storage — a crashed esp_zb_factory_reset (arduino-esp32
  //       #10644) left the ZBOSS dataset half-written; begin() then fails on EVERY boot.
  //
  // The OLD recovery called ESP.restart() on any failure. On the ESP32-H2 that restart,
  // fired while the Zigbee task is mid-commissioning-retry, itself CRASHES (SW_CPU /
  // Saved PC — arduino-esp32 #11554). The crash reset is not a sleep wake, so the next
  // boot sees wake-cause "none" and runs COLD_BOOT, which steers fresh and needs Permit
  // Join open — i.e. the device drops out of pairing-free operation entirely.
  //
  // New policy: NEVER restart on a begin() failure. For case (a) we just return false;
  // the caller skips this wake's report and deep-sleeps, and the NEXT wake retries the
  // rejoin with the stored network intact — no pairing mode needed. Only after
  // ZB_RECOVER_AFTER_FAILS consecutive failures do we spend ONE in-place erase_nvs=true
  // begin() as a last resort for case (b). A successful begin() clears the counter, so a
  // one-off slow start never escalates to an erase. erase_nvs=true wipes ONLY the
  // zb_storage ZBOSS dataset; the ESP32-H2 EUI-64 is in eFuse and is untouched.
  static constexpr uint8_t ZB_RECOVER_AFTER_FAILS = 3;

  Preferences prefs;
  prefs.begin("zbrec", false);
  uint8_t fails = prefs.getUChar("failcnt", 0);
  bool recover = (fails >= ZB_RECOVER_AFTER_FAILS);
  prefs.end();
  if (recover) {
    DBGLN("[zb] RECOVERY: erasing zb_storage (erase_nvs=true) after repeated fails");
  }

  // Time the begin() call. This tells us WHY it fails when it does: a fast fail (<1 s)
  // means zigbeeInit() itself bailed (task/channel/register error); a slow fail (~30 s)
  // means the start semaphore timed out (the DEVICE_REBOOT signal never fired — the stack
  // didn't come up after deep sleep). Different failures need different fixes.
  uint32_t begin_t0 = millis();
  bool begin_ok = Zigbee.begin(&zbCfg, recover /* erase_nvs */);
  DBG("[zb] begin() returned %s after %lums\n", begin_ok ? "true" : "false",
      (unsigned long)(millis() - begin_t0));

  if (!begin_ok) {
    DBGLN("[zb] Zigbee.begin failed");
    prefs.begin("zbrec", false);
    // If this was the erase-recovery attempt, reset the counter (we spent the recovery);
    // otherwise bump it toward the erase threshold.
    uint8_t n = recover ? 0 : (uint8_t)(fails + 1);
    prefs.putUChar("failcnt", n);
    prefs.end();
    DBG("[zb] begin fail (count %u) — sleeping, will retry rejoin next wake (erase after %u)\n",
        n, ZB_RECOVER_AFTER_FAILS);
    return false; // caller skips this wake's report and deep-sleeps; network stays intact
  }

  // begin() succeeded — clear the failure counter so a future transient starts fresh.
  prefs.begin("zbrec", false);
  if (prefs.getUChar("failcnt", 0) != 0) prefs.putUChar("failcnt", 0);
  prefs.end();

  // Confirm the IEEE (eFuse-derived; must be non-zero) survived any recovery erase.
  esp_zb_ieee_addr_t ieee;
  esp_zb_get_long_address(ieee);
  DBG("[zb] IEEE %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
      ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);

  // begin() returning true only means the stack STARTED. For a provisioned device the
  // stack now does a background NWK rejoin (no permit-join needed); connected() flips
  // true when it completes. Give it a realistic window — an all-channel rejoin scan can
  // take well over 10 s. On timeout we do NOT erase or re-steer: just return false and
  // let the caller sleep and try again on the next wake (the stored network is intact).
  DBGLN("[zb] joining...");
  uint32_t t0 = millis();
  while (!Zigbee.connected() && (millis() - t0) < timeout_ms) {
    delay(100);
  }
  bool ok = Zigbee.connected();
  if (ok) {
    // Log the channel + PAN ID we actually joined on, so a wrong ZB_CHANNEL is obvious.
    // Expect channel 15, PAN ID 0x4AD4 for this deCONZ network.
    DBG("[zb] on channel %u, PAN 0x%04x\n", esp_zb_get_current_channel(), esp_zb_get_pan_id());
  }
  DBG("[zb] %s after %lums\n", ok ? "connected" : "join TIMEOUT", (unsigned long)(millis() - t0));
  return ok;
}

bool zbConnected() { return Zigbee.connected(); }

// Push the four analog values and (best-effort) wait for the coordinator to
// acknowledge each one, retrying unacked reports. Getting these reports confirmed
// is what keeps the parent from aging us out during the next deep sleep, which is
// what makes the next wake a fast silent rejoin instead of a 40 s steering timeout.
// Mirrors the confirm-and-retry loop in the official arduino-esp32 sleepy example.
static void reportMetricsOnce(const AccelMetrics &m) {
  // setAnalogInput + report per endpoint. Units are documented in the Z2M
  // external converter (§11), not carried on the wire here.
  analogRms.setAnalogInput(m.movement_rms);   analogRms.reportAnalogInput();
  analogPeak.setAnalogInput(m.movement_peak); analogPeak.reportAnalogInput();
  analogPitch.setAnalogInput(m.tilt_pitch);   analogPitch.reportAnalogInput();
  analogRoll.setAnalogInput(m.tilt_roll);     analogRoll.reportAnalogInput();
}

void zbReportMetrics(const AccelMetrics &m) {
  if (!Zigbee.connected()) {
    DBGLN("[zb] report skipped — not connected");
    return;
  }

  static constexpr uint8_t  REPORTS_PER_CYCLE = 4;    // four analog endpoints
  static constexpr uint32_t REPORT_ACK_MS     = 1000; // per-attempt ack window
  static constexpr uint8_t  MAX_ATTEMPTS      = 3;    // resend the whole set on miss

  // Expect one ack per endpoint. The global default-response callback decrements
  // s_pending_reports on each successful REPORT_ATTRIBUTE ack (runs on the Zigbee task).
  s_pending_reports = REPORTS_PER_CYCLE;
  reportMetricsOnce(m);
  DBG("[zb] reported rms=%.1f peak=%.1f pitch=%.1f roll=%.1f (awaiting %u acks)\n",
      m.movement_rms, m.movement_peak, m.tilt_pitch, m.tilt_roll, REPORTS_PER_CYCLE);

  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
    uint32_t t0 = millis();
    while (s_pending_reports > 0 && (millis() - t0) < REPORT_ACK_MS) {
      delay(50); // yields to the Zigbee task so acks can arrive and be counted
    }
    if (s_pending_reports == 0) {
      DBG("[zb] all reports confirmed (attempt %u)\n", attempt + 1);
      return;
    }
    // Some (or all) reports were not acked. Resend the full set and wait again — but
    // only if another wait iteration follows, so we don't transmit-then-immediately-exit.
    // Not every coordinator default-responds to every report; if acks never come this
    // still bounds the effort to MAX_ATTEMPTS, having transmitted on each one.
    if (attempt + 1 < MAX_ATTEMPTS) {
      DBG("[zb] %u report(s) unconfirmed — resending (attempt %u/%u)\n",
          s_pending_reports.load(), attempt + 2, MAX_ATTEMPTS);
      s_pending_reports = REPORTS_PER_CYCLE;
      reportMetricsOnce(m);
    }
  }
  DBGLN("[zb] reports still unconfirmed after retries — proceeding anyway");
}

void zbSettleForBindings(const AccelMetrics &m, uint32_t ms) {
  // Only meaningful if we actually joined — nothing to bind otherwise.
  if (!Zigbee.connected()) {
    DBGLN("[zb] settle skipped — not connected");
    return;
  }

  // Keep the device awake for `ms` so deCONZ can deliver its queued Bind +
  // Configure-Reporting requests. The Zigbee stack polls the parent on its own task
  // (keep_alive, set to 10 s in zbBegin), so we just avoid deep sleep and re-report
  // every few seconds to give the coordinator live attribute traffic to configure
  // against. delay() yields to the scheduler, so the stack task keeps running throughout.
  DBG("[zb] settling for bindings, staying awake %lums\n", (unsigned long)ms);
  uint32_t t0 = millis();
  uint32_t last_report = 0;
  while ((millis() - t0) < ms) {
    if ((millis() - last_report) >= 5000) {
      zbReportMetrics(m);          // periodic traffic during the bind window
      last_report = millis();
    }
    delay(200);                    // yields to the Zigbee_main FreeRTOS task
  }
  DBGLN("[zb] settle window done");
}

void zbReportBattery(uint8_t soc_percent, float volts) {
  (void)volts; // voltage is carried by the stack's own battery voltage attribute
  // Per-endpoint battery methods (ZigbeeEP), on the same endpoint as setPowerSource.
  analogRms.setBatteryPercentage(soc_percent);
  analogRms.reportBatteryPercentage();
  DBG("[zb] reported battery %u%%\n", soc_percent);
}
