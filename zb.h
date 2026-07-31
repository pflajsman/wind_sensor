// zb.h — Zigbee end-device interface (spec §7).
//
// API verified against arduino-esp32 master (libraries/Zigbee/src/): ZigbeeAnalog
// (Analog Input cluster 0x000C), and the ZigbeeEP methods setManufacturerAndModel /
// setPowerSource / setBatteryPercentage / reportBatteryPercentage, plus the core
// ZigbeeCore::factoryReset(bool restart). Requires a core new enough to ship the
// Zigbee library with ZigbeeAnalog (core >= 3.1). If you pin an older core, confirm
// these still exist in libraries/Zigbee/src/ep/ (§7, §13).

#pragma once

#include <Arduino.h>
#include "adxl345.h"

// Endpoint numbers (spec §7 table).
static constexpr uint8_t EP_MOVEMENT_RMS  = 10; // mg
static constexpr uint8_t EP_MOVEMENT_PEAK = 11; // mg
static constexpr uint8_t EP_TILT_PITCH    = 12; // degrees
static constexpr uint8_t EP_TILT_ROLL     = 13; // degrees

// Bring up Zigbee as a sleepy end device and (re)commission if needed.
// Returns true once connected. Blocks up to timeout_ms for the join.
bool zbBegin(uint32_t timeout_ms);

// Force a fresh commissioning / network-steering attempt (the "pairing" action,
// triggered by the IO14 button). Open the coordinator network first (Phoscon
// permit-join). Returns true once connected. Does NOT erase flash (§8.1).
bool zbForceCommission(uint32_t timeout_ms);

// Push one set of metrics to the four analog endpoints (§7).
void zbReportMetrics(const AccelMetrics &m);

// Push battery state through the standard Power Configuration cluster (§7).
void zbReportBattery(uint8_t soc_percent, float volts);

// True once the network is joined/connected.
bool zbConnected();
