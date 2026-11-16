# Board specifications

## LaskaKit ESP32-H2-DEVKit V2 (`laskakit-esp32-h2-devkit-v2/`)

Downloaded 2026-10-14 from https://github.com/LaskaKit/ESP32-H2-DEVKit (TAPR Open Hardware
License, see `LICENSE.txt`). Product page:
https://www.laskakit.cz/laskakit-esp32-h2-devkit-bluetooth--zigbee--thread--matter/

| File | Content |
|---|---|
| `ESP32-H2-DEVKit_v2.pdf` | Schematic, rev 2.0 |
| `ESP32-H2-DEVKit_v2.step` | 3D model of the assembled board (36 MB, git-ignored; re-download from the repo above) |
| `gerbers/` | Production Gerbers + drill file, v9 (2026-01-29) |
| `cpl_v2_v9.csv` / `bom_v2_v9.csv` | Pick-and-place positions and bill of materials |

Key mechanical facts (extracted for `../enclosure.scad`):

- PCB 60.33 x 25.4 x 1.6 mm, **no mounting holes**. Board origin: USB end x = 0, centreline y = 0.
- USB-C J4 (CH9102F UART) at y +6.35, J6 (native USB) at y -6.35; shells overhang the edge by 0.43 mm.
- ESP32-H2-MINI-1 module x 43.64..60.24, |y| <= 6.6; antenna at the x = 60.33 end.
- RESET button (30.66, -8.26), user button IO25 (30.66, +8.26), actuator top 3.57 mm above PCB bottom.
- Battery JST-PH (right angle) x 9.1..17.0 at the -y edge, 7.1 mm tall above PCB bottom (tallest part).
- I2C "uŠup" J3 (SDA IO11, SCL IO10, power switched by IO12) x 11.1..17.1 at the +y edge.
- 2x 16 header pads (2.54 mm pitch) at y = ±11.43, x 18.57..56.67.

## GY-291 (ADXL345 breakout)

No manufacturer drawing exists (generic module, several makers). Typical vendor listing values:
20 x 15 mm board, 2 mounting holes 3 mm, 15 mm spacing. Measure your board.
ADXL345 datasheet: https://www.analog.com/media/en/technical-documentation/data-sheets/adxl345.pdf
