# Awning protection sensor

Battery-powered **Zigbee awning movement sensor** for the ESP32-H2 (LaskaKit
ESP32-H2-DEVKit V2 + ADXL345). It reports a single "awning protected" occupancy value plus
battery to a **deCONZ** coordinator, running on **Zigbee light sleep** so the link stays
alive and motion is reported instantly.

## Layout

| Path | What |
|---|---|
| [`idf/awning_protection/`](idf/awning_protection/) | The firmware (ESP-IDF 6.x). Start here. |
| [`idf/awning_protection/README.md`](idf/awning_protection/README.md) | Overview, **sensitivity table**, mounting & housing, build. |
| [`idf/awning_protection/TECHNICAL.md`](idf/awning_protection/TECHNICAL.md) | Architecture: light sleep, motion latch, wake path, Zigbee/DDF, gotchas. |
| [`integration/deconz/`](integration/deconz/) | deCONZ **DDF** (`awning_protection.json` + bundle) and install guide. |
| [`integration/ha_package_awning_protection.yaml`](integration/ha_package_awning_protection.yaml) | Home Assistant package. |
| [`integration/home_assistant_rest_protection.yaml`](integration/home_assistant_rest_protection.yaml) | HA REST integration (alternate host path). |

## Quick start

```powershell
cd idf/awning_protection
idf.py build flash monitor
```

Then install the DDF and pair the device: see
[`integration/deconz/README.md`](integration/deconz/README.md).

Sensitivity is tunable live over Zigbee (Occupancy attr `0x0012`) — see the table in the
firmware README.
