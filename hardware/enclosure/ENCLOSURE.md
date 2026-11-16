# Enclosure: 3D-printed housing for the awning sensor

`enclosure.scad` is a parametric OpenSCAD model. The ready-to-print STL files in `stl/`
were exported with the default values.

```
  front (label, PAIR + RST pockets, LED window)   cover: one piece, no visible screws
  +--------------------------------------------------------------------+
  | [GY-291] [ ===== 18650 in 2 snap clips ===== ] [ ESP32-H2 devkit |USB]=[hatch]
  +--------------------------------------------------------------------+
  back plate (6.5 mm, stiff): all mounts, gasket groove, dovetail channel in the back
  ============ awning_base: rail + snap latch, stuck/screwed to the awning ==========
```

## Construction

- **Back plate (`plate`)** carries everything: GY-291 on 2 snap pins (`adxl_pins`), the 18650 in two snap
  clips, and the ESP32 board on support posts, side fences, antenna-end stops and 2 separate
  snap clips (`board_clips`). The board has no mounting holes.
- **Cover (`cover`)** is one deep shell that goes over the plate from the front. Its front
  is clean apart from the engraved label, the two button pockets and the LED window.
- **Screws from the back:** 4x countersunk M3 go through the plate into short heat-set inserts
  in the cover's end walls. They are only reachable with the box off the awning base.
- **Hold-down ribs inside the cover** press the ESP32 board down (on a strip with no parts,
  0.3 mm interference) and keep the cell in its clips. No separate clamp parts.
- **Seal:** a printed TPU `gasket` (or 2 mm cord) in a groove in the plate, running inside
  the 4 screws, so the screw holes are outside the sealed space.

## Parts

| Part | Size (mm) | Print |
|---|---|---|
| `plate` | 176.9 x 36.8 x 22.4 | Back down |
| `cover` | 176.9 x 36.8 x 23.8 | Front face down (label on the bed) |
| `plunger` (PAIR, IO25 user button, `button_style = "oring"`) | 6.5 x 12.3 | Standing on its flange |
| `reset_plunger` (RST) | 6.5 x 10.8 | Standing on its flange |
| `button_pin` (PAIR), `reset_button_pin` (RST) | 8.2 x 13.1 / 4.7 x 11.6 | PETG, head down |
| `button_collar` (2x) | 6.5 x 1.5 | PETG, flat |
| `board_clips` (only with `esp_hold = "clips"`; 2 clips, mirror images) | 3.3 x 12.3 x 3.3 each | PETG, lying flat as exported, beak end on the bed |
| `usb_clamp` (optional, not with `esp_hold = "clips"`) | 7.7 x 24.1 x 11.8 | PETG, top down, pockets up |
| `usb_hatch` | 36.5 x 25 x 4.8 (3 + seal bead) | TPU 90A, outer face down |
| `usb_plug` (instead of `usb_hatch`) | 24.5 x 20.5 x 8 | TPU 90A, cap down, stem up |
| `gasket` | 164.9 x 35.4 x 2.0 | TPU 90A, flat, ridge up |
| `awning_base` | 176.9 x 41.7 x 4.7 | PETG or ASA, flat, as exported |
| `adxl_pins` (4: 2 + 2 spares) | 6 x 8.8 x 2.4 each | PETG, lying on a flat, as exported |
| `mount` (`mount_style = "dovetail"` only) | 75 x 36.8 x 15.7 | Plate down |

Print the rigid parts in **PETG or ASA** (PLA softens in direct sun), 4 perimeters, 30 to
40 % infill, no supports. Needs a bed of at least 180 mm. The two seals are TPU, see below.

## Holding the ESP32 board: 4 snap hooks in the plate

The board clicks in by pressing it down. 4 hooks are moulded into the plate (no separate
parts, no glue): 2 at the USB end and 2 in the middle of the board, one on each long edge.
The middle pair keeps the battery end of the board free for the wiring.

```
        USB end hooks              middle hooks
          ◣                           ◣
   ═══════╪═══════════════ PCB ═══════╪════════════════════════════
          ║  arm 13 mm, starts 2 mm   ║            (battery end free)
   ───────╨─ down in a moat ──────────╨──── plate (6.5 mm) ────────
```

