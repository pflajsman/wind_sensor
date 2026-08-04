# Awning Wind Sensor — ESP-IDF port (native esp-zigbee-sdk)

Native ESP-IDF firmware for the ESP32-H2 deep-sleep Zigbee end device. This replaces the
arduino-esp32 build, whose post-deep-sleep rejoin was unreliable (`Zigbee.begin()` hung
~30 s, stack never restored the saved network). On native IDF we drive the commissioning
signals and the sleep path directly — the vendor-supported way to do deep sleep + rejoin.

> **Status: FIRST-PASS PORT WRITTEN — ready for a trial build.** `main/awning_wind_sensor.c`
> is written against the **2.x `ezb_*` API** (esp-zigbee-lib ≥ 2.0.0, what IDF 6.0.2 pulls),
> modeled on the vendor `deep_sleep_end_device` example, with our four Analog Input endpoints
> (10–13), Basic (DIY / awning-wind-sensor) + Identify on the primary, and deep-sleep
> measure→report→sleep. Every API used was checked against the local 2.0.3 headers. It has
> **not been compiled yet** — expect to fix a few names on the first `idf.py build`. The
> ADXL345 driver (`adxl345.c`) is IDF-native.
>
> **Go/no-go already PASSED:** the unmodified vendor deep-sleep example rejoined after sleep
> with the network closed on this hardware (`Device rebooted` → re-sleep, no re-pair). Deep
> sleep is entered with a one-shot `esp_timer` + `esp_deep_sleep_start()`; the rejoin happens
> in the `EZB_BDB_SIGNAL_DEVICE_REBOOT` handler. No `esp_zb_sleep_enable()` (that's light
> sleep). Channel pinned via `ezb_bdb_set_primary_channel_set(1<<15)`.
>
> **Deferred TODOs** (marked in the .c): battery report + low-voltage lockout, WIND_EVENT
> active loop on accel wake, NVS runtime config, first-join bind-settle window.

## Prerequisites

- **ESP-IDF v5.1+** with the **VS Code Espressif IDF extension** (Express install, ESP-IDF
  v5.3 recommended). Target: **esp32h2**.
- The `esp-zigbee-lib` / `esp-zboss-lib` managed components download automatically on the
  first build (see `main/idf_component.yml`).

## Build & flash

```
idf.py set-target esp32h2
idf.py build
idf.py -p <PORT> flash monitor
```

(In VS Code: set target esp32h2 via the extension, then Build / Flash / Monitor buttons.)

Never do a full flash-erase once joined — the ZBOSS dataset + IEEE state live in the
`nvs` / `zb_storage` partitions; erasing orphans the pairing in deCONZ.

## Verify against the vendor example FIRST (the go/no-go gate)

You have **ESP-IDF v6.0.2** installed at `C:\esp\v6.0.2\esp-idf`. Before investing in the
port, prove native IDF actually fixes rejoin on YOUR hardware, using the vendor example
UNMODIFIED. The example lives in the esp-zigbee-sdk repo (NOT inside IDF); the component
manager downloads it, or you clone it:

```
git clone -b main https://github.com/espressif/esp-zigbee-sdk.git
cd esp-zigbee-sdk/examples/sleepy_devices/deep_sleep_end_device
idf.py set-target esp32h2
# set the primary channel to 15 to match deCONZ: menuconfig -> Zigbee Example ->
#   Zigbee Primary Channel = 15   (CONFIG_ZB_EXAMPLE_PRIMARY_CHANNEL)
idf.py menuconfig
idf.py build
idf.py -p <PORT> flash monitor
```

(It's an on/off switch, not our sensor — that's fine. We're testing the SLEEP+REJOIN
mechanism only.)

1. Pair it to deCONZ (Permit Join open).
2. **Close Permit Join.** Let it deep-sleep (it sleeps ~20 s in the example) and wake.
3. Watch the monitor: on the wake you want to see `Device rebooted` → it goes straight back
   to sleep, i.e. it was **already connected** — no re-steering, no 30 s hang.
4. **Result:**
   - Rejoins reliably with the network closed → IDF is the right path. Then I rewrite
     `main/awning_wind_sensor.c` against the 2.x `ezb_*` API (using the vendor file as the
     template) with our 4 analog endpoints, and we finish the port.
   - Even the vendor example fails → the problem is deeper than the Arduino wrapper
     (hardware / deCONZ / channel), and we rethink rather than port.

## What's done vs TODO

Done (skeleton):
- Project scaffold: CMake, `sdkconfig.defaults`, `partitions.csv`, component manifest.
- Four Analog Input endpoints (10–13) + Basic/Identify/PowerConfig on the primary, matching
  the deCONZ DDF (`DIY` / `awning-wind-sensor`).
- ADXL345 driver ported 1:1 from the Arduino build to IDF `i2c_master` (`adxl345.c`).
- Deep-sleep entry with timer heartbeat + EXT1 accel-INT1 wake.
- Channel pinned to 15 (`esp_zb_set_primary_network_channel_set`).

TODO (port from the Arduino firmware):
- Battery read + SoC + low-voltage lockout (`power.cpp` → IDF ADC oneshot).
- NVS-backed runtime config (`config.cpp`).
- WIND_EVENT active loop (stay awake across gusts) + quiet-window exit.
- First-join settle window for deCONZ Bind + Configure-Reporting (`zbSettleForBindings`).
- Report-delivery confirmation before sleep (the ack-count logic we added in Arduino).
- A join-timeout watchdog so a failed wake still returns to deep sleep.

## Why the channel is pinned (and the one gotcha)

`ZB_CHANNEL` in `main/awning_config.h` is **15** (this deCONZ network). Pinning makes the
post-sleep rejoin scan one channel, fast. **If deCONZ moves to another channel, change
`ZB_CHANNEL` and reflash**, or the device never finds its parent.
