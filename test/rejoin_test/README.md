# rejoin_test — minimal deep-sleep rejoin probe

A stripped-down sketch to isolate the one failing behaviour in the main firmware:
**does the ESP32-H2 Zigbee ED rejoin the deCONZ network after a deep-sleep wake, or does
`Zigbee.begin()` hang ~30 s?**

It is as close as possible to the official arduino-esp32 example
`libraries/Zigbee/examples/Zigbee_Temp_Hum_Sensor_Sleepy`, changed only where our app
differs: four `ZigbeeAnalog` endpoints (10–13), manufacturer `DIY` / model
`awning-wind-sensor`, and pinned to **channel 15**. No accelerometer, battery, NVS config,
recovery logic, or button — so nothing from the real firmware can be blamed.

## Build & flash

1. Board: **ESP32-H2 Dev Module**
2. **Tools → Zigbee mode → "Zigbee ED (end device)"**
3. Partition scheme: one **with Zigbee** support
4. **Tools → Core Debug Level → "Info"**  ← important: prints the ZBOSS/core rejoin lines
5. Plain **Upload** (never *Erase All Flash* — it wipes the join)

> Flashing this is a *separate* Zigbee device from the main firmware only if the IEEE
> changed — it won't, the EUI-64 is in eFuse. So deCONZ sees the same node. That's fine
> for the test.

## Procedure

1. Open deCONZ **Permit Join**.
2. Flash + open Serial Monitor at 115200. Watch for:
   `Zigbee.begin() = true after …ms` → `Connected! channel=15 PAN=0x4ad4`.
3. Close Permit Join.
4. Let it run. It sleeps 60 s, wakes, and tries to rejoin **with the network closed** —
   exactly the field scenario.

## Reading the result

This build gives `begin()` a **90 s** window (`BEGIN_TIMEOUT`) instead of the 30 s default,
specifically to answer *"would it work if it just had more time?"* — is the rejoin **slow**
or **stuck**?

- **`Zigbee.begin() = true` then `Connected!` — at any time (even 45–60 s)** → deep-sleep
  rejoin WORKS; it was only slow. A longer `setTimeout()` in the real firmware is then a
  valid (if not pretty) fix, and we also port this simple shape back.
- **`Zigbee.begin() = false after ~90000ms`** → it is **stuck**, not slow. More time does
  not help; deep-sleep rejoin is not viable on this library for our setup. Switch strategy
  (light sleep, or ESP-IDF + esp-zigbee-sdk).
- **`begin() = true` but `connect() still false after 90s`** → the stack starts but never
  finds/joins the parent — points at RF/channel/parent availability rather than the stack
  init. The ZBOSS lines (Core Debug Level = Info) will name the reason.

Note the earlier real-firmware log showed `false after 30035ms` — waited the *full* default
and got nothing, which smells like "stuck." This test confirms or refutes that: if 90 s also
returns false, it is stuck.

With Core Debug Level = Info, the lines between `Calling Zigbee.begin()` and the result are
the ZBOSS reason (e.g. `Rejoin failure: no dev with xpanid`, `can't choose best parent`).
Capture the **full** log of one warm wake and share it.