- **Why these do not break like the first ones:** the first hooks were 8 mm tall and 1.6 mm
  thick and had to bend 0.9 mm, about 3.4 % strain across the layer lines. The plate is now
  6.5 mm thick, so each arm starts 2 mm below the plate surface, in a moat cut around it.
  That makes the arm 13.3 mm long; it is 1.2 mm thick and bends 0.7 mm, about **0.7 %**
  strain, roughly a fifth of before. The moat is blind (1.5 mm of plate stays under it), so
  the back stays sealed.
- **Beaks on free copper only:** USB end on board x 7.6 to 9.0 (between ESD diodes D7/D10 and
  the battery JST). The middle beaks are 0.6 mm wide and sit in the 0.89 mm gap between header
  pads 38.885 and 41.425 (drill file), between the two side fences. The arms are wider below
  the beak. The stop blocks at the antenna end are now 0.1 mm from the board (was 0.3), so the
  board cannot shift far enough in X to put a beak on a pad: checked with 0.5 mm solder on every
  pad, for the board 0.1 mm toward the antenna up to 0.2 mm toward the USB end.
- Checked in the model: the board fits with nothing overlapping; raised 0.3 mm it hits the
  hooks at both ends; the cover clears the hooks.

Fitting: set the board on the posts between the side fences and press it down near each end
until both pairs of hooks click. To take it out, pull the two hooks at one end outward with a
fingernail and lift that end, then do the same at the other end.

The previous variant (2 separate clips at the antenna end and a fixed bridge over the USB-C
shells) is still available with `esp_hold = "clips"`.

Other changes on the new plate:

- **Free space under the board for wiring.** The 3 full-width support ribs are now pairs of
  small posts (2 x 4 mm, 3.5 to 7.5 mm off the centre line). The centre, the edges (header
  pads) and the gaps between the posts are free for battery and sensor wires under the board.
- **Side fences** are 2.0 mm thick instead of 1.4.
- **Cable guides** beside the battery (see below).
- The plate must be reprinted for these changes. The cover is unchanged.

### Cable guides

Loose ADXL and battery wires used to creep up between the ESP32 board and the cover. The plate
now has 6 small arches on the floor (`cable_guides`), 3 in each lane between the cell and the
side wall, placed clear of the battery end walls and clips:

```
   side wall |  ┌──┐ ┌──┐   ← two halves, 45 deg roof, 1.2 mm slot on top
             |  │  └─┘  │
             |  │ wires │   channel 4.5 x 2.5 mm (plus the roof triangle)
   floor  ───┴──┴───────┴────
```

- Push the wires in through the slot (the halves flex apart a little) or thread them through.
  The channel takes about 6 thin wires (for example 26 AWG).
- Use the lane on whichever side suits the wiring; both have guides.
- At the antenna end, turn the wires inward in the 6 mm gap after the battery and run them
  under the ESP32 board through the middle, between the two stop blocks, not over it. The
  hooks stand at the board edges at both ends, so there is little room along the walls there.
- `cable_guide_x` moves the guides, `cable_ch` and `cable_slot` resize them,
  `cable_guides = false` removes them.

`usb_clamp` (a block on the two USB-C shells, pressed by the cover) is optional; it is not
used with `esp_hold = "clips"`, where the plate bridge does that job.

## TPU seals (gasket and USB hatch)

Both fit the grooves that are already in the plate and cover, so they work with a plate and
cover you have already printed. Neither part changes the plate or cover geometry.

- **`gasket`** replaces the 2 mm cord. It is a 1.8 x 0.8 mm base that sits loose in the 2.0 x
  1.5 mm plate groove, with a 0.9 mm ridge on top that stands 0.5 mm (`gasket_squeeze`) above
  the plate face (0.3 in the first version sealed only just). The cover flattens the ridge. Lay it in the groove with the ridge up.
