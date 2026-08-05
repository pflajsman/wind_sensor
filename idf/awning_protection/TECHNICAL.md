# awning_protection — Technical details

Design notes and hard-won gotchas for the ESP32-H2 awning movement sensor. For usage
(sensitivity table, mounting, build) see [`README.md`](README.md).

---

## 1. Hardware

- **MCU board:** LaskaKit ESP32-H2-DEVKit V2 (`ESP32-H2-MINI-1-N4`, PCB trace antenna).
- **Accelerometer:** GY-291 breakout (Analog Devices **ADXL345**), I²C mode, `CS` tied HIGH.
- **Power:** 18650 Li-Ion via the board's JST-PH-2; battery sensed through the on-board divider.

Pin map (`main/awning_config.h`):

| Signal | GPIO | Notes |
|---|---|---|
| I²C SDA | 11 | ADXL345 |
| I²C SCL | 10 | ADXL345 |
| ADXL INT1 | 13 | active-high; light-sleep **EXT1** wake (GPIO13 is RTC-capable, RTCIO ch6) |
| VBAT ADC | 3 | LaskaKit V2 IO3 divider, ratio 1.769 |

---

## 2. Sleep model — Zigbee light sleep (not deep sleep)

This is the central design decision. **The firmware uses Zigbee light sleep, driven entirely
by IDF power management**, not deep sleep.

- `esp_pm_configure({max=min=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, light_sleep_enable=true})` in
  `app_main` (gated on `CONFIG_PM_ENABLE`). With tickless idle, the CPU light-sleeps whenever
  all tasks are blocked; the Zigbee stack parks and wakes the radio around its own poll timer.
- **There is NO `esp_zb_sleep_now()` / `CAN_SLEEP` application hook** in the 2.x `ezbee` SDK —
  those are 1.x-only, and the compat macros are empty stubs. Do not look for them. Light sleep
  is purely `esp_pm` + the sdkconfig flags below.

### Why light sleep instead of deep sleep

Deep sleep sends **no keepalive**, so the coordinator ages the sleepy ED out of its neighbour
table; every wake then pays a full network rejoin, with a race that drops the first reports.
In light sleep the **ZED long-poll (`keep_alive` = 4000 ms) keeps running**, so the parent
link never drops → reports land instantly, no rejoin. Cost: idle current ~1–2 mA instead of
µA (weeks on an 18650, which is the accepted budget here). Espressif's own guidance is that
deep sleep with Zigbee is only worth it for report intervals > ~30 min.

