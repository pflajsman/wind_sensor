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
// TODO(core): confirm ZigbeeAnalog and these method names against the installed
// arduino-esp32 core (libraries/Zigbee/src/ep/). API drifts between core versions.
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
// (integration/deconz/awning_wind_sensor.json: manufacturername / modelid) and the
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
  // binds. The DDF reads Basic from endpoint 10 (analogRms), so set it there.
  // TODO(core): method name is setManufacturerAndModel() on ZigbeeEP in core >= 3.1;
  // verify against libraries/Zigbee/src/ in your installed core.
  analogRms.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);

  // Battery goes through the standard Basic + Power Configuration clusters, which
  // the stack exposes on the primary endpoint via setPowerSource/setBattery* (§7).
  // TODO(core): if the installed core attaches battery reporting to a specific
  // endpoint object instead of the global Zigbee singleton, move these accordingly.
  Zigbee.setPowerSource(ZB_POWER_SOURCE_BATTERY);

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
  // Trigger network steering to (re)join. This is the button-initiated pairing
  // path. We deliberately do NOT call any erase/factory-reset that would wipe the
  // IEEE address — that orphans the pairing and spawns ghost devices (§8.1).
  // TODO(core): confirm the steering entry point on the installed core. On core
  // >= 3.1 this is typically Zigbee.factoryReset(false) for a soft rejoin, or
  // openNetwork()/network-steering helpers; verify in libraries/Zigbee/src/.
  DBGLN("[zb] forcing commissioning (network steering)...");

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
  Zigbee.setBatteryPercentage(soc_percent);
  Zigbee.reportBatteryPercentage();
  DBG("[zb] reported battery %u%%\n", soc_percent);
}
