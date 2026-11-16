# awning_protection — ESP-IDF firmware

Battery-powered Zigbee **awning movement sensor** for the ESP32-H2 (LaskaKit
ESP32-H2-DEVKit V2 + ADXL345). It reports a single **occupancy** value ("awning
protected") plus battery to a deCONZ coordinator, and runs on **Zigbee light sleep**
so the parent link never drops.

- **One endpoint (10):** Basic + Identify + Power Config (battery) + Occupancy Sensing (0x0406).
- **Occupancy = motion latch:** when the ADXL345 activity interrupt fires (movement crosses
  the sensitivity threshold), occupancy latches **OCCUPIED** immediately; it clears back to
  **UNOCCUPIED** after `OCCUPANCY_CLEAR_S` (default **30 s**) with no further motion.
- **Light sleep, not deep sleep:** the ZED long-poll keeps the coordinator link alive, so
  reports land instantly with no rejoin. Idle current ~1–2 mA (weeks on an 18650).

## Pairing, reset and status LED

- **Unpaired sensor:** power it up (or press RST) with Permit Join open on the coordinator. It
  searches all Zigbee channels for 3 minutes (LED blinks blue), red if the window ends. Short
  press the user button to search again. After it joins, the LED keeps blinking blue for
  another 3 minutes while deCONZ sets the sensor up (the sensor polls fast and stays awake),
  then turns green. Leave Permit Join open and the sensor powered until it is green.
- **Factory reset / move to another coordinator:** hold the user button (IO25) for 5 s until
  the LED blinks red fast, then release.
- Full button and LED reference: [`../../integration/deconz/README.md`](../../integration/deconz/README.md#joining--pairing).

The enclosure has a PAIR plunger for the user button, a recessed RST plunger and a window for
the status LED: see `hardware/enclosure/ENCLOSURE.md`.

## Build

ESP-IDF **6.0.2**. The `export.ps1` env script fails on usernames with non-ASCII
characters — run the build from a working IDF terminal.

```powershell
cd idf/awning_protection
idf.py build flash monitor
```

If you change `sdkconfig.defaults`, delete `sdkconfig` first so it re-seeds
(`del sdkconfig; idf.py reconfigure`).

## Sensitivity (the "threshold" knob)

Sensitivity is the **ADXL345 activity threshold** (`THRESH_ACT` register, **62.5 mg per
unit**). It is the *only* thing that decides whether movement trips occupancy. You set it
over Zigbee on the Occupancy Sensing attribute **`0x0012`**
(`PIRUnoccupiedToOccupiedThreshold`, `uint8`) from deCONZ — the value written **is the raw
register setting** (not mg). It applies to the accelerometer immediately and is saved to NVS,
so it survives sleep and power-cycle. No reflash.

- **Lower value → more sensitive** (triggers on smaller movement).
- **Higher value → less sensitive** (needs a bigger jolt).
- Valid range **1–255**. `0` disables detection and is clamped up to 1.
- Approx trigger in mg = **value × 62.5**  (1000 mg = 1 g = the pull of gravity).
- **Default: 2** (≈ 125 mg), set by `ACT_THRESH_DEFAULT` in `main/awning_config.h`.

### Setting → real movement

| Attr `0x0012` | ≈ Trigger (mg) | ≈ Trigger (g) | Sensitivity | What it takes to trip it — real example |
|--------------:|---------------:|--------------:|-------------|------------------------------------------|
| **1**  |   62 mg | 0.06 g | Very twitchy | A door slam in the room, someone brushing the awning fabric, light building vibration. Prone to false triggers. |
| **2** *(default)* |  125 mg | 0.13 g | High — light nudge | A hand tap on the awning arm; the first stir of a light breeze moving the fabric. Good indoor bench default. |
| **3**  |  188 mg | 0.19 g | High | A gentle push on the awning; a **light breeze (~2–3 m/s)** flapping the valance. |
| **4**  |  250 mg | 0.25 g | Medium-high | A **moderate breeze (~4–5 m/s)** rippling the fabric and lightly rocking the arms. |
| **6**  |  375 mg | 0.38 g | Medium | A **fresh breeze (~6–8 m/s)** visibly shaking the awning; a firm shove by hand. |
| **8**  |  500 mg | 0.50 g | Medium-low — firm shake | A **strong breeze (~8–10 m/s)** giving the awning a real workout; needs a deliberate firm shake by hand to trip on the bench. |
| **12** |  750 mg | 0.75 g | Low | A **near-gale gust (~12–14 m/s)** snapping the awning; a sharp knock on the frame. |
| **16** | 1000 mg | 1.0 g  | Low — real gust/knock | A **strong gust (~15+ m/s)** — the kind of wind you actually want the awning retracted for; a hard whack on the arm. |
| **24** | 1500 mg | 1.5 g  | Very low | Only a **violent gust / storm buffeting** or someone yanking the awning. Ignores everyday breeze. |
| **255** (max) | ~15,900 mg | ~15.9 g | Effectively off | Nothing short of dropping the sensor will trip it. Only useful to mute the sensor. |

> **Wind-speed figures are rough guidance, not calibrated.** How many mg a given wind speed
> produces depends entirely on *your* awning — its size, fabric tension, arm stiffness, and
> where the sensor is mounted on it. A big loose lateral-arm awning shakes far more than a
> small taut cassette one at the same wind speed. **Use the table to pick a starting point,
> then tune on the real awning:** set a value, watch when occupancy trips in deCONZ during
> actual wind, and raise it if it false-triggers on harmless breeze or lower it if it misses
> the gusts you care about.

### How to write it in deCONZ

In the deCONZ GUI (or Phoscon via a DDF-mapped config field), write attribute **`0x0012`** on
the Occupancy Sensing cluster (`0x0406`) of endpoint **10** with your chosen value (1–255).
The firmware logs the change on the serial monitor:

```
sensitivity set to THRESH_ACT raw=8 (~500 mg, persisted)
```

On the next boot it confirms the persisted value:

```
sensitivity loaded from NVS: THRESH_ACT raw=8 (~500 mg)
```

### Tuning tips

- **False triggers on calm days?** Raise the value (e.g. 2 → 4 → 8) until only real wind trips it.
- **Missing gusts you care about?** Lower the value.
- **Occupancy clears too fast/slow after wind stops?** That's a separate knob:
  `OCCUPANCY_CLEAR_S` in `main/awning_config.h` (default 30 s, compile-time). Increase it to
  hold "protected" through gusty lulls; decrease it for a snappier clear.
- The heartbeat log also prints a 4 s movement-RMS window (`movement window rms=… mg`) — handy
  for seeing how much energy your awning actually produces, to inform the threshold choice.

## Key files

| File | Purpose |
|---|---|
| `main/awning_protection.c` | App: light-sleep, pairing window / reset / rejoin, motion-latch report task, ZCL write + Identify handler |
| `main/awning_config.h` | Pins, timings, sensitivity defaults, NVS keys — all tunables |
| `main/adxl345.c` / `.h` | ADXL345 driver (activity interrupt, live threshold, movement window) |
| `main/battery.c` / `.h` | Battery divider read + SoC |
| `main/ui.c` / `.h` | User button (pairing / reset) + SK6812 status LED |
| `sdkconfig.defaults` | Light-sleep config (PM, tickless idle, radio power-down) |

See [`TECHNICAL.md`](TECHNICAL.md) for the architecture (light sleep, motion latch, wake
path, Zigbee/DDF, known gotchas), [`POWER_TUNING.md`](POWER_TUNING.md) for
battery-saving knobs to reach for if the battery drains too fast (none applied yet), and
[`MOTION_CLASSIFIER_DESIGN.md`](MOTION_CLASSIFIER_DESIGN.md) for the proposed
open/close-vs-wind discriminator (design only, not yet implemented).

## Mounting & housing

**Orientation does not matter for detection.** The accelerometer is configured for
**AC-coupled activity on all three axes (X+Y+Z)**: it detects *changes* in acceleration and
continuously re-references gravity as zero, so it triggers on movement in any direction
regardless of which way the housing points — flat, on its side, upside down, tilted. The
firmware never uses tilt/angle for the occupancy decision. Mount the box whichever way fits.

What **does** matter, in priority order:

1. **Rigid coupling to a moving part of the awning.** The sensor only detects awning
   movement as well as the housing transmits it. Fix the housing firmly to the **front bar /
   lead rail or an arm** — a part that actually moves — not to the wall bracket or a part
   that stays still. Avoid rubber/foam-damped mounts: they absorb the vibration you want to
   sense, forcing a lower threshold and inviting false triggers.
2. **Board fixed inside the housing.** The PCB must move *with* the case. If it can rattle,
   it generates its own spurious motion and mutes real movement. Screw or foam-lock it solid.
3. **Antenna clearance.** The ESP32-H2-DEVKit V2 has a **PCB trace antenna**. Keep that edge
   of the board away from the awning's aluminium (metal detunes/blocks it) and behind an
   RF-transparent (plastic) wall — never a metallic/foil-lined enclosure. Face the antenna
   edge outward, toward the coordinator.
4. **Cable strain relief.** Anchor the ADXL/I²C and battery leads — awning vibration will
   eventually fatigue unsupported wires/joints.
5. **Weather sealing (outdoor).** The devkit and 18650 are not weatherproof bare — use at
   least an IP54-ish enclosure with a gland/drip-loop for any cable entry.

> Because detection triggers on **any** axis, a mount that also picks up unrelated vibration
> (a shared wall transmitting door slams, an arm buzzing in traffic) will false-trigger no
> matter the orientation. Fix it with a better mounting point or a higher sensitivity value
> — not by rotating the housing.