- **`usb_hatch`** (needs `mount_style = "dovetail"`: its lower screw goes into the sled) with
  `hatch_seal = "bead"` (default) is one TPU part: a 1.2 mm bead on its
  inner face goes into the cover's port groove and is 0.3 mm (`hatch_squeeze`) longer than the
  groove is deep. No cord. `hatch_seal = "cord"` gives the old flat hatch for PETG + cord.
- **`usb_plug`** is a push-in alternative to `usb_hatch`, with no screws to undo when charging.
  A hollow stem goes 5.5 mm into the cover's USB tunnel and is held by 2 barbed ribs
  (0.3 mm interference, `plug_rib_int`). The cap carries the same seal bead into the port
  groove (0.2 mm squeeze, `plug_squeeze`), so there are two seals. A pull tab at the top
  stands off the box so a fingernail gets behind it. Pull the plug straight out by the tab.
  The cap is only as wide as the flat part of the box's end face, so it does not stick out
  past the rounded corners. No strap, no screw.
  If it is too loose or too tight in your printed tunnel, change `plug_rib_int`
  by 0.1 and reprint.
- TPU does not compress in volume, so a squeezed seal must still fit in its groove. The model
  checks this: the gasket fills 84 % of its groove and the bead 72 % when squeezed.

Printing TPU 90A: direct-drive extruder, slow (15 to 25 mm/s), little or no retraction, 0.2 mm
layers, 100 % infill (both parts are mostly perimeters anyway). Use a 0.45 mm line width so the
0.9 mm ridge prints as exactly 2 lines.

Fitting:
- Tighten the 4 cover screws evenly until the cover sits flat on the plate. If it needs a lot
  of force, or the long sides gap open in the middle, reprint the gasket with
  `gasket_squeeze = 0.4`. If it still seals only just, 0.6 also fits (87 % fill); the model
  stops above 90 %.
- On the hatch, tighten only until the hatch face touches the cover. TPU under a screw head
  creeps, so do not crank it; a pan or cylinder head M3 that fits the 6.2 mm counterbore is fine.

### Outside dimensions (default values)

| | Length (X) | Width (Y) | Height (Z) |
|---|---|---|---|
| Closed box (plate + cover) | 176.9 | 36.8 | 30.3 |
| Mounted on the awning (with `awning_base`) | 176.9 | 41.7 | 32.3 |
| Old dovetail version, mounted with `mount` | 179.9 | 36.8 | 39.0 |

The previous lid-and-ears design was 194.8 x 58 x 36.7 mm.

To go smaller still: `bat_l = 66` for a bare (unprotected) cell saves 4 mm, and
`gap_antenna` can drop from 6 to 4 at some cost to radio range.

## Buttons and status LED

```
   front, USB end on the right:

        "...Lab"   (o)      ( RST )  RST     <- small pocket, pin 2.5 mm deep: pen tip
                 LED ring
                            (( O ))  PAIR    <- finger pocket, pin 1 mm deep
```

- **PAIR** (IO25 user button): short press = status / pairing window, hold 5 s and release =
  factory reset (see `integration/deconz/README.md`). Finger pocket, `user_recess = 1`.
- **RST** (EN): reboots the board. Recessed 2.5 mm in a 5.5 mm pocket (`reset_recess`,
  `reset_pocket_d`) so it needs a pen tip and is not pressed by accident.
- Both pins are the same design (4.0 mm pin, 2.5 x 1 mm O-ring); only the length differs, so
  print the right one for each bore (`plunger` is the longer one, for PAIR).
