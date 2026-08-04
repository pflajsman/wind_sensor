# Awning Wind Protection Sensor — Firmware

Firmware for a battery-powered Zigbee sensor that detects **awning movement** and
reports numeric movement intensity to Home Assistant, which decides when to retract.

Full design rationale: [`awning-wind-sensor-spec.md`](awning-wind-sensor-spec.md).
**Read spec §8 (pitfalls) before changing any code.**

## Hardware

- LaskaKit ESP32-H2-DEVKit V2 (`ESP32-H2-MINI-1-N4`, PCB antenna)
- GY-291 breakout (Analog Devices ADXL345), I2C mode, `CS` tied HIGH (§8.8)
- Protected 18650 Li-Ion cell via the board's `JST-PH-2` connector

Wiring is in spec §3. The accelerometer's `VCC` goes to the **permanent `3V3`**
pin, never to the `IO12`-switched µSup rail (§3.2).

### Wake & pairing

The device wakes from deep sleep on the accelerometer interrupt (`IO13`) via **EXT1**
(the ESP32-H2's deep-sleep GPIO mechanism, active-high) or on the heartbeat timer.
There is no pairing button — pairing happens on **cold boot**: power-cycle the device
with the coordinator network open (Phoscon *Permit join* / deCONZ *Permit Join*) and it
commissions itself. See `integration/deconz/README.md`.

## Files

| File | Purpose |
|---|---|
| `wind_sensor.ino` | Wake-reason-driven state machine (COLD_BOOT / WIND_EVENT / HEARTBEAT) |
| `config.h` / `config.cpp` | Pin map + all NVS-backed tunables (spec §9) in one place |
| `adxl345.h` / `adxl345.cpp` | Hand-rolled register-level ADXL345 driver (§5, §6) |
| `power.h` / `power.cpp` | Battery divider read, SoC lookup, low-voltage lockout (§7) |
| `zb.h` / `zb.cpp` | Zigbee end device, four analog endpoints + battery cluster (§7) |
| `integration/deconz/awning_wind_sensor.json` | **deCONZ DDF** (ConBee II + deCONZ 2.33.2) — current host path |
| `integration/deconz/README.md` | DDF install + verification steps |
| `integration/awning_wind_sensor.js` | Zigbee2MQTT external converter (§11) — *alternate* host path |
| `integration/home_assistant.yaml` | HA helpers + the five required automations (§11) |

> **Coordinator:** ConBee II running **deCONZ** (a single ConBee II can't run deCONZ
> and Z2M simultaneously). Use `integration/deconz/`. The Z2M converter is kept only
> for a Z2M-based rebuild. The firmware is identical for both.

## Build (Arduino IDE)

1. Board: **ESP32-H2 Dev Module** (or the LaskaKit definition), arduino-esp32 core **≥ 3.1.0**.
2. **Tools → Zigbee mode → "Zigbee ED (end device)"**. The sketch `#error`s otherwise.
3. Partition scheme: one **with Zigbee support**.
4. Plain **Upload**. Never `erase_flash` — it wipes the IEEE address and orphans the
   pairing, leaving ghost devices in Z2M (§8.1).

For bench debugging, uncomment `#define DEBUG_SERIAL` in `config.h`. Leave it off for
battery builds — serial is a power leak and adds boot delay (§13).

## Must-verify-on-hardware TODOs

These are marked `TODO(...)` in the source because they cannot be derived, only
measured or checked against the installed toolchain:

- **ADXL345 register scaling** (`adxl345.h`): confirm `3.9 mg/LSB` and
  `62.5 mg/LSB`, plus every register address/bit, against the ADXL345 datasheet
  Rev. G (§5.3). Register *addresses* were cross-checked and match Rev. G.
- **`vbat_ratio`** (`config.h`): the on-board divider ratio is a placeholder of
  `2.0`. Calibrate it per §7 / acceptance test 4 — do not assume 1:2.
- **Zigbee endpoint API** (`zb.cpp`): `ZigbeeAnalog` method names drift between
  core versions. Verify against `libraries/Zigbee/src/ep/` in your installed core
  (§7, §13). *(No ESP32 core was installed on the build machine when this was
  written, so this could not be introspected here.)*
- **Z2M model/manufacturer** (`integration/awning_wind_sensor.js`): fill from the
  joined device's "About" tab.

## Acceptance tests

Work through spec §12 in order. The one people skip is **step 7**: log
`movement_rms` for a week on the real awning and pick the threshold from the data
rather than guessing.
