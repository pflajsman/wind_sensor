# Power tuning — battery-saving options (for later)

**Nothing here is applied yet.** These are safe knobs to reach for **if the battery drains
faster than acceptable**. None of them change core functionality (motion detection, occupancy
latch, reporting) — they only adjust timeouts / check frequency / logging.

Read this together with [`TECHNICAL.md`](TECHNICAL.md). All settings live in
`main/awning_config.h` unless noted.

## Where the current goes (biggest → smallest lever)

1. **Radio long-poll (`keep_alive` = 4000 ms)** — the ZED wakes the *radio* every 4 s to poll
   the coordinator. The radio is the power-hungry part, so this is likely the largest
   continuous draw.
2. **Activity poll (`MOTION_POLL_S` = 1 s)** — the CPU exits light sleep every second to read
   the ADXL activity latch. 3600 wakeups/hour.
3. **Per-wake logging (`ESP_LOGI`)** — keeps UART alive and lengthens each wake.
4. **Heartbeat 4 s RMS window** — every 30 min a 4 s blocking measurement runs purely for a
   diagnostic log line.

> Key fact that makes most of this safe: **motion still arrives instantly via EXT1 wake**
> regardless of the poll rate. The 1 s poll is only a *backstop* (in case the GPIO edge ISR is
> missed across sleep isolation) plus the driver of the occupancy *clear* timer. So the idle
> poll can be slowed a lot without hurting motion responsiveness.

---

## Option 1 — Slow the activity poll (biggest CPU saving, low risk)

`#define MOTION_POLL_S` in `awning_config.h` (currently `1`).

| Change | Idle wakeups | Effect | Risk |
|---|---|---|---|
| **Adaptive: 30 s idle / 1 s occupied** *(best balance)* | ~30× fewer | Motion still instant via EXT1; clear timer stays tight while occupied | very low |
| Fixed 15 s | ~15× fewer | Clear timing becomes 15–30 s granular | very low |
| Fixed 5 s | ~5× fewer | Clear timing stays tight | negligible |

**Simple version** — one-line change to a fixed value (e.g. `#define MOTION_POLL_S 15`). Clear
timing then resolves to `MOTION_POLL_S`..`MOTION_POLL_S + OCCUPANCY_CLEAR_S`; bump
`OCCUPANCY_CLEAR_S` if you want it tighter.

**Adaptive version** — in `report_task` (`awning_protection.c`), make the `xTaskNotifyWait`
timeout depend on state:

```c
uint32_t poll_s = occupied ? MOTION_POLL_OCCUPIED_S : MOTION_POLL_IDLE_S;
xTaskNotifyWait(0, UINT32_MAX, &note, pdMS_TO_TICKS((uint64_t)poll_s * 1000));
```
with e.g. `MOTION_POLL_IDLE_S = 30`, `MOTION_POLL_OCCUPIED_S = 1`. Idle → rare backstop
wakeups; a real shake still wakes immediately through EXT1 and reports; once occupied it polls
fast to time the clear accurately.

> Do **not** slow this so far that a genuinely-missed EXT1 edge would go unnoticed for too
> long. 30 s idle is a sound backstop; going to minutes starts to matter only if EXT1 ever
> fails, which it shouldn't.

---

## Option 2 — Stretch the radio long-poll (biggest radio saving, low–moderate risk)

`keep_alive` in `ESP_ZIGBEE_ZED_CONFIG()` (`awning_config.h`, currently `4000` ms). This is
how often the radio polls the coordinator.

| Change | Radio polls | Effect | Risk |
|---|---|---|---|
| 4 s → **7.5 s** | ½ | Very safe, modest saving | very low |
| 4 s → **15 s** | ¼ | Good saving; coordinator-initiated reads/writes (e.g. a sensitivity write from deCONZ) land within ~15 s instead of ~4 s | low |

`ed_timeout` is 64 min, far longer than any of these, so the coordinator will **not** age the
device out — the link stays safe. The only real trade-off is latency for
coordinator→device traffic (attribute writes, reads). Device→coordinator *reports* are
unaffected (the firmware pushes those itself).

> If you stretch this and later find deCONZ writes feel sluggish, that's the cause — shorten
> it back. Detection/report latency is unaffected.

---

## Option 3 — Battery-build cleanups (behaviour-neutral)

- **Lower log level.** `sdkconfig.defaults` currently forces `CONFIG_LOG_DEFAULT_LEVEL_INFO=y`.
  Set it to `WARN` (or `ERROR`) for battery builds — trims UART-on time per wake. Flip back to
  `INFO` for debugging. *(After editing `sdkconfig.defaults`: `del sdkconfig; idf.py reconfigure`.)*
- **Drop / shorten the heartbeat RMS window.** Remove the `awning_log_movement();` call from
  the heartbeat block in `report_task` (it's a 4 s blocking, 200-sample diagnostic every
  30 min, purely for a log line). Or shorten `WINDOW_MS` if you still want a rough number.
- **Lengthen the heartbeat.** `HB_INTERVAL_S` 1800 → 3600 (30 → 60 min) halves the periodic
  occupancy+battery report rate. Occupancy still reacts to motion instantly; this only slows
  the keep-fresh/battery refresh. (Keep any Home-Assistant "unavailable" watchdog window
  comfortably longer than this.)

---

## Suggested order if the battery is draining too fast

1. **Option 1 adaptive poll (30 s idle / 1 s occupied)** — biggest CPU saving, no downside.
2. **Option 2 → 7.5 s or 15 s** — biggest radio saving; accept slightly slower deCONZ writes.
3. **Option 3 cleanups** — log level to WARN, drop the heartbeat RMS window, optionally
   heartbeat → 60 min.

Measure before/after with a USB power meter or the battery %-over-time in Home Assistant so
you know which lever actually moved the needle for *your* install.