- Small "PAIR" / "RST" marks are engraved next to the pockets (`button_marks`).
- **Captive buttons (`button_style = "captive"`, default):** the pin goes into the bore from
  the **front**. Its head sits in the pocket (PAIR head 8.2 mm, 0.2 mm below the surface; RST
  head 4.7 mm, 1.7 mm below). From inside, a `button_collar` slides onto the pin's thin
  3 mm end up to the shoulder and is glued there (CA glue on the stub only, not on the boss).
  The head stops the pin going in, the collar stops it coming out, so the pin is held by the
  cover alone. It moves freely (4.0 pin in the 4.3 bore, no O-ring), 0.8 mm until the head
  reaches the pocket floor. The switch spring pushes it back. Works with covers that are
  already printed.
  - **Reach (`pin_reach`, default 0.4):** the 3 mm stub sticks out this far below the collar.
    The pin end then just touches the switch at rest (nominal 0 gap) and presses it up to
    0.8 mm at full stroke. The first version (no extra stub, 0.4 mm gap) was too short in
    practice: print and assembly tolerances ate the overlap and the switch did not click.
  - **Test set** in `stl/button_pin_test/`: both pins with reach 0.2, 0.6, 0.8 and 1.0 mm
    (the default 0.4 is `stl/button_pin.stl` / `reset_button_pin.stl`). Print a few, fit
    them without glue and keep the shortest that clicks reliably. If the button triggers on
    its own (the LED shows a press with nobody touching it), the pin is too long: go one step
    shorter. Glue the collar tight against the shoulder, otherwise the reach changes.
  - Not sealed yet. A TPU membrane over the pocket (or a TPU washer on the pin) is the
    planned seal.
  - The earlier TPU pins with moulded lips were dropped: they jammed in the bore.

### LED window

The SK6812 LED sits about 12 mm under the front and only 4 mm from the RST plunger, so a
window straight above it would cut into the RST bore. Instead a **shrouded light channel**
runs from just above the LED, slightly slanted, to a window 4 mm further from the RST
pocket. The channel keeps at least 1 mm of wall to the RST bore (checked by `assert`s in the
model). An engraved ring marks the window.

`led_window` picks how the light gets out:

| Mode | How | Use with |
|---|---|---|
| `thin` (default) | The channel stops 0.6 mm (`led_skin`) under the outer face. The skin glows. Fully sealed, nothing to assemble | **White, natural or other translucent PETG.** Does not work with black/dark ASA |
| `hole` | 3.1 mm through-hole. Glue in a 3 mm clear acrylic rod, or fill with clear epoxy / UV resin | Any colour, including opaque ASA. The glue is the seal |
| `none` | No window | |

Notes:
- With `thin`, print a test piece first: how bright the window looks depends on the filament.
  If it is too faint in daylight, raise `LED_BRIGHTNESS` in the firmware (`awning_config.h`)
  or use `hole`.
- The channel bottom ends about 4.9 mm above the PCB top, above the RST button and (by the
  datasheet height of a vertical 1 mm uŠup header) the J5 connector. Check J5 on your board.

## Board data

| Board | Source | Confidence |
|---|---|---|
| LaskaKit ESP32-H2-DEVKit V2 | Manufacturer's Gerber, pick-and-place and STEP files (production v9, 2026-01-29), in `specs/` | Exact |
| GY-291 (ADXL345) | Typical vendor figures: 20.3 x 15.7 mm, 2 holes 3 mm, 15 mm apart | **Measure yours**, makers differ |
| 18650 cell | Standard 18 mm diameter; length 65 mm bare, up to 70 mm protected | Check your cell |

What the ESP32 data showed:

- **60.33 x 25.4 mm, no mounting holes:** held by 4 snap hooks moulded into the plate (2 per end), side fences,
  stop blocks at the antenna end and the cover rib at a component-free strip between
  connector J5 and the radio module.
- **Two USB-C ports at one short end.** J4 is the CH9102F USB-UART port, which carries the
  firmware console (`CONFIG_ESP_CONSOLE_UART`). J6 is the ESP32-H2's own USB. Both charge
  the cell. The default opening is for J4 (`usb_ports = "uart"`; `"both"` opens both).
- **Antenna at the other short end,** facing a 6 mm gap toward the battery holder.
- **RESET button** (30.66, -8.26 mm on the board) and **user button IO25** (30.66, 8.26)
  sit under the two front plungers. The **SK6812 LED** (LED_IO22) is at (34.72, -7.39), next
  to RESET.
- The battery JST and the I2C connector (J3, 1 mm "uŠup") are near the USB end. Route the
  ADXL wires and battery leads along the sides of the battery holder.

## Waterproofing

