# Hardware: screws, inserts and what to buy

Shopping list and fitting notes for the enclosure (`mount_style = "rail"`, `esp_hold = "hooks"`,
`adxl_fix = "pins"`, the defaults). Background and design notes are in [ENCLOSURE.md](ENCLOSURE.md).

## Shopping list

| Qty | Part | Where it goes |
|---|---|---|
| 4 | M3 heat-set insert, short (3 to 4 mm long), for a **4.0 mm** hole | Cover end walls |
| 4 | M3 x **10** mm countersunk screw, DIN 7991 / ISO 10642, hex socket 2 mm, stainless A2 | Plate to cover, from the back |
| 2 | M4 countersunk screw (DIN 7991) or rivet, length to suit your awning bar | `awning_base` to the awning |
| 1 | Outdoor double-sided acrylic foam tape (VHB type) | `awning_base` to the awning |
| - | CA glue (a few drops) | Button collars |
| - | TPU 90A filament | `gasket`, `usb_plug` / `usb_hatch` |

Nothing else needs screws: the ESP32 board clicks into 4 hooks in the plate, the GY-291 is
held by the printed `adxl_pins`, the cell sits in printed clips.

## Plate to cover: M3 x 10 countersunk

- **Length 10 mm.** With DIN 7991 the length includes the head. The plate is 6.5 mm thick, so
  3.5 mm of thread goes into the insert.
  - M3 x 8 leaves only 1.5 mm of thread in the insert: too little.
  - M3 x 12 hits the bottom of the 4.5 mm insert hole before the plate is tight.
  - M3 x 6 was for the old 3 mm plate. Do not use it with the current plate.
- **Countersunk head.** The countersinks in the plate back take a 6 to 6.7 mm head, so it ends
  flush and does not catch on `awning_base` when the box slides on.
- **Stainless A2**, because the box lives outdoors.

## Heat-set inserts in the cover

A heat-set insert is a small brass sleeve with an M3 thread inside and knurling outside. It is
melted into the plastic, so the cover gets a metal thread instead of a thread cut in PETG.

```
   insert M3              in the cover (section)
   ┌─┬─┬─┐               ┌──┐ ┌──┐
   │╳│ │╳│  ← knurling    │▓▓│ │▓▓│ ← brass melted into PETG
   │ │ │ │               │▓▓│ │▓▓│
   └─┴─┴─┘               │  └─┘  │ ← hole 4.0 mm, 4.5 mm deep
   M3, ~4 mm long
```

**What to buy:** M3 heat-set inserts, short (3 to 4 mm), made for a 4.0 mm hole. Examples:
Ruthex M3 x 4, CNC Kitchen M3 short. Check the hole size the maker recommends: it must be
4.0 mm (`insert_d` in `enclosure.scad`).

**How to fit them:**

1. Set the insert on one of the 4 holes in the cover end walls.
2. Press it in gently with a soldering iron at about 220 to 250 °C (PETG). The plastic around
   it softens and flows into the knurling. A heat-set insert tip for the iron helps, but a
   plain tip works.
3. Stop when the insert is flush with the surface and let it cool before you load it.

**Why not without inserts:** the holes are 4.0 mm, so an M3 screw spins in them and does not
bite, and a bigger screw does not fit the countersinks in the plate. If you want no inserts,
reprint the cover with `insert_d = 2.6` and drive the M3 screws straight into the plastic.
That thread survives only a few open and close cycles, which is weak for a cover you open to
change the battery.

## `awning_base` to the awning

- VHB tape under the main area of the base, **not** under the latch arm and its root block (the
  arm must move freely). Clean and degrease the bar first.
- On a moving awning also use the 2 countersunk M4 screws or rivets. Their holes sit in the two
  dovetail segments, where the base is 3.7 mm thick, so an 8 mm M4 countersunk head ends flush.
- Leave about 21 mm free on the ADXL side of the base to set the box down and slide it on.

## Only for the other variants

- `mount_style = "dovetail"`: 3 more M3 inserts (2 hatch, 1 lock), 4x M3 x 10 cylinder head
  instead of the countersunk screws, 2x M3 x 8 (hatch), 1x M3 x 10 (lock), 2 stainless hose
  clamps.
- `adxl_fix = "screws"`: 2x M2.5 x 6 self-tapping (GY-291).
- `esp_hold = "clips"`: CA glue for the 2 `board_clips`.
- `button_style = "oring"`: 2x O-ring 2.5 x 1 mm.
- `led_window = "hole"`: 3 mm clear acrylic rod (about 4 mm) or clear epoxy / UV resin.
