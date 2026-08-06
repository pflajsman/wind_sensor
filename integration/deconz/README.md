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

The sensor pairs on **cold boot** — no pairing button.

1. **Open the network first:** Phoscon → ☰ → **Settings → Gateway → Permit join** (~180 s),
   or deCONZ GUI → **Edit → Permit Join**.
2. **Cold-boot the sensor** (cell reinsert / **RST**) to trigger commissioning.
3. Keep the ConBee II near the sensor for the first join.
4. It joins as a raw node; the DDF binds it (identifiers already match).

> **Channel must match.** The firmware pins its scan to **channel 15** (`ZB_CHANNEL` in
> `main/awning_config.h`). If you move the deCONZ network to a different channel, update
> `ZB_CHANNEL` and re-flash, or the device won't find its parent. On each join the serial
> log prints the joined channel/PAN.

Do **not** use `erase_flash` to force a rejoin — it wipes the IEEE address and creates ghost
devices. A power-cycle is all that's needed.

**After the first pairing, routine wakes rejoin silently — no Permit Join needed.** Because
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

- **DDF Editor says "failed to load":** deCONZ rejected the file before matching. The custom
  `state/presence` item carries an inline descriptor (`"schema": "resourceitem1.schema.json"`,
  `datatype`, `access`, `public`) so deCONZ registers the name; don't remove it. Run deCONZ
  with `--dbg-ddf=2` to see the exact rejection.
- **Node shows as generic, not "Awning protection sensor":** the fingerprint didn't match —
  recheck `modelid` / `manufacturername` against what the device reports (above).
- **Bindings empty:** the DDF sets `"supportsMgmtBind": false` so deCONZ pushes bindings
  directly rather than reading the (unanswered) `Mgmt_Bind_Req` first. Keep Permit Join open
  across the first join so the queued Bind + Configure-Reporting reach the device.