- Cover to plate: TPU `gasket` (or 2 mm silicone / EPDM cord) in the plate groove, 4x M3 screws.
- Buttons (PAIR and RST): with the O-ring plungers, 4.0 mm pins in 4.3 mm bores, each sealed
  by a **2.5 x 1 mm O-ring** in the pin groove; fit the O-rings and push the pins into their
  bores from inside the cover before closing. The captive buttons are **not sealed yet**.
- LED window: sealed by the 0.6 mm skin (`thin`) or by the glued rod / resin (`hole`).
- USB port, one of:
  - `usb_plug`: barbed TPU stem in the port tunnel plus the bead in the groove around the
    port. No screws.
  - `usb_hatch`: TPU hatch with its moulded bead in the groove around the port (or a PETG
    hatch with 2 mm cord), 1 screw above the port (into the cover) and 1 below (into the plate).
- Nothing passes through the sealed space.

Realistic target is splash-proof (roughly IP54 to IP65), not submersion. FDM walls can seep:
4 perimeters plus a slightly higher extrusion multiplier help, a coat of clear lacquer more.

The 4 screws sit in the end walls only. With the earlier 3 mm plate the gasket pushed the
middle of the plate away from the cover and the long sides did not seal; the plate is now
6.5 mm thick (`back_t = 3.5` under the old 3 mm floor), about 10 times stiffer in bending.

## Mounting on the awning: `awning_base` (rail + snap latch)

The first thin base (paws at one end, a spring latch at the other) held the box only by two
short V grooves in the end faces and did not feel secure, and its 3 mm plate bent. Now
(`mount_style = "rail"`):

```
   ADXL end                                                          USB end
   plate:   |..[ slot 20 ][ opening 21 ]............................[ slot 20, open ]
   base:    |..[ rail 20 ]..............................................[ rail 20 ]
   1) set the box down 20.5 mm toward the ADXL end: the ADXL rail drops into the opening,
      the USB rail is still outside the box
   2) slide 20.5 mm toward the USB end: both rails enter their slots, the latch clicks
```

- **Stiffer plate:** the box back is 3.5 mm thicker (6.5 mm in all), so it no longer bends
  between the 4 end screws and the gasket can seal along the long sides. Nothing above the
  old plate face changed: the printed cover, gasket, board clips and USB plug still fit.
- **Two short dovetails:** one 20 mm segment at each end (`rail_seg`; 10 mm wide at the face,
  45 degree flanks, 2 mm deep, 0.15 mm clearance). They hold the box down at both ends, so it
  cannot be pulled off upward. The USB-end slot is open to the end face. The ADXL-end slot
  starts 16 mm from the end face (clear of the sensor pin chambers); its closed end is the
  stop. Behind it (toward the middle) the plate has a 21 mm drop-in opening as wide as the
  rail top, so the box goes on from above and slides only 20.5 mm (`rail_travel`), instead of
  about 180 mm with the earlier full-length rail.
- **Snap latch ("click"):** a spring arm on the base beside the box (+Y side, near the USB
  end), 25 mm long, 2 mm thick. Its tooth snaps into a notch in the side of the plate. The
  lock face is square, so pulling on the box cannot release it, only the grip can. The arm
  stands upright and bends sideways, so the strain runs along the layer lines (0.8 % when it
  opens).
- The model checks the fit: with the box in place nothing overlaps; moved 0.5 mm out, 0.5 mm
  further in, 0.5 mm up or 0.3 mm sideways, it collides with the tooth, the stop, the
  dovetail or the rail flanks. Set down 20.5 mm short of home it sits on the base with nothing
  overlapping and lifts off freely; 15 mm short it slides freely; 10 mm short it can no longer
  be lifted (both dovetails engaged).

**Fitting the box:** set the box flat on the base, 20.5 mm toward the ADXL end from its final
position (the box overhangs the base at the ADXL end), and slide it toward the USB end. The
tooth rides along the box side for the last 10 mm and clicks into the notch. **Removing:**
pull the grip at the end of the latch arm outward (away from the box), slide the box 20.5 mm
toward the ADXL end and lift it off.

