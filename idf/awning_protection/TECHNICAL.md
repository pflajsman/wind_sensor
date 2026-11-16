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
| Status LED | 22 | SK6812 RGB (V2 moved it from IO9 to IO22); powered from VSENSOR |
| VSENSOR enable | 12 | RT9080 LDO IC1 EN (10k pull-down); on only while the LED is lit |
| User button | 25 | pairing / factory reset; active-low assumed (`BUTTON_ACTIVE_LEVEL`) |

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

`ed_timeout = 4 min`, `keep_alive = 4000 ms` (`ESP_ZIGBEE_ZED_CONFIG()` in
`awning_config.h`). `rx_on_when_idle = false`. Pairing only scans **channel 15**
(`ZB_CHANNEL`, `ZB_CHANNEL_MASK = 1 << 15`), as the pre-button firmware did, so it cannot
join a neighbour's open network. `0x07FFF800` scans 11 to 26.

---

## 6. Join / pairing / reset state machine

`app_signal_handler`:

- `SKIP_STARTUP` → set `s_stack_ready`, `bdb_start_top_level_commissioning(INITIALIZATION)`.
- `DEVICE_FIRST_START` / `DEVICE_REBOOT`:
  - factory-new → **open the pairing window** (`pairing_open`): `NETWORK_STEERING` now, LED
    blue blink, deadline `PAIR_WINDOW_S` (180 s);
  - already-joined → `s_joined = true`, LED green, baseline report;
  - init failed but not factory-new → retry `INITIALIZATION` with backoff 3 → 60 s (coordinator
    down; a short button press retries immediately);
  - factory-new + failed → pairing window (steering scheduled, not inline).
- `STEERING` success → close window, `s_joined = true`, baseline report, and the
  **interview window** (`interview_start`): keepalive (poll) `INTERVIEW_POLL_MS` (250 ms)
  instead of `ZED_KEEP_ALIVE_MS` (4 s) plus a `NO_LIGHT_SLEEP` PM lock for `INTERVIEW_S`
  (180 s), LED blue. deCONZ's descriptor reads, binds and report config otherwise each wait
  up to 4 s at the parent and the interview can time out half done. Then poll 4 s, light
  sleep, LED green. Rejoins of a paired sensor skip this.
  Failure → retry every `PAIR_RETRY_S` (3 s) **until the deadline**, then LED red and stop.
  An unpaired sensor no longer scans forever.
- `EZB_ZDO_SIGNAL_LEAVE`:
  - leave **with** rejoin → clear `s_joined`, the stack rejoins on its own;
  - leave **without** rejoin after our own 5 s button reset (`s_reset_requested`) →
    `esp_zigbee_factory_reset()` (wipes Zigbee data, reboots) → boots factory-new → pairing window;
  - any other leave without rejoin (deCONZ delete, stale entry) → only clear `s_joined` and
    pause reports, as before the button firmware. Wiping + rebooting here made deCONZ create
    the sensor during pairing and then delete it again.

### User button (IO25) and LED (IO22), `main/ui.c`

- IO25 is not an LP GPIO, so it **cannot wake light sleep**. The report task calls
  `ui_tick()` on every 1 s wakeup; if the button is down, the button task takes a
  `ESP_PM_NO_LIGHT_SLEEP` lock and times the press at 20 ms resolution.
- Short press → `short_press_stack` (posted with `esp_zigbee_task_queue_post`): joined =
  ACK + report; unpaired = open/extend window; paired but offline = rejoin now.
- Hold ≥ 5 s, release → erase app NVS (`awning` namespace), then if joined
  `ezb_bdb_reset_via_local_action()` (sends leave; its LEAVE signal finishes the reset), with
  a 10 s fallback to `esp_zigbee_factory_reset()` if no leave confirmation arrives. Not joined
  → `esp_zigbee_factory_reset()` directly. Holds > 20 s are ignored.
- LED: SK6812 via `espressif/led_strip` (RMT). The LED task holds a PM lock only while a
  pattern is showing; off = normal light sleep. The SK6812 VDD is VSENSOR, so `ui.c` drives
  IO12 high for each lit frame and low when dark (an idle SK6812 draws about 1 mA).
- Identify: `EZB_ZCL_CORE_IDENTIFY_EFFECT_CB_ID` and writes to IdentifyTime (0x0003/0x0000)
  blink the LED white.

### Link loss and hang recovery

- On an end device `ed_timeout` is how long keep-alive polls may fail before the stack declares
  the parent lost. It was 64 min; now 4 min.
- `EZB_NWK_SIGNAL_NETWORK_STATUS` with `PARENT_LINK_FAILURE` (or `NO_ACTIVE_LINKS_LEFT`) →
  `link_lost()` → reboot after 2 s, except within `LINK_LOSS_GRACE_S` (300 s) of a fresh
  join, where it is only logged so the deCONZ setup is not cut off. The boot path rejoins with the stored keys and backs off
  if the network is still down. Before this, nothing handled parent loss: the sensor stayed
  unavailable until RST.
- Task watchdog: the report task feeds it every 1 s; `CONFIG_ESP_TASK_WDT_PANIC=y`,
  timeout 30 s, so a hung task reboots the chip.
- Boot logs `reset reason` (`esp_reset_reason()`): tells a firmware fault (panic / watchdog)
  from a link-loss reboot (software) or a power-up / RST.

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
| Sensor unavailable until RST pressed | Parent lost (router unplugged, coordinator restart); nothing triggered a rejoin, and `ed_timeout` was 64 min | Link-loss reboot + 4 min `ed_timeout` + task watchdog (§6) |
| Device aged out, slow rejoin every wake | Deep sleep (the thing we left) | Light sleep keeps the poll alive (§2) |