### sdkconfig (from the vendor `light_sleep_end_device` example)

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y
CONFIG_IEEE802154_SLEEP_ENABLE=y
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_SLEEP_POWER_DOWN_FLASH=y
```

The authoritative template for this SDK is
`esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device`.

---

## 3. Motion wake — EXT1, not GPIO wakeup (important)

`CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y` powers the GPIO peripheral down during
sleep. Therefore:

- **`esp_sleep_enable_gpio_wakeup()` is a silent no-op on the ESP32-H2** under this config —
  it logs a warning and returns `ESP_OK`, but INT1 never wakes the CPU. Using it will make
  motion detection appear completely dead.
- The correct wake source is **EXT1** (always-on RTC domain):
  `esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_INT1, ESP_EXT1_WAKEUP_ANY_HIGH)`. GPIO13 is
  RTC-capable, so it is a legal EXT1 pin.
- `esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown()` **does not exist on the H2**
  (`SOC_GPIO_SUPPORT_HP_PERIPH_PD_SLEEP_WAKEUP` is undefined). Don't reach for it.

A normal GPIO edge ISR (`GPIO_INTR_POSEDGE`) is *also* installed, but only as a nudge while
already awake — see the next section for why it isn't relied upon.

---

## 4. Occupancy — the motion-latch model

Occupancy is decided by the **ADXL345 ACTIVITY interrupt**, not by re-measuring an RMS window.

- ADXL config (`main/adxl345.h`): `ACT_INACT_CTL = 0xF0` (AC-coupled activity on X/Y/Z),
  `INT_MAP = 0x00` (activity → INT1), `INT_ENABLE = 0x10`, `DATA_FORMAT = 0x08`
  (FULL_RES, ±2 g, active-high INT). Sensitivity is `THRESH_ACT` (62.5 mg/LSB).
- The report task **polls the ADXL activity latch every `MOTION_POLL_S` (1 s)** via
  `adxl345_activity_fired()` (reads `INT_SOURCE`, which also deasserts + re-arms INT1). On
  activity → occupancy latches **OCCUPIED** and reports immediately. After
  `OCCUPANCY_CLEAR_S` (30 s) with no further activity → latches **UNOCCUPIED** and reports.
- A **heartbeat** (`HB_INTERVAL_S`, 30 min) re-asserts occupancy + refreshes battery, and logs
  a 4 s movement-RMS window as a diagnostic only.

### Why poll instead of relying on the ISR

Two bugs made motion "not react" during development, both fixed by the poll-based latch:

1. **The old 4 s RMS re-measure averaged brief shakes to nothing.** A shake trips the ADXL's
   instantaneous ~125 mg activity detector easily, but averaged over 4 s falls below the mg
   RMS threshold → reported `occupancy=0`. The ADXL already made the decision; re-measuring
   discarded it.
2. **The GPIO edge ISR is missed across light-sleep GPIO isolation.** EXT1 wakes the silicon
   on INT1-high, but the edge that occurred *during* sleep (peripheral powered down) generates
   no GPIO interrupt on resume. If the task blocked solely waiting for an ISR notification, it
   would wake and immediately re-sleep without reporting. Polling the latch each cycle makes
   the wake path work regardless of whether the ISR fired — the truth is always in
   `INT_SOURCE`.

---

## 5. Zigbee endpoint & reporting

- **One endpoint (0x0a / 10):** Basic (0x0000), Identify (0x0003), Power Config (0x0001,
  battery), Occupancy Sensing (0x0406). Single endpoint = single DDF fingerprint → deCONZ
  instantiates the `ZHAPresence` sensor cleanly on pair.
- **Reported values:** occupancy (0x0406/0x0000), BatteryPercentageRemaining
  (0x0001/0x0021, half-percent → deCONZ does ÷2).
- Reports go to configured bindings (`EZB_ADDR_MODE_NONE`). Reliable here because the
  light-sleep link is always up; no APS-ack/settle dance needed.
- **Sensitivity write knob:** the ADXL `THRESH_ACT` is exposed as Occupancy attr **0x0012**
  (`PIRUnoccupiedToOccupiedThreshold`, uint8). The attribute value **is the raw register
  setting** (×62.5 = mg). On write, `zcl_action_handler` clamps (≥1), applies it to the ADXL
  live (`adxl345_set_act_thresh`), and persists to NVS (namespace `awning`, key `act_thresh`).
  Boot seeds the ADXL from the persisted value.

### ZED config

`ed_timeout = 64 min`, `keep_alive = 4000 ms` (`ESP_ZIGBEE_ZED_CONFIG()` in
`awning_config.h`). `rx_on_when_idle = false`. Channel is pinned (`ZB_CHANNEL = 15`,
`ZB_CHANNEL_MASK`) so a scan touches one channel; match it to your deCONZ network.

---

## 6. Join / rejoin state machine

`app_signal_handler` (mirrors the vendor example):

- `SKIP_STARTUP` → `bdb_start_top_level_commissioning(INITIALIZATION)`.
- `DEVICE_FIRST_START` / `DEVICE_REBOOT`:
  - factory-new → `NETWORK_STEERING`;
  - already-joined → set `s_joined = true`, fire a baseline report;
  - init failed but not factory-new → retry `INITIALIZATION` with backoff (cold-power-on
    rejoin path — rare under light sleep since the link rarely drops);
  - factory-new + failed → keep `STEERING` until Permit Join opens.
- `STEERING` success → set `s_joined = true`, baseline report; failure → retry with backoff.
- `EZB_ZDO_SIGNAL_LEAVE` → clear `s_joined` (stop reporting until rejoin).

`s_joined` (a `volatile bool`, **owned by the signal handler**, which runs in the stack
context) is the report task's gate — see the next section.

---

## 7. Threading & the boot-crash gotcha

**Never call into the ZBOSS stack (`ezb_*` / `esp_zb_*`) from a non-stack task without the
stack lock, and never before `esp_zigbee_init()` has run.** An early version polled
`ezb_bdb_dev_joined()` from the report task — which is created in `app_main` *before* the
Zigbee task — and it faulted (`Load access fault` in `__assert_func`) on the uninitialised,
unlocked stack, boot-looping.

The fix and the rule:

- The report task **never queries the stack**. It reads the `s_joined` flag that the signal
  handler sets (the handler runs inside the stack context, so that write is safe).
- The actual report calls (`report_occupancy` / `report_battery`) wrap their stack access in
  `esp_zigbee_lock_acquire()/release()`.
- The INT1 ISR only does `xTaskNotifyFromISR` — no stack calls.

---

## 8. Persistence

- **Zigbee network** in the `zb_storage` NVS partition (`esp_zigbee_start(false)` keeps it).
- **Sensitivity** in NVS namespace `awning`, key `act_thresh` (raw `THRESH_ACT` units).
  The key was renamed from `occ_thresh` when the semantics changed from "mg RMS" to "raw
  units" — an old `occ_thresh` value is simply ignored (fresh key), so no migration is needed;
  the device starts at `ACT_THRESH_DEFAULT` until you write a new value.
- `RTC_DATA_ATTR` on the live sensitivity var is now just harmless (light sleep preserves all
  RAM); it would only matter if this were ever run on deep sleep again.

---

## 9. Build notes

- ESP-IDF **6.0.2**; `esp-zigbee-lib >= 2.0.0` (the 2.x `ezbee` API — *not* the 1.x `esp_zb_*`).
- `managed_components/` and the vendor `esp-zigbee-sdk` checkout are **not committed** — the
  lib is re-downloaded from `dependencies.lock` on build.
- The IDF `export.ps1` env script fails when the Windows username contains non-ASCII
  characters (mangled venv path) — build from a working IDF terminal.
- After editing `sdkconfig.defaults`, delete `sdkconfig` so it re-seeds
  (`del sdkconfig; idf.py reconfigure`). Forgetting this is why a light-sleep change can
  silently not take effect.

---

## 10. Known gotchas — quick reference

| Symptom | Cause | Fix |
|---|---|---|
| Motion never reacts | `esp_sleep_enable_gpio_wakeup()` is a no-op under peripheral power-down | Use EXT1 (§3) |
| Motion never reacts | 4 s RMS window averaged the shake to 0; or ISR missed across sleep isolation | Poll the ADXL activity latch (§4) |
| Boot loop, `Load access fault` in `__assert_func` | Stack call from report task without lock / before init | Gate on `s_joined`; lock report calls (§7) |
| Light-sleep change had no effect | Stale `sdkconfig` not re-seeded | `del sdkconfig; idf.py reconfigure` (§9) |
| Sensitivity write does nothing | Writing the old mg semantics / wrong attr | It's raw `THRESH_ACT` on attr 0x0012 (§5) |
| Device aged out, slow rejoin every wake | Deep sleep (the thing we left) | Light sleep keeps the poll alive (§2) |