**Fixing the base:** outdoor double-sided acrylic foam tape (VHB type) under the main area,
**not** under the latch arm and its root block (the arm must move freely). Clean and degrease
the bar first. For a moving awning, also use the 2 countersunk M4 screws or rivets
(`base_screws`): they sit flush in the two rail segments. Leave about 21 mm free on the ADXL
side of the base to slide the box on and off.

The old dovetail is still available with `mount_style = "dovetail"` (plate with the 6 mm
sled, `mount` with hose clamps, M3 lock screw).

## Fixing the GY-291: `adxl_pins`

The sensor is held by 2 snap pins instead of screws (`adxl_fix = "pins"`; `"screws"` brings
back the M2.5 pilot holes).

- Each pin goes through the GY-291 hole and its standoff into a hidden chamber in the plate.
  Two split prongs squeeze through the 2.9 mm hole and spring open under it. The 45 degree
  shoulder is set 0.1 mm tight, so the pin pulls the board down onto the standoffs and it
  cannot rattle (rattle would show up as false motion).
- The chamber is closed underneath, so the box back stays sealed.
- Printed lying on a flat, so the prongs bend along the layers. 4 are exported (2 spares).
- To remove one, pull firmly on the head with pliers. If it breaks, print another.
- **Measure** the hole in your GY-291 (`adxl_hole_d`, default 3.0 mm) and the hole spacing.

## Hardware list

Full shopping list, screw lengths and how to fit the heat-set inserts: [HARDWARE.md](HARDWARE.md).

- 4x M3 x 4 mm short heat-set inserts (cover)
- 4x M3 x 10 mm countersunk, DIN 7991 (cover, from the back; 6 mm was for the old 3 mm plate)
- Outdoor double-sided foam tape (VHB type) for `awning_base`, plus 2x M4 countersunk
  screws or rivets (recommended)
- Dovetail version only: 3x M3 standard inserts (2 hatch, 1 lock), 4x M3 x 10 mm cylinder
  head instead of the countersunk ones, 2x M3 x 8 mm (hatch), 1x M3 x 10 mm (lock), 2x
  stainless hose clamps
- `adxl_fix = "screws"` only: 2x M2.5 x 6 mm self-tapping (GY-291)
- TPU 90A filament for `gasket` and `usb_hatch` / `usb_plug` (or 2 mm silicone / EPDM cord, about 45 cm)
- O-ring buttons only: 2x O-ring 2.5 x 1 mm. Captive buttons: CA glue for the 2 collars
- `led_window = "hole"` only: 3 mm clear acrylic rod (about 4 mm long) or clear epoxy / UV resin

## Check before printing

- [ ] GY-291: `adxl_hole_d`, `adxl_hole_spacing`, `adxl_hole_inset`, `adxl_len`, `adxl_dep`
- [ ] Your 18650 length (`bat_l`)
- [ ] Your USB-C cable plug: overmold fits 13 x 7.5 mm (`usb_plug_w`, `usb_plug_h`)
- [ ] Print the `plate` first and test-fit the boards and cell before printing the cover

## Rendering

```
openscad -o stl/plate.stl -D 'part="plate"' enclosure.scad
```

Parts: `plate`, `cover`, `plunger`, `reset_plunger`, `usb_hatch`, `usb_plug`, `gasket`, `board_clips`, `usb_clamp`, `button_pin`, `reset_button_pin`, `button_collar`, `awning_base`, `adxl_pins`, `mount`, plus `assembly` (preview with ghost
boards) and `print_plate`. The STLs in `stl/` were rendered with OpenSCAD 2021.01 and
export as valid closed solids. `cover`, `plunger` and `reset_plunger` were re-exported for the
two-button / LED-window version. `gasket`, `usb_plug`, `board_clips`, `usb_clamp` and the captive button parts are new, `usb_hatch` was re-exported with the TPU
seal bead. `plate` and `awning_base` were re-exported for the rail mount (6.5 mm plate,
dovetail channel, latch notch, ADXL pin holes); `adxl_pins` is new; `mount` is only for the
dovetail version.
