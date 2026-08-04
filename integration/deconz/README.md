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
| `state/presence`      | endpoint 14, Occupancy Sensing 0x0406 attr 0x0000 | bool |
| `config/battery`      | endpoint 13, Power Configuration 0x0001 attr 0x0021 | % |

`0x0055` is `presentValue` of the Analog Input cluster; `0x39` is the ZCL `single`
(float) type. Battery attr `0x0021` is ZCL BatteryPercentageRemaining, reported in
half-percent units, hence the `/2` in the parse.

### Phoscon "Motion" card (the occupancy endpoint)

The four analog endpoints (10–13) are `ZHAConsumption` sensors, which **Phoscon does not
render on its Sensors page** — they are visible only in the deCONZ GUI / REST API / Home
Assistant. To get a real Phoscon card, endpoint **14** exposes an **Occupancy Sensing**
cluster (0x0406). deCONZ maps it to a **`ZHAPresence`** sensor, which Phoscon shows as a
**Motion** card (with the battery %). The firmware sets `Occupancy = (movement_rms >
threshold)` on every wake; the raw mg/degree numbers still ride on eps 10–13 for HA to
calibrate the awning-close automation against (see `home_assistant_rest.yaml`). The occupancy
attribute is a bitmap8 (`dt 0x18`); the DDF parses bit 0 into the boolean `state/presence`.
The motion threshold is runtime-adjustable via NVS (namespace `awning`, key `motion_mg`, u16;
default 60 mg) — no reflash needed to change it.

> **⚠ Re-pairing required after flashing this firmware.** Adding endpoint 14 changes the
> device's endpoint layout, so deCONZ must re-read the descriptors. **Delete the node in
> deCONZ/Phoscon and re-join it** (open Permit Join, cold-boot the sensor) once after
> flashing. Routine wakes afterward rejoin silently as before.

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

The sensor pairs on **cold boot** — it has no pairing button. A power-cycle (or **RST**)
with the coordinator network open triggers a fresh commissioning attempt.

**Phoscon has no generic "Other" sensor entry for a DIY analog device**, so do NOT
try to join through *Sensors → Add new sensor* (that screen is a category filter for
known commercial devices). Instead open the network directly:

1. **Open the network for joining FIRST, before you touch the device:**
   - Phoscon → ☰ menu → **Settings → Gateway → Permit join** (opens the network ~180 s), or
   - deCONZ GUI → **Edit → Permit Join**, or click the network padlock icon.
2. **Cold-boot the sensor** to trigger commissioning — cell reinsert or **RST** (per spec §4.1).
3. Keep the ConBee II near the sensor for the first join; relocate afterward.
4. The device joins as a raw node and the DDF binds it (identifiers already match —
   see step 4 of *Install the DDF*).

**Give the join time.** Commissioning on the ESP32-H2 regularly takes ~9–10 s of network
steering; the firmware allows **40 s** for a cold-boot join (`ZB_COMMISSION_TIMEOUT_MS`).
Don't yank power mid-join — on the serial log you'll see `[zb] joining...` then
`[zb] connected after …ms`.

Do **not** use `erase_flash` to force a rejoin — it wipes the stored IEEE address and
creates ghost devices in the coordinator (spec §8.1). A power-cycle is all you need.

