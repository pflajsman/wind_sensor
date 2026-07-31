# deCONZ integration (ConBee II + Phoscon)

For setups where the ConBee II runs **deCONZ**, not Zigbee2MQTT. (A single ConBee II
cannot serve deCONZ and Z2M at the same time — pick one.) The firmware is unchanged:
it is a standard Zigbee end device with four Analog Input endpoints and the standard
battery cluster, so only the host-side mapping differs from the Z2M path.

Verified for **deCONZ 2.33.2** (modern DDF support; well past the 2.23 cutoff).

## What this maps

| deCONZ state item | Source | Unit |
|---|---|---|
| `state/movement_rms`  | endpoint 10, cluster 0x000C attr 0x0055 | mg |
| `state/movement_peak` | endpoint 11, cluster 0x000C attr 0x0055 | mg |
| `state/tilt_pitch`    | endpoint 12, cluster 0x000C attr 0x0055 | deg |
| `state/tilt_roll`     | endpoint 13, cluster 0x000C attr 0x0055 | deg |
| `config/battery`      | endpoint 13, Power Configuration 0x0001 attr 0x0021 | % |

`0x0055` is `presentValue` of the Analog Input cluster; `0x39` is the ZCL `single`
(float) type. Battery attr `0x0021` is ZCL BatteryPercentageRemaining, reported in
half-percent units, hence the `/2` in the parse.

## About the file suffix and where DDFs live

A "DDF" is stored **as a `.json` file** — that IS the correct extension. deCONZ's own
bundled device files are `.json` too; the `"schema": "devcap1.schema.json"` line at the
top of ours is what marks the JSON as a device-capability DDF. So `awning_wind_sensor.json`
is named correctly; there is no separate `.ddf` extension.

**You cannot upload a DDF through the Phoscon web app.** Phoscon is only the
network/pairing front-end and has no DDF management. DDFs are a deCONZ *core* feature,
loaded either by copying the file into the deCONZ `devices/` folder or via the DDF Editor
in the deCONZ **desktop GUI** (VNC/X11), never in Phoscon.

## Install the DDF

1. Copy `awning_wind_sensor.json` into the deCONZ user-DDF `devices/` folder:
   - **Windows:** `%LOCALAPPDATA%\dresden-elektronik\deCONZ\devices\`
     (i.e. `C:\Users\<you>\AppData\Local\dresden-elektronik\deCONZ\devices\`)
   - **Linux (native):** `~/.local/share/dresden-elektronik/deCONZ/devices/`
   - **Home Assistant deCONZ add-on:**
     `/mnt/data/supervisor/addons/data/core_deconz/.local/share/dresden-elektronik/deCONZ/devices/`
     (reach it via the SSH/Samba add-on; the deCONZ add-on's own filesystem is otherwise
     not exposed). Create the `devices/` folder if it does not exist.
   - Bundled (read-only) DDFs live elsewhere under the install dir — put *custom* files
     in the user `devices/` path above, not among the bundled ones.
2. Load it — either way works:
   - **Copy + hot-reload:** after copying, open the deCONZ desktop GUI → node context menu
     or the DDF editor → **Hot Reload** (no restart), **or**
   - **DDF Editor:** deCONZ desktop GUI → **DDF editor** → open the file → **Save** (it
     writes into the `devices/` path and reloads).
   - If neither is convenient, restarting deCONZ also picks up files in `devices/`.
3. Join the sensor — see **Joining / pairing** below.
4. **Confirm `modelid` / `manufacturername` match.** The firmware advertises
   `manufacturername = "DIY"`, `modelid = "awning-wind-sensor"` (set in `zb.cpp`,
   `ZB_MANUFACTURER` / `ZB_MODEL`) and the DDF already matches these. If you change
   either string, change it in **both** places or the DDF stops binding. To double-check
   what the device actually sent, open the joined node in the deCONZ GUI and read Basic
   cluster (0x0000) attrs 0x0005 (model) / 0x0004 (manufacturer).

## Joining / pairing

The sensor has a **pairing button on IO14** (added after the initial build). A press
wakes it and, if it is not on a network, forces a commissioning attempt.

**Phoscon has no generic "Other" sensor entry for a DIY analog device**, so do NOT
try to join through *Sensors → Add new sensor* (that screen is a category filter for
known commercial devices). Instead open the network directly:

1. **Open the network for joining:**
   - Phoscon → ☰ menu → **Settings → Gateway → Permit join** (opens the network ~180 s), or
   - deCONZ GUI → **Edit → Permit Join**, or click the network padlock icon.
2. **Press the IO14 pairing button** on the sensor. It wakes, sees it is not joined,
   and runs commissioning. (Equivalently, a cold boot — cell reinsert or **RST** — also
   commissions, per spec §4.1.)
3. Keep the ConBee II near the sensor for the first join; relocate afterward.
4. The device joins as a raw node and the DDF binds it (identifiers already match —
   see step 4 of *Install the DDF*).

Do **not** use `erase_flash` to force a rejoin — it wipes the stored IEEE address and
creates ghost devices in the coordinator (spec §8.1). The button / a power-cycle is
all you need.

Because it is a sleepy end device, after joining you may need to **press the button
once** (or tap the sensor) so it wakes and completes the report bindings; until then
the values can read as empty.

## Verify

- deCONZ GUI → node → each Analog Input cluster: **Read** attr 0x0055, confirm a plausible
  value (near zero at rest, hundreds of mg when you tap the sensor).
- Phoscon → the sensor should appear; the four values show under its details.
- If values never populate, check the **binding table** on the node (GUI → node → *Bind
  dropdown*) — a sleepy end device sometimes needs one wake (tap it) for bindings to take.

## Reporting note for a sleepy end device

The firmware pushes reports itself while awake (it does not rely on the coordinator
polling it in deep sleep). The `report` bindings above set the coordinator's expectations;
the `read` + `refresh.interval` entries are the fallback poll when the device is awake.
Because the device is asleep most of the time, treat the firmware's own heartbeat
(spec §4.3, 30 min) as the real update cadence, and keep the HA watchdog window at 45 min
(spec §11.3) exactly as before.
