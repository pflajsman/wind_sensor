# deCONZ integration (ConBee II + Phoscon)

DDF and install steps for the **awning_protection** firmware on a ConBee II running
**deCONZ** (not Zigbee2MQTT — a single ConBee II can serve only one). Verified for
**deCONZ 2.33.2**.

The device is a Zigbee **sleepy end device** with **one endpoint (0x0a / 10)** carrying
Basic (0x0000), Identify (0x0003), Power Configuration (0x0001, battery) and Occupancy
Sensing (0x0406). Occupancy is the single "awning protected" value; the sensitivity knob
rides on the Occupancy `PIRUnoccupiedToOccupiedThreshold` attribute (0x0012).

## What this maps

| deCONZ item | Source | Meaning |
|---|---|---|
| `state/presence`   | ep 10, Occupancy Sensing 0x0406 attr 0x0000 (bit 0) | awning protected (occupied) — a Phoscon **Motion** card |
| `config/battery`   | ep 10, Power Config 0x0001 attr 0x0021 (÷2)          | battery % |
| `config/sensitivity` | ep 10, Occupancy 0x0406 attr **0x0012** (writable) | ADXL activity threshold, raw units (×62.5 = mg). Lower = more sensitive. |

Single endpoint = single fingerprint, so deCONZ instantiates the `ZHAPresence` sensor
(Phoscon "Motion" card) cleanly on pair. See the main firmware README for the full
sensitivity table (`../../idf/awning_protection/README.md`).

> **Sensitivity is written live.** `config/sensitivity` is the raw ADXL `THRESH_ACT`
> value (1–255). The firmware applies it to the accelerometer immediately and persists it
> to NVS. Because the device is a sleepy ED, a write from deCONZ is queued and takes effect
> on the device's next wake.

## Files

| File | Purpose |
|---|---|
| `awning_protection.json` | The DDF (a DDF is stored as `.json` — that is the correct extension). |
| `awning_protection.ddb` | Pre-built offline DDF **bundle** (DDF + the generic items it references). |
| `build_bundle_protection.py` | Rebuilds `awning_protection.ddb` offline from the JSON + `generic/`. |
| `generic/` | Shared item/constant descriptors the DDF references. |

## Install the DDF

Copy `awning_protection.json` into the deCONZ user-DDF `devices/` folder:

- **Windows:** `%LOCALAPPDATA%\dresden-elektronik\deCONZ\devices\`
- **Linux (native):** `~/.local/share/dresden-elektronik/deCONZ/devices/`
- **Home Assistant deCONZ add-on:**
  `/mnt/data/supervisor/addons/data/core_deconz/.local/share/dresden-elektronik/deCONZ/devices/`
  (reach it via the SSH/Samba add-on). Create `devices/` if it doesn't exist.

Then load it: deCONZ desktop GUI → **DDF Editor** → open the file → **Save/Hot Reload**,
or just restart deCONZ (it picks up files in `devices/` on start). **You cannot upload a
DDF through Phoscon** — Phoscon is only the pairing front-end; DDFs are a deCONZ core feature.

**Confirm the identifiers match.** The firmware advertises `manufacturername = "DIY"`,
`modelid = "awning-protection"` (set in `main/awning_config.h`,
`ZB_MANUFACTURER_ZCL` / `ZB_MODEL_ZCL`) and the DDF matches these. If you change either
string, change it in **both** places or the DDF stops matching. Read what the device
actually sent from the joined node's Basic cluster (0x0000) attrs 0x0004 (manufacturer) /
0x0005 (model).

## Joining / pairing

Pairing is controlled by the **user button (IO25)** and the **status LED (IO22)**. The
sensor scans **all Zigbee channels (11 to 26)**, so it joins any coordinator without a
reflash. It only joins a network with Permit Join open.

### Pair a new sensor

1. **Open the network:** Phoscon: Settings, Gateway, **Permit join** (about 180 s), or deCONZ
   GUI: **Edit, Permit Join**.
2. **Power the sensor** (insert the cell, or press **RST**). An unpaired sensor opens a
   **3 minute pairing window**: the LED **blinks blue**.
3. When it joins, the LED **blinks green for 3 minutes** while deCONZ reads
   and binds it with the DDF (identifiers already match). The sensor polls every 250 ms and
   stays awake during this time. Then the LED is **solid green for 2 s**. Keep the sensor powered
   until it is green.
4. If no open network is found within 3 minutes, the LED is **red for 2 s** and the sensor
   stops scanning to save the battery. **Short press** the button (or press RST) to try again.

### Move a sensor to another coordinator / factory reset

1. **Hold the user button for 5 s** until the LED **blinks red fast**, then **release**.
2. The sensor sends a Zigbee *leave* to its old coordinator (if reachable), erases its network
   data **and its sensitivity setting**, reboots, and opens a new pairing window (blue blink).
3. Pair it as above.

Holding the button longer than 20 s cancels (stuck-button protection). Releasing before 5 s is
a short press, not a reset.

### Button and LED reference

| Action | When | Result |
|---|---|---|
| Short press (hold about 1 s) | Paired and connected | LED green flash, sends occupancy + battery now |
| Short press | Unpaired | Opens / extends the 3 min pairing window |
| Short press | Paired, coordinator unreachable | Rejoin attempt now |
| Hold 5 s, release | Any | Factory reset, then pairing window |
| RST | Any | Reboot. Unpaired: pairing window. Paired: reconnects (LED green 2 s) |
| Delete in deCONZ / Phoscon | Paired | Sensor stops reporting. Hold the button 5 s to pair it again |
| Identify in deCONZ | Paired | LED blinks white |

The button cannot wake the sensor from light sleep, so it is sampled once per second: hold a
short press for about 1 s.

| LED | Meaning |
|---|---|
| Blue blink | Pairing window open (searching for a network) |
| Green blink | Joined, deCONZ is setting the sensor up (3 min): keep it powered |
| Green 2 s | Joined / reconnected |
| Red 2 s | Pairing window ended without a network |
| Fast red blink | Long press reached: release to factory reset |
| Solid red | Factory reset in progress |
| White blink | Identify |
| Off | Normal operation |

> **Deleting the sensor in deCONZ while Permit Join is open** makes it rejoin right away
> (it opens a pairing window after the reset). Close Permit Join first if you want it gone.

Do **not** use `erase_flash` to reset a sensor: use the 5 s button hold. A power-cycle of a
paired sensor only reconnects it to the same coordinator.

**After pairing, routine wakes rejoin silently, no Permit Join needed.** Because
this firmware uses **Zigbee light sleep** (not deep sleep), the parent link stays alive the
whole time via the ZED long-poll, so there is no aging-out and no rejoin race. Reports land
against a live coordinator instantly.

## Verify

- Phoscon: the sensor appears as a **Motion** card with a battery %. Shake it → it flips to
  presence/occupied; after 30 s of quiet it clears.
- deCONZ GUI → node → Occupancy Sensing (0x0406) → **Read** attr 0x0000 (occupancy) and
  0x0012 (sensitivity). Write 0x0012 to retune (raw 1–255; see the firmware README table).
- Binding table (GUI → node → *Bind*) should hold the Occupancy + Power Config entries after
  a fresh join.

### If it never instantiates / the DDF "failed to load"

- **DDF Editor says "failed to load":** deCONZ rejected the file before matching. Run deCONZ
  with `--dbg-ddf=2` to see the exact rejection.
- **Sensor found, but the Sensors list (or Help, API information) is empty or broken:** the
  DDF follows the official deCONZ presence-sensor DDFs (for example
  `sonoff/snzb-03p_motion_sensor.json` in deconz-rest-plugin). All of them have
  `config/on`, `config/reachable` and `state/lastupdated`; older versions of our DDF lacked
  them. The `generic/items/` files are unmodified copies from deconz-rest-plugin, so the
  bundle does not change the shared item definitions other devices use.
- **Node shows as generic, not "Awning protection sensor":** the fingerprint didn't match —
  recheck `modelid` / `manufacturername` against what the device reports (above).
- **Bindings empty:** the DDF sets `"supportsMgmtBind": false` so deCONZ pushes bindings
  directly rather than reading the (unanswered) `Mgmt_Bind_Req` first. Keep Permit Join open
  across the first join so the queued Bind + Configure-Reporting reach the device.