**After the first pairing, wakes rejoin silently — no Permit Join needed.** Once joined,
every heartbeat/sensor wake comes up, `Zigbee.begin()` restores the saved network and does
a background NWK rejoin, and the device reports and sleeps again. You do **not** re-open the
network for routine wakes. Three firmware details make this reliable: (1) a radio-settle
delay before `begin()` so the ESP32-H2 stack starts cleanly after deep sleep; (2) the ED is
**pinned to a single Zigbee channel** so the post-sleep rejoin scans one channel instead of
all 16 (Espressif's recommended deep-sleep-rejoin fix — see below); and (3) on a rare
`begin()` failure the firmware just sleeps and retries on the **next** wake with the stored
network intact — it never `ESP.restart()`s (that crashed the H2 with `SW_CPU` and dropped it
into a fresh cold-boot steer that *did* need Permit Join). If a wake ever logs
`[zb] Zigbee.begin failed`, that is expected-and-handled: the next wake reconnects.

> **⚠ Channel must match — `ZB_CHANNEL` in `config.h`.** The firmware pins its scan to
> **channel 15** (this deCONZ network's channel; PAN ID `0x4AD4`). This is what makes the
> deep-sleep rejoin fast and reliable. **If you move the deCONZ network to a different
> channel, you MUST update `ZB_CHANNEL` in `config.h` and re-flash**, or the device will
> never find its parent again. Check the channel in Phoscon → Settings → Gateway → Advanced.
> On each successful join the serial log prints `[zb] on channel 15, PAN 0x4ad4` so a
> mismatch is immediately visible.

**Bindings are written automatically during a 90 s settle window.** After a fresh join
(cold boot), the firmware stays awake **90 s** (`ZB_SETTLE_MS`) instead of
sleeping immediately, re-reporting every ~5 s. A sleepy end device only receives the
coordinator's queued **Bind + Configure-Reporting** requests while it is awake and polling
its parent, so this window is what lets deCONZ populate the binding table on its own. On
the serial log you'll see `[zb] settling for bindings, staying awake 90000ms`. Keep the
network open (Permit Join) across this window on the first pairing.

## Verify

- deCONZ GUI → node → each Analog Input cluster: **Read** attr 0x0055, confirm a plausible
  value (near zero at rest, hundreds of mg when you tap the sensor).
- Phoscon → the sensor should appear; the four values show under its details.
- Check the **binding table** on the node (GUI → node → *Bind dropdown*) — after a fresh
  join it should hold five entries (four Analog Input 0x000C + one Power Config 0x0001),
  written by deCONZ during the 90 s settle window. If it is **empty**, work through the
  causes below in order.

### DDF "failed to load" in the DDF Editor (must fix first)

If the deCONZ DDF Editor says **"failed to load"**, deCONZ rejected the file before it could
ever match your node — so it will never bind. The cause (confirmed against the
deconz-rest-plugin source) is that deCONZ requires **every item `name` to resolve to a
registered item descriptor**. The four custom analog items here — `state/movement_rms`,
`state/movement_peak`, `state/tilt_pitch`, `state/tilt_roll` — are **not** in deCONZ's
built-in item vocabulary, so they must declare an **inline descriptor**. Each therefore
carries:

```json
"schema": "resourceitem1.schema.json",
"datatype": "Double",
"access": "R",
"public": true,
```

which makes deCONZ register the item (`R_AddResourceItemDescriptor`) and resolve the name.
`config/battery` and the `attr/*` items are already registered, so they need no descriptor.

Things that are **not** the cause (don't "fix" these): the fingerprint values as quoted hex
strings (`"0x0104"`, `"0x0a"`) are correct; `modelid`/`manufacturername` as **plain strings**
are correct (do **not** make them arrays); `$TYPE_CONSUMPTION_SENSOR` is a valid macro; and a
missing `"fn"` in `parse`/`read` is harmless (it defaults to `zcl`).

To see the exact rejection reason, run deCONZ with `--dbg-ddf=2` (or check the add-on log)
while loading the file.

### Empty binding table (device joined, DDF matched, values never populate)

This is the expected failure mode for a DIY sleepy end device, and the DDF already carries
the fix. In order of likelihood:

1. **`Mgmt_Bind_Req` not answered (the usual cause).** deCONZ normally *reads* the device's
   binding table before writing to it, and the table you see in the GUI is whatever the
   device returned. arduino-esp32 sleepy end devices typically do **not** answer the ZDP
   `Mgmt_Bind_Req`, so the read times out, the write flow stalls, and the table shows empty
   even though nothing is wrong with the DDF. The DDF sets **`"supportsMgmtBind": false`**
   (top of `awning_wind_sensor.json`) precisely to tell deCONZ *not* to read the table and
   to push the bindings straight to the device. If you removed that line, put it back.
2. **Device wasn't awake when deCONZ pushed the binds.** deCONZ delivers queued Bind +
   Configure-Reporting only while the sleeper is awake and polling. The firmware's 90 s
   settle window (after cold boot) exists for this. Re-trigger a fresh join with
   Permit Join open and let the full window run. deCONZ also re-checks bindings on an hourly
   auto-interval, so it can also self-heal on the next wake.
3. **DDF didn't actually match.** If the node shows as a generic/legacy device rather than
   "Awning wind protection sensor", the binding state machine never starts. Recheck the
   fingerprint `in` lists and `modelid`/`manufacturername` (step 4 of *Install the DDF*).

The `bindings` array schema itself is verified-correct against the deCONZ parser
(`bind:"unicast"`, numeric `src.ep`, string `cl`/`at`/`dt`/`change`, `dst.ep` omitted →
defaults to coordinator EP 0x01), and the report datatypes match the firmware
(`0x39`/single-float for Analog Input present value, `0x20`/uint8 for battery percentage),
so a malformed-JSON cause has been ruled out.

## Reporting note for a sleepy end device

The firmware pushes reports itself while awake (it does not rely on the coordinator
polling it in deep sleep). The `report` bindings above set the coordinator's expectations;
the `read` + `refresh.interval` entries are the fallback poll when the device is awake.
Because the device is asleep most of the time, treat the firmware's own heartbeat
(spec §4.3, 30 min) as the real update cadence, and keep the HA watchdog window at 45 min
(spec §11.3) exactly as before.

**Reports are confirmed before the device sleeps.** On every wake the firmware waits for
the coordinator's ZCL Default Response to its attribute reports (retrying up to 3×) before
returning to deep sleep. This is deliberate: a sleepy ED that fires-and-forgets its reports
can be aged out of the parent's neighbor table mid-sleep, after which the next wake needs a
full network *steering* (Permit Join open) instead of a silent *rejoin* — the "join TIMEOUT
40 s with the network closed" symptom. Confirming delivery keeps the parent relationship
alive so wakes rejoin fast with the network closed. On the serial log you'll see
`[zb] report ack …` and `[zb] all reports confirmed`.
