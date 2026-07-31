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

// Basic-cluster identifiers advertised on join. These MUST match the deCONZ DDF
// (integration/deconz/awning_wind_sensor.ddf: manufacturername / modelid) and the
// Z2M converter, or the host-side mapping will not bind to the device. Without an
// explicit call the arduino-esp32 stack advertises a generic/blank model, which
// matches nothing — so set it deterministically here.
static constexpr char ZB_MANUFACTURER[] = "DIY";
static constexpr char ZB_MODEL[]        = "awning-wind-sensor";

bool zbBegin(uint32_t timeout_ms) {
  // Sleepy battery end device (§7).
  Zigbee.setRxOnWhenIdle(false);

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

  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    DBGLN("[zb] Zigbee.begin failed");
    return false;
  }

  DBGLN("[zb] joining...");
  uint32_t t0 = millis();
  while (!Zigbee.connected() && (millis() - t0) < timeout_ms) {
    delay(100);
  }
  bool ok = Zigbee.connected();
  DBG("[zb] %s after %lums\n", ok ? "connected" : "join TIMEOUT", (unsigned long)(millis() - t0));
  return ok;
}

bool zbConnected() { return Zigbee.connected(); }

bool zbForceCommission(uint32_t timeout_ms) {
  // Button-initiated pairing. If we are already connected there is nothing to do —
  // avoid a needless reset (and the device restart it triggers).
  if (Zigbee.connected()) {
    DBGLN("[zb] already connected — no commissioning needed");
    return true;
  }

  // Reset the local Zigbee network state and re-run steering so the device rejoins
  // the (now-open) coordinator network. factoryReset(false) clears the stored
  // network binding WITHOUT rebooting here, so we can wait for the rejoin inline.
  //
  // This is the Zigbee-stack network reset, NOT esp erase_flash: it does not wipe
  // the IEEE address, so it does not orphan the pairing or spawn ghost devices
  // (§8.1). Open the coordinator network first (Phoscon permit-join / deCONZ
  // Permit Join), then press the button.
  //
  // Verified against arduino-esp32 master: ZigbeeCore::factoryReset(bool restart),
  // restart defaults to true; pass false to keep running.
  DBGLN("[zb] forcing commissioning: factoryReset(false) + rejoin (open the network now)");
  Zigbee.factoryReset(false);

  uint32_t t0 = millis();
  while (!Zigbee.connected() && (millis() - t0) < timeout_ms) {
    delay(200);
  }
  bool ok = Zigbee.connected();
  DBG("[zb] commission %s after %lums\n", ok ? "OK" : "TIMEOUT",
      (unsigned long)(millis() - t0));
  return ok;
}

void zbReportMetrics(const AccelMetrics &m) {
  // setAnalogInput + report per endpoint. Units are documented in the Z2M
  // external converter (§11), not carried on the wire here.
  analogRms.setAnalogInput(m.movement_rms);   analogRms.reportAnalogInput();
  analogPeak.setAnalogInput(m.movement_peak); analogPeak.reportAnalogInput();
  analogPitch.setAnalogInput(m.tilt_pitch);   analogPitch.reportAnalogInput();
  analogRoll.setAnalogInput(m.tilt_roll);     analogRoll.reportAnalogInput();
  DBG("[zb] reported rms=%.1f peak=%.1f pitch=%.1f roll=%.1f\n",
      m.movement_rms, m.movement_peak, m.tilt_pitch, m.tilt_roll);
}

void zbReportBattery(uint8_t soc_percent, float volts) {
  (void)volts; // voltage is carried by the stack's own battery voltage attribute
  // Per-endpoint battery methods (ZigbeeEP), on the same endpoint as setPowerSource.
  analogRms.setBatteryPercentage(soc_percent);
  analogRms.reportBatteryPercentage();
  DBG("[zb] reported battery %u%%\n", soc_percent);
}
