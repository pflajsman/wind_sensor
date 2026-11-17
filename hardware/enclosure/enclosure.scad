// Awning protection sensor: parametric 3D-printable enclosure (OpenSCAD)
//
// Everything sits in one line:  [ADXL345] [ 18650 cell ] [ ESP32-H2 devkit | USB ]
//
// Construction: all electronics mount on a back `plate` (which slides onto the awning
// bracket). A one-piece `cover` goes over it from the front and is held by 4 screws driven
// in from the back, into heat-set inserts in the cover's end walls. The front stays clean
// (label, two button pockets, LED window) and the screws are only reachable with the box off the bracket.
// Ribs inside the cover press the ESP32 board and the cell down, so no clamp parts are needed.
//
// Parts (set `part`, F6, File > Export > STL):
//   plate      back plate: board mounts, battery holder, gasket groove, dovetail channel
//   cover      front shell: label, 2 sealed button plungers, LED window, USB port, hold-down ribs
//   plunger        user-button (IO25: pairing / factory reset) pin, finger pocket, 2.5 x 1 mm O-ring
//   reset_plunger  RST pin, recessed (press with a pen tip), 2.5 x 1 mm O-ring
//   usb_plug   TPU push-in stopper for the USB port, no screws
//   gasket     TPU 90A seal for the plate groove (replaces the 2 mm cord)
//   button_pin, reset_button_pin, button_collar  captive buttons: pin with a head in from the
//              front, collar glued onto its end from inside (alternative to the O-ring plungers)
//   board_clips  2 snap clips for the ESP32 board, printed lying down, pressed into plate pockets
//   usb_clamp  optional saddle on the two USB-C shells, pressed by the cover (not with esp_hold = clips)
//   awning_base  plate stuck (VHB tape) or screwed to the awning: a dovetail rail the box slides
//              onto, and a snap latch that clicks into the box side (mount_style = rail)
//   adxl_pins  plain press-fit pins that hold the GY-291 instead of screws (adxl_fix = pins)
//   mount      old dovetail bracket for hose clamps (mount_style = dovetail only)
//
// Board data: LaskaKit ESP32-H2-DEVKit V2 (rev 2.0, production v9), taken from the
// manufacturer's Gerber, pick-and-place and STEP files in specs/. GY-291 values are typical
// vendor figures (boards differ between makers), so measure yours. See ENCLOSURE.md.
// All dimensions in mm. Axes: X along the line, Y across, Z from back (bar side) to front;
// the plate/cover split is at z = floor_t.

/* [Output] */
part = "assembly"; // [assembly, print_plate, plate, cover, plunger, reset_plunger, usb_plug, gasket, board_clips, usb_clamp, button_pin, reset_button_pin, button_collar, awning_base, adxl_pins, mount]

/* [USB access] */
usb_ports = "uart"; // [uart, native, both]
// uart   = J4, CH9102F USB-UART: firmware console + flashing (sdkconfig: console on UART)
// native = J6, ESP32-H2 native USB Serial/JTAG. Both ports charge the cell.
usb_plug_w = 13;    // cable overmold width + clearance
usb_plug_h = 7.5;   // cable overmold height + clearance

/* [Buttons] */
user_button_extender = true; // plunger for the IO25 user button (pairing / factory reset)
user_recess = 1.0;     // user pin tip below the front surface, in a finger pocket
user_pocket_d = 9;
reset_recess = 2.5;    // RST pin tip deeper, in a small pocket: needs a pen tip, no accidental resets
reset_pocket_d = 5.5;
button_marks = true;   // engrave "PAIR" / "RST" next to the pockets
mark_size = 3;

// Button style. Both fit the same cover bores and pockets.
//   captive = PETG pin pushed in from the front, its head sits in the pocket; a collar is
//             glued onto the pin end from inside. Captive in the cover, moves freely, no seal yet
//   oring   = the original loose pin with a flange, sealed by a 2.5 x 1 mm O-ring
button_style = "captive"; // [captive, oring]
pin_travel = 0.8;      // head above the pocket floor at rest (0.4 gap to the switch + its travel)
pin_head_t = 1.0;
pin_head_clr = 0.4;    // head to pocket wall, per side
pin_stub_d = 3.0;      // thin end of the pin the collar slides onto
pin_reach = 0.4;       // stub sticks out this far below the collar (cover unchanged): pin end sits
                       // 0.4 - pin_reach above the switch at rest. Raise if presses don't click,
                       // lower if the button triggers by itself.
collar_d = 6.5;        // wider than the 4.3 bore: stops the pin coming out the front
collar_h = 1.5;
collar_clr = 0.2;      // collar hole over the stub (room for glue)

/* [Status LED] */
// The SK6812 sits 12 mm under the front, right beside the RST plunger boss, so the light is
// carried by a shrouded, slightly slanted channel to a window offset from the LED.
//   thin = channel stops led_skin under the front: glows through translucent / white / natural
//          PETG, fully sealed, nothing to assemble
//   hole = through-hole for a 3 mm clear acrylic rod or clear epoxy / UV resin (any colour cover)
//   none = no window
led_window = "thin";   // [thin, hole, none]
led_window_d = 4;      // light channel / window diameter
led_skin = 0.6;        // "thin": remaining front thickness over the window
led_rod_d = 3.1;       // "hole": bore for a 3 mm rod (glue it in to seal)
led_shift = [0, -4];   // window offset from the LED (box X, Y): away from the RST pocket
led_mark = true;       // engraved ring around the window so users know where to look

/* [ESP32 board retention] */
// The first snap hooks were part of the plate: 1.6 x 1.4 mm pillars printed upright, so they bent
// across the layer lines and broke. Now the 2 clips are separate parts printed LYING DOWN (the
// arm bends along the layers, the strong direction), with a longer arm that starts 3 mm below
// the floor, so it bends less. Each clip presses into a pocket in the plate (a drop of CA glue
// helps) and can be replaced if it ever breaks. The space under the board stays free for wiring.
// Fitting: slide the USB end under the fixed usb_bridge (over the two USB-C shells), then press
// the antenna end down until both clips click. The clips sit at the antenna end, on the free edge
// strip after the last header pad (pad edge at x 57.5; from the drill file).
clip_x = [57.0, 60.3];    // clip along the board edge (board-local x); the lip only on lip_x
clip_lip_x = [57.7, 60.1];
// hooks = 4 snap hooks moulded into the plate, 2 at the USB end + 2 at the antenna end. Each arm
//         starts 2 mm below the plate surface (in a moat in the thicker plate), so it is 13 mm
//         long and bends only ~0.8 %: the old 8 mm hooks bent ~3.4 % across the layers and broke.
// clips = 2 separate clips at the antenna end + a fixed bridge over the USB-C shells
esp_hold = "hooks";       // [hooks, clips]
// 2nd pair: mid-board (away from the wiring at the battery end), between the side fences, its
// beak in the 0.89 mm gap between header pads 38.885 and 41.425 (pad octagons 1.65 mm; drill file)
hook_x = [[4.0, 10.5], [37.0, 43.5]];    // arm span along the board edge (board-local x); wide = strong
hook_lip_x = [[7.6, 9.0], [39.855, 40.455]]; // beak only where the edge is free
hook_t = 1.4;             // arm thickness (flex direction); 1.2 cracked
hook_lip = 0.7;           // beak over the board edge; 0.5 held too little
hook_lip_h = 0.7;         // straight part of the beak face before the lead-in slope
hook_root = -2;           // box z where the arm starts (bottom of its moat)
hook_moat = 0.4;          // moat clearance around the arm
usb_bridge = esp_hold == "clips"; // rigid bridge of the plate over the USB-C shells
usb_bridge_x = [-0.4, 2.3]; // board-local x: from the shell fronts to 2.3 mm in
usb_bridge_clr = 0.2;     // above the shells at the front edge
usb_bridge_tilt = 4;      // underside rises this much (deg) inward: lets the board go in tilted
usb_bridge_t = 2.0;
clip_t = 1.2;            // arm thickness (flex direction)
clip_lip = 0.5;          // lip over the board edge
clip_fit = 0.0;          // pocket clearance per side: 0 = press fit; raise to 0.1 if too tight
usb_clamp_press = 0.3;   // clamp taller than the gap to the cover front = press on the shells

/* [Cable guides] */
// Arches on the floor beside the battery, in the lanes between the cell and the side walls, so
// the ADXL and battery wires stay low and cannot creep between the board and the cover. Wires
// press in through a slot at the top (or thread through). Then route them under the ESP board.
cable_guides = true;
cable_ch = [4.8, 2.6];   // channel width x height (plus a 45 deg roof up to the slot)
cable_slot = 1.8;        // entry slot at the top: wide enough that the halves hardly flex (1.2 cracked)
cable_w = 1.5;           // leg / roof thickness (was 1.2)
cable_guide_l = 5;       // length along the cable (was 3)
cable_guide_x = [4, 36, 65.5]; // from the battery holder start: clear of the end walls and clips
cable_cell_clr = 0.4;    // the guides are trimmed to stay this far under the cell

/* [TPU seals] */

// Both fit the grooves of the existing plate and cover; nothing in those two parts changes.
// gasket: sits in the plate groove (2.0 x 1.5). A wide base keeps it centred, a narrow ridge
// stands gasket_squeeze above the plate face and is flattened by the cover.
gasket_base_w = 1.8;   // groove is 2.0
gasket_base_h = 0.8;
gasket_ridge_w = 0.9;  // 2 extrusion lines
gasket_squeeze = 0.5;  // ridge above the plate face = compression. Lower it if the cover is hard to close
// usb_plug: TPU stopper pushed into the cover's USB tunnel, held by barbed ribs, plus a
// seal bead in the port groove.
plug_bead_w = 1.2;     // groove is 2.0 The cap stays within the flat end face of the box.
plug_stem_l = 5.5;     // into the 8.5 mm tunnel; the USB-C socket is at its far end
plug_clr = 0.2;        // stem body to tunnel wall, per side
plug_rib_int = 0.3;    // rib interference with the tunnel, per side. Higher = tighter
plug_wall = 1.2;       // hollow stem: flexes in, ribs seal
plug_squeeze = 0.2;    // seal bead compression when the plug is fully in

/* [GY-291 ADXL345: MEASURE] */
adxl_len = 20.3;        // along the 8-pin header (lies across the box, Y)
adxl_dep = 15.7;        // header edge to hole edge (lies along the box, X)
adxl_hole_spacing = 15; // MEASURE: centre distance of the 2 mounting holes
adxl_hole_inset = 2.5;  // MEASURE: hole centre to the board edge opposite the header
adxl_standoff = 3;
// pins  = press-fit pins through the GY-291 holes into the standoffs (adxl_pins, printed head down)
// screws = 2 x M2.5 self-tapping screws into the standoffs
adxl_fix = "pins";      // [pins, screws]
adxl_hole_d = 3.0;      // MEASURE: GY-291 mounting hole diameter
adxl_pcb_t = 1.6;

/* [18650 cell] */
bat_d = 18.8;      // cell diameter + clearance (cradle)
bat_clip_d = 18.3; // clip inner diameter, a bit tight so the cell cannot rattle
bat_l = 70;        // ~65 bare cell, up to ~70 for a protected cell; 66 saves 4 mm of length

/* [Shell] */
wall = 3.2;        // side walls (gasket groove sits under them)
end_wall = 8.5;    // end walls hold the 4 cover screws and the USB plug tunnel
floor_t = 3;       // plate thickness above the sled
front_t = 3;       // cover front face
cover_bevel = 1.5; // 45 degree chamfer on the cover's upper (front) outer edges; 0 = sharp
margin = 1;        // ADXL to the -X wall
gap_sensor = 3;    // ADXL header to battery holder
gap_antenna = 6;   // battery holder to the ESP32 antenna edge (stop blocks sit in this gap)
stop_t = 3.0;      // antenna-end stop blocks: thickness at the top (was 2.0)
stop_t_base = 4.5; // ... and at the floor (buttress toward the battery)
inner_y = 30.4;    // 25.4 board + side fences + room for the snap hooks to flex
gasket_w = 2.0;    // groove for 2 mm silicone / EPDM cord
gasket_depth = 1.5;
insert_d = 4.0;    // M3 heat-set insert hole (2.6 for M3 self-tapping)
insert_len = 4.5;  // short M3 x 3 / x 4 inserts (cover screws must stay under the USB tunnel)
// cover screw heads in the plate back (rail mount):
//   cylinder    = low cylinder head ISO 14580 (CNC Kitchen M3 x 6, head 5.5 x 2.4) in a counterbore
//   countersunk = DIN 7991 M3 x 10, flush in a countersink
cover_screw_head = "cylinder"; // [cylinder, countersunk]
cbore_d = 6.2;     // counterbore for the 5.5 mm head
cbore_depth = 3.5; // from the back face: leaves 3.0 mm under the head, so M3 x 6 reaches 3.0 mm into the cover
pilot_m25 = 2.1;   // M2.5 self-tapping pilot
press = 0.3;       // interference of the cover ribs on the ESP32 board

/* [Label] */
label = "FlajsmanLab";  // engraved in the centre of the front; "" for none
label_size = 9;         // cap height, mm
label_depth = 0.6;
label_font = "Liberation Sans:style=Bold";

/* [Mount] */
// rail     = the plate is back_t thicker underneath (stiff, so the gasket seals along the long
//            sides) with a dovetail channel in its back. awning_base (stuck with VHB tape or
//            screwed to the awning) carries the matching rail and a snap latch: slide the box on
//            from the ADXL end until it clicks; pull the latch grip outward to slide it off.
// dovetail = the original 6 mm sled under the plate + `mount` with hose clamps
mount_style = "rail";  // [rail, dovetail]
back_t = 3.5;          // rail: extra plate thickness under the box floor
base_t = 2.0;          // awning_base thickness under the box
rail_h = 2.0;          // dovetail depth in the plate back
rail_neck = 10;        // dovetail width at the plate back face (clear of the cover screw heads)
rail_flare = 45;       // flank angle from vertical: 45 deg = 2 mm hook per side, prints without support
rail_clr = 0.15;       // per flank. Lower = less play, higher = slides easier
rail_stop = 16;        // ADXL-end channel closed this far from the -X end (the end stop); clear of the ADXL pin chambers
rail_seg = 20;         // two short dovetail segments, one at each end. Behind the ADXL-end one the plate has
                       // an opening: set the box down rail_travel toward the ADXL end, slide it to the USB end
latch_len = 25;        // spring arm length (root to tooth)
latch_t = 2.0;         // spring arm thickness (flexes sideways, along the layers)
latch_tooth = 1.5;     // tooth depth into the notch in the plate side
latch_x = 10;          // lock face this far from the USB end (clear of the cover screw head)
base_screws = true;    // 2 countersunk holes for M4 screws / rivets (or use tape only)
mount_len = 70;
sled_h = mount_style == "dovetail" ? 6 : 0; // block under the plate holding the dovetail channel
back_h = mount_style == "dovetail" ? sled_h : back_t; // plate material below the box floor (z < 0)
dt_h = 3.5;        // dovetail depth
dt_neck = 10;      // dovetail neck width at the deep (narrow) end
dt_taper = 1.0;    // extra neck width at the entry end: wedges tight when fully slid in
dt_clr = 0.2;
lock_z = 0;        // lock screw height (box z): just above the dovetail channel roof
strap_w = 10;      // stainless hose clamp up to 9 mm or zip tie up to 8 mm
strap_t = 2.2;

$fn = 48;

// ======================================================================= ESP32 board data
// Local board frame (from the Gerber/STEP): x 0..60.33 (USB end at 0, antenna end at 60.33),
// y -12.7..12.7, z 0 = PCB bottom. Placed rotated 180 deg so the USB end faces the +X wall.
pcb_l = 60.33;
pcb_w = 25.4;
pcb_t = 1.6;
usb_overhang = 0.43;   // USB-C shells stick out past the PCB edge
usb_y = [6.35, -6.35]; // J4 (UART), J6 (native)
usb_zc = 2.8;          // receptacle centre above PCB bottom
reset_xy = [30.66, -8.26];
user_btn_xy = [30.66, 8.26];
led_xy = [34.72, -7.39]; // SK6812-EC20 (LED_IO22), 2 x 2 mm, pick-and-place centre
btn_top = 3.57;        // button actuator top above PCB bottom
esp_gap_below = 6.5;   // air gap under the PCB; lifts the USB port above the cover screws and split line
press_x = 40.6;        // free strip across the board between J5 (to 37.7) and the module (from 43.6)

// ======================================================================= derived layout
yc = inner_y / 2;
bat_end_t = 1.6;
bat_lift = 1;

x_s0 = margin;                                  // ADXL zone
x_b0 = x_s0 + adxl_dep + gap_sensor;            // battery holder
x_b1 = x_b0 + bat_l + 2 * bat_end_t;
esp_x0 = x_b1 + gap_antenna + pcb_l;            // box X of the PCB's USB edge (local x = 0)
inner_x = esp_x0 + usb_overhang + 0.2;

bat_zc = floor_t + bat_lift + bat_d / 2;
esp_z0 = floor_t + esp_gap_below;
inner_h = bat_lift + bat_d + 1;
front_z = floor_t + inner_h;                    // inner face of the front
top_z = front_z + front_t;

usb_sel = usb_ports == "uart" ? [usb_y[0]] : usb_ports == "native" ? [usb_y[1]] : usb_y;
port_yc = yc - (usb_sel[0] + usb_sel[len(usb_sel) - 1]) / 2; // box Y of the port centre
port_w = usb_plug_w + abs(usb_sel[0] - usb_sel[len(usb_sel) - 1]);
port_z = esp_z0 + usb_zc;
port_seal = 2;                                  // groove offset from the port opening

function esp(p) = [esp_x0 - p[0], yc - p[1]]; // board local -> box XY

// [board xy, recess below the front, pocket diameter, mark]
reset_btn = [reset_xy, reset_recess, reset_pocket_d, "RST"];
user_btn = [user_btn_xy, user_recess, user_pocket_d, "PAIR"];
buttons = user_button_extender ? [reset_btn, user_btn] : [reset_btn];
plunger_bot = esp_z0 + btn_top + 0.4;          // 0.4 mm above the button at rest
boss_bot = plunger_bot + 1.5;                   // flange sits under the boss
oring_z = front_z - 3;

// LED light channel: from just above the LED (clear of the RST button and J5) to the window.
led_box = esp(led_xy);
led_win = led_box + led_shift;
led_ch_bot = boss_bot + 1;                      // channel bottom, ~4.9 mm above the PCB top
led_bot = led_box + [0, -1.5];                  // channel bottom, nudged off the RST bore
led_bot_d = 2.5;                                // bottom aperture (the LED is 2 x 2 mm)
led_wall = 1.2;

// cover screws: 4, in the end walls, outside the gasket loop
cover_screws = [for (x = [-end_wall + 4, inner_x + end_wall - 4], y = [3, inner_y - 3]) [x, y]];

echo(str("Outer size: ", inner_x + 2 * end_wall, " x ", inner_y + 2 * wall, " x ",
         back_h + top_z, " mm (+2.5 mm USB plug cap)", mount_style == "rail" ? str(", ", back_h + top_z + base_t, " mm with awning_base") : ""));
// LED channel must not cut into the RST plunger bore (r 2.15) or its pocket
assert(norm(led_win - esp(reset_xy)) - led_window_d / 2 - 2.15 >= 1.0, "LED window too close to the RST bore");
assert(norm(led_bot - esp(reset_xy)) - led_bot_d / 2 - 2.15 >= 1.0, "LED channel bottom too close to the RST bore");
assert(norm(led_win - esp(reset_xy)) - led_window_d / 2 - reset_pocket_d / 2 >= 1.0, "LED window too close to the RST pocket");
assert(port_z - usb_plug_h / 2 - port_seal - gasket_w > floor_t + 0.5, "USB seal crosses the split line");
// TPU does not compress in volume: once squeezed, a seal must still fit in its groove
gasket_ridge_h = gasket_depth + gasket_squeeze - gasket_base_h;
gasket_fill = (gasket_base_w * gasket_base_h + gasket_ridge_w * gasket_ridge_h) / (gasket_w * gasket_depth);
bead_fill = plug_bead_w * (gasket_depth + plug_squeeze) / (gasket_w * gasket_depth);
echo(str("TPU gasket groove fill ", round(gasket_fill * 100), " %, USB plug bead fill ", round(bead_fill * 100), " %"));
assert(gasket_fill <= 0.9 && bead_fill <= 0.9, "TPU seal overfills its groove when squeezed");
assert(gasket_base_w < gasket_w && gasket_ridge_w <= gasket_base_w && gasket_ridge_h > 0, "TPU gasket profile");

// ======================================================================= 2D helpers

module cavity2d() { square([inner_x, inner_y]); }

module outline2d() {
    hull() {
        translate([-end_wall + 2, -wall + 2]) offset(r = 2) square([inner_x + 2 * end_wall - 4, inner_y + 2 * wall - 4]);
    }
}

// band of width w on the groove centre line (wall / 2 outside the cavity)
module band2d(w) {
    difference() {
        offset(r = wall / 2 + w / 2) cavity2d();
        offset(r = wall / 2 - w / 2) cavity2d();
    }
}
module groove2d() { band2d(gasket_w); }

// USB port groove ring (in the cover end face) in the port's own 2D frame: u = box Y, v = box Z
module port_ring2d(w) {
    c = port_seal + gasket_w / 2;   // groove centre line, outside the port opening
    difference() {
        rrect(port_w + 2 * c + w, usb_plug_h + 2 * c + w, c + w / 2);
        rrect(port_w + 2 * c - w, usb_plug_h + 2 * c - w, c - w / 2);
    }
}

module rrect(w, h, r) { offset(r = r) square([w - 2 * r, h - 2 * r], center = true); }

// Dovetail cross-section in the YZ plane: neck width wn at z = 0, flaring 30 deg per side.
module dove2d(wn, h, clr) {
    t = tan(30);
    polygon([[-wn / 2 - clr, 0], [wn / 2 + clr, 0],
             [wn / 2 + h * t + clr, h], [-wn / 2 - h * t - clr, h]]);
}

// Tapered dovetail between two X positions (x0 gets neck w0, x1 gets neck w1).
module dove_taper(x0, x1, w0, w1, h, clr) {
    hull() {
        translate([x0, 0, 0]) rotate([90, 0, 90]) linear_extrude(0.01) dove2d(w0, h, clr);
        translate([x1, 0, 0]) rotate([90, 0, 90]) linear_extrude(0.01) dove2d(w1, h, clr);
    }
}

// ======================================================================= plate internals

// ADXL pin holes in the plate (sized for the old split snap pin; kept so printed plates still fit).
apin_d = adxl_hole_d - 0.2;          // old snap-pin shaft, sets the hole below
apin_neck = apin_d + 0.1;            // hole through the standoff
apin_neck_l = 1.2;                   // hole length in the floor below the standoff
apin_barb = apin_neck + 0.7;         // barb width (squeezes 0.35 per side through the neck)
apin_grip = adxl_pcb_t + adxl_standoff + apin_neck_l; // head to the chamber ceiling
apin_cham = [apin_barb + 0.8, 2.6];  // hidden chamber under the neck (d, h)
// Plain press-fit pin (no split): solid round shaft through the GY-291 hole into the 2.9 mm
// standoff hole. FDM prints the vertical hole a little undersize, so the shaft is a light press
// fit and the head clamps the board onto the standoff. The tip stops 0.2 mm above the bottom of
// the hole so the head always seats on the board. The plate holes above are unchanged.
adxl_pin_test_d = 2.9;               // shaft for part = adxl_pins_test (test set export)
apin_shaft_d = 2.9;                  // MEASURE/TUNE: 2.8 if it will not go in, 3.0 if it falls out
apin_len = adxl_pcb_t + adxl_standoff + apin_neck_l - 0.2;   // head underside to tip
apin_head = [6, 1.2];                // head (d, h)
module adxl_pin(d = apin_shaft_d) {   // head down, shaft up (print orientation)
    cylinder(d = apin_head[0], h = apin_head[1]);
    translate([0, 0, apin_head[1] - 0.01]) {
        cylinder(d = d, h = apin_len - 0.4 + 0.01);
        translate([0, 0, apin_len - 0.4]) cylinder(d1 = d, d2 = d - 0.8, h = 0.4);   // lead-in chamfer
    }
}
module adxl_pins_print(d = apin_shaft_d) {   // standing head down, 4 pins (2 + 2 spares)
    for (i = [0 : 3]) translate([i * 8, 0, 0]) adxl_pin(d);
}
assert(apin_cham[1] >= 2.0 + 0.4, "ADXL pin chamber too short for the barbs");
assert(adxl_fix != "pins" || mount_style != "rail" || -end_wall + rail_stop > x_s0 + adxl_hole_inset + apin_cham[0] / 2 + 1,
       "rail channel runs under the ADXL pin chambers");

module adxl_holes() {   // box frame: pin holes + hidden chambers (cut from the plate)
    if (adxl_fix == "pins") for (dy = [-1, 1])
        translate([x_s0 + adxl_hole_inset, yc + dy * adxl_hole_spacing / 2, 0]) {
            translate([0, 0, floor_t - apin_neck_l - 0.01]) cylinder(d = apin_neck, h = apin_neck_l + adxl_standoff + 1);
            translate([0, 0, floor_t - apin_neck_l - apin_cham[1]]) cylinder(d = apin_cham[0], h = apin_cham[1]);
        }
}

module adxl_mount() {
    hx = x_s0 + adxl_hole_inset;
    for (dy = [-1, 1]) translate([hx, yc + dy * adxl_hole_spacing / 2, floor_t - 0.01])
        difference() {
            cylinder(d = adxl_fix == "pins" ? 6 : 5.5, h = adxl_standoff);
            if (adxl_fix == "screws") cylinder(d = pilot_m25, h = 10);
            else translate([0, 0, -1]) cylinder(d = apin_neck, h = adxl_standoff + 2);
        }
    // support rib under the header side so the breakout cannot vibrate on its own
    translate([x_s0 + adxl_dep - 5, yc - adxl_len / 2 + 2, floor_t - 0.01])
        cube([2, adxl_len - 4, adxl_standoff]);
}

usb_shell = [6.5, 9, 4.07];                       // board-local x from -usb_overhang, y, z from 0.77
usb_shell_top = esp_z0 + 0.77 + usb_shell[2];
// board-local frame (x from the USB edge, y across, z = box z)
usb_bridge_post_y = [pcb_w / 2 + 0.25, inner_y / 2 - 0.2];
function usb_bridge_zu(x) = usb_shell_top + usb_bridge_clr + max(0, x - usb_bridge_x[0]) * tan(usb_bridge_tilt);
usb_bridge_top = usb_bridge_zu(usb_bridge_x[1]) + usb_bridge_t;
module usb_bridge_local() {
    x0 = usb_bridge_x[0]; x1 = usb_bridge_x[1];
    for (s = [-1, 1]) translate([x0, s > 0 ? usb_bridge_post_y[0] : -usb_bridge_post_y[1], floor_t - 0.01])
        cube([x1 - x0, usb_bridge_post_y[1] - usb_bridge_post_y[0], usb_bridge_top - floor_t + 0.01]);
    translate([0, usb_bridge_post_y[1], 0]) rotate([90, 0, 0]) linear_extrude(2 * usb_bridge_post_y[1])
        polygon([[x0, usb_bridge_zu(x0)], [x1, usb_bridge_zu(x1)], [x1, usb_bridge_top], [x0, usb_bridge_top]]);
}
assert(!usb_bridge || usb_bridge_top < front_z - 1, "usb_bridge reaches the cover front");

module battery_holder() {
    clip_t = 1.8;
    clip_w = 8;
    open_deg = 120;              // opening at the top of each clip
    ro = bat_clip_d / 2 + clip_t;
    // end walls with a notch for the cell tabs / leads
    for (x = [x_b0, x_b1 - bat_end_t]) translate([x, yc - bat_d / 2 - 1, floor_t - 0.01])
        difference() {
            cube([bat_end_t, bat_d + 2, bat_lift + bat_d * 0.6]);
            translate([-1, (bat_d + 2) / 2 - 3, bat_lift + bat_d / 2 - 2]) cube([bat_end_t + 2, 6, 20]);
        }
    // cradle rail along the bottom
    translate([x_b0, yc - 4, floor_t - 0.01]) difference() {
        cube([x_b1 - x_b0, 8, bat_lift + 2]);
        translate([-1, 4, bat_lift + bat_d / 2 + 0.01]) rotate([0, 90, 0]) cylinder(d = bat_d, h = bat_l + 10);
    }
    // two snap clips
    for (f = [0.25, 0.75]) translate([x_b0 + bat_end_t + f * bat_l - clip_w / 2, yc, bat_zc])
        difference() {
            union() {
                rotate([0, 90, 0]) cylinder(r = ro, h = clip_w);
                translate([0, -ro * 0.7, -(bat_zc - floor_t + 0.01)]) cube([clip_w, 1.4 * ro, bat_zc - floor_t]);
            }
            translate([-1, 0, 0]) rotate([0, 90, 0]) cylinder(d = bat_clip_d, h = clip_w + 2);
            translate([-1, 0, 0]) rotate([0, 90, 0]) rotate([0, 0, 180 - open_deg / 2])
                rotate_extrude(angle = open_deg) square([ro + 1, clip_w + 2]);
        }
}

module esp_mount() {
    top = esp_z0 + pcb_t;
    // support posts under the PCB (2 per line, 3.5..7.5 mm off the centre line): the centre, the
    // edges (header pads) and the gaps between the posts stay free for wiring under the board
    for (lx = [8.0, press_x, 58.0], s = [-1, 1]) {
        p = esp([lx, s * 5.5]);
        translate([p[0] - 1, p[1] - 2, floor_t - 0.01]) cube([2, 4, esp_gap_below + 0.01]);
    }
    // side fences locate the PCB edges (2 mm thick, up to 0.25 mm from the cavity wall)
    for (r = [[18, 26], [46, 56]], s = [-1, 1]) {
        a = esp([r[1], 0]);
        translate([a[0], yc + s * (pcb_w / 2 + 0.25) - (s < 0 ? 2 : 0), floor_t - 0.01])
            cube([r[1] - r[0], 2, top - floor_t]);
    }
    // fixed bridge over the two USB-C shells, on 2 posts beside the board edges
    if (usb_bridge) translate([esp_x0, yc, 0]) rotate([0, 0, 180]) usb_bridge_local();
    // stop blocks beyond the antenna edge (outside the module's |y| < 6.6). 0.1 mm from the
    // antenna edge: locates the board in X for the beaks. stop_t thick at the top, buttressed
    // to stop_t_base at the floor toward the battery (2 mm blocks cracked at the root).
    for (s = [-1, 1]) {
        a = esp([pcb_l + 0.1, 0]);
        translate([a[0], s > 0 ? yc + 8 : yc - pcb_w / 2, floor_t - 0.01]) hull() {
            translate([-stop_t, 0, 0]) cube([stop_t, pcb_w / 2 - 8, top + 1 - floor_t]);
            translate([-stop_t_base, 0, 0]) cube([stop_t_base, pcb_w / 2 - 8, 1]);
        }
    }
}

// ---- cable guides. Profile in (u, z): u = inward from the side wall (0.3 mm gap), z = above floor.
module cable_guide_profile() {
    w = cable_w;                                  // leg / roof thickness
    r = (cable_ch[0] - cable_slot) / 2;           // 45 deg roof run from each leg to the slot
    top = cable_ch[1] + r + 0.85;
    for (m = [0, 1]) translate([m * (cable_ch[0] + 2 * w), 0]) mirror([m, 0])
        polygon([[0, 0], [w, 0], [w, cable_ch[1]], [w + r, cable_ch[1] + r], [w + r, top], [0, top]]);
}
module cable_guides_all() {
    difference() {
        for (x = cable_guide_x, s = [-1, 1])
            multmatrix([[1, 0, 0, x_b0 + x], [0, -s, 0, s > 0 ? inner_y - 0.3 : 0.3], [0, 0, 1, floor_t - 0.01], [0, 0, 0, 1]])
                rotate([90, 0, 90]) linear_extrude(cable_guide_l) cable_guide_profile();
        // keep clear of the cell lying above the lanes
        translate([x_b0 - 1, yc, bat_zc]) rotate([0, 90, 0]) cylinder(d = bat_d + 2 * cable_cell_clr, h = bat_l + 10);
    }
}

// ---- board clips. Profile in (u, z): u = outward from the PCB edge, z = above the plate floor.
clip_zt = esp_gap_below + pcb_t + 0.15;          // lip underside: 0.15 mm above the PCB top
// with the sled the clip block sits 6 mm deep and the arm starts 3 mm below the floor; with the
// thin base plate the pocket is 2 mm deep (1 mm floor left) and the arm starts at the floor
clip_bot = sled_h > 0 ? -6 : -2;
clip_root = sled_h > 0 ? -3 : 0;
module clip_profile() {
    polygon([[-1.0, clip_bot], [2.3, clip_bot], [2.3, clip_root], [0.2 + clip_t, clip_root],
             [0.2 + clip_t, clip_zt + 2.0], [-clip_lip, clip_zt + 0.4], [-clip_lip, clip_zt],
             [0.2, clip_zt], [0.2, clip_root], [-1.0, clip_root]]);
}
// clip in board-local x, u, z: block (in the pocket) + arm over clip_x, lip only over clip_lip_x
module clip_local() {
    translate([clip_x[0], 0, 0]) rotate([90, 0, 90]) linear_extrude(clip_x[1] - clip_x[0])
        intersection() {
            clip_profile();
            union() { translate([0.2, -10]) square([10, 40]); translate([-5, -10]) square([10, 10 + clip_root + 0.01]); }
        }
    translate([clip_lip_x[0], 0, 0]) rotate([90, 0, 90]) linear_extrude(clip_lip_x[1] - clip_lip_x[0])
        intersection() { clip_profile(); translate([-5, clip_root + 1]) square([5.21, 30]); }
}
module clip_place(s) multmatrix([[-1, 0, 0, esp_x0], [0, s, 0, yc + s * pcb_w / 2], [0, 0, 1, floor_t], [0, 0, 0, 1]]) children();
module board_clip(s) clip_place(s) clip_local();
module clip_pocket(s) clip_place(s)
    translate([clip_x[0] - clip_fit, -1.0 - clip_fit, clip_bot]) cube([clip_x[1] - clip_x[0] + 2 * clip_fit, 3.3 + 2 * clip_fit, -clip_bot + 1]);
// both clips for printing: lying on the side (profile flat on the bed, so the arm bends along
// the layers), lip end on the bed. The two clips are mirror images (one per board edge).
module clip_local_print() multmatrix([[0, 1, 0, 0], [0, 0, 1, -clip_bot], [-1, 0, 0, clip_x[1]], [0, 0, 0, 1]]) clip_local();
module board_clips_print() {
    clip_local_print();
    translate([-6, 0, 0]) mirror([1, 0, 0]) clip_local_print();
}
assert(2.3 + clip_fit <= (inner_y - pcb_w) / 2 - 0.1, "clip pocket reaches the cavity wall / gasket groove");
assert(floor_t + sled_h + clip_bot >= 1, "clip pocket leaves less than 1 mm of floor");

// ---- moulded snap hooks. Profile in (u, z): u = outward from the PCB edge, z = box z.
hook_zt = floor_t + clip_zt;                     // beak underside, 0.15 mm above the PCB top
hook_top = hook_zt + 2.4;
hook_len = hook_zt - hook_root;                  // flexing length
hook_strain = 3 * hook_t * (hook_lip + 0.2) / (2 * hook_len * hook_len);
module hook_profile(lip = true) {
    if (lip) polygon([[0.2, hook_root], [0.2 + hook_t, hook_root], [0.2 + hook_t, hook_top],
                      [-hook_lip, hook_zt + hook_lip_h], [-hook_lip, hook_zt], [0.2, hook_zt]]);
    else polygon([[0.2, hook_root], [0.2 + hook_t, hook_root], [0.2 + hook_t, hook_top], [0.2, hook_top - 1.2]]);
}
// board-local x, u, box z -> box frame (u outward on side s)
module hook_place(s) multmatrix([[-1, 0, 0, esp_x0], [0, s, 0, yc + s * pcb_w / 2], [0, 0, 1, 0], [0, 0, 0, 1]]) children();
module hook_local(i) {
    translate([hook_x[i][0], 0, 0]) rotate([90, 0, 90]) linear_extrude(hook_x[i][1] - hook_x[i][0]) hook_profile(false);
    translate([hook_lip_x[i][0], 0, 0]) rotate([90, 0, 90]) linear_extrude(hook_lip_x[i][1] - hook_lip_x[i][0]) hook_profile(true);
}
module hooks_all() for (i = [0, 1], s = [-1, 1]) hook_place(s) hook_local(i);
module hook_moats() for (i = [0, 1], s = [-1, 1]) hook_place(s)
    translate([hook_x[i][0] - hook_moat, 0.2 - hook_moat, hook_root]) cube([hook_x[i][1] - hook_x[i][0] + 2 * hook_moat, hook_t + 2 * hook_moat, floor_t - hook_root + 1]);
echo(str("ESP32 hooks: ", round(hook_len * 10) / 10, " mm arm, bending strain ", round(hook_strain * 1000) / 10, " %"));
assert(esp_hold != "hooks" || hook_root - (-back_h) >= 1.5, "hook moat leaves less than 1.5 mm under it");
assert(esp_hold != "hooks" || 0.2 + hook_t + hook_lip + 0.2 <= (inner_y - pcb_w) / 2, "hook cannot open: too close to the cavity wall");
// screw tip in the cover = what passes the split line; must stay inside the insert hole
cover_screw_l = cover_screw_head == "cylinder" ? 6 : 10;   // ISO 14580 length excludes the head; DIN 7991 includes it
cover_screw_in = cover_screw_l - (cover_screw_head == "cylinder" ? back_h + floor_t - cbore_depth : back_h + floor_t);
echo(str("Cover screw M3 x ", cover_screw_l, ": ", cover_screw_in, " mm into the cover (insert hole ", insert_len, " mm)"));
assert(mount_style != "rail" || (cover_screw_in >= 2.5 && cover_screw_in <= insert_len - 0.5), "cover screw length does not suit the insert hole");
assert(stop_t_base + 0.1 <= gap_antenna - 1, "antenna stop blocks reach the battery holder");
assert(cable_guide_x[0] >= bat_end_t + 0.5 && cable_guide_x[2] + cable_guide_l <= bat_l + bat_end_t - 0.5, "cable guides hit the battery end walls");

// ---- rail mount. Dovetail profile in the YZ plane: width wn at z = 0, widening upward.
module rail2d(wn, h, clr) {
    t = tan(rail_flare);
    polygon([[-wn / 2 - clr, 0], [wn / 2 + clr, 0], [wn / 2 + h * t + clr, h], [-wn / 2 - h * t - clr, h]]);
}
seg_a = [-end_wall + rail_stop, -end_wall + rail_stop + rail_seg];   // ADXL-end segment (box x)
seg_b = [inner_x + end_wall - rail_seg, inner_x + end_wall];          // USB-end segment, open at the end face
rail_open_l = rail_seg + 1;                                          // drop-in opening behind seg_a
rail_travel = rail_seg + 0.5;                                        // slide from drop-in to locked
rail_top_w = rail_neck + 2 * rail_h * tan(rail_flare);
side_y = inner_y + wall;                                // +Y outer face of the box
latch_x1 = inner_x + end_wall - latch_x;                // lock face (tooth's +X face)
latch_ramp = 2.5;                                       // lead-in on the tooth's -X side
latch_flat = 1.5;
latch_gap = 0.4;                                        // arm to the box side
latch_ztop = -0.8;                                      // arm top (box z)
module rail_channel() {   // cut from the plate: open at the USB end
    translate([seg_a[0], yc, -back_h - 0.01]) rotate([90, 0, 90])
        linear_extrude(rail_seg + 0.01) rail2d(rail_neck, rail_h + 0.01, rail_clr);
    // drop-in opening: as wide as the rail top, so the ADXL-end rail comes straight down through it
    translate([seg_a[1], yc - rail_top_w / 2 - 0.4, -back_h - 0.01]) cube([rail_open_l, rail_top_w + 0.8, rail_h + 0.01]);
    // USB-end segment starts 0.5 before the rail, so only the ADXL end is the stop
    translate([seg_b[0] - 0.5, yc, -back_h - 0.01]) rotate([90, 0, 90])
        linear_extrude(rail_seg + 1.5) rail2d(rail_neck, rail_h + 0.01, rail_clr);
}
module latch_notch() {    // cut from the plate's +Y side
    translate([latch_x1 - latch_flat - latch_ramp - 0.3, side_y - latch_tooth - 0.2, -back_h - 1])
        cube([latch_flat + latch_ramp + 0.3 + 0.1, latch_tooth + 1.2, back_h + 1 + latch_ztop + 0.3]);
}
// base in box coordinates: its top is the box bottom (z = -back_h)
module awning_base() {
    z0 = -back_h - base_t;
    ay = side_y + latch_gap;                            // arm inner face
    ax0 = latch_x1 - latch_len;                         // arm root
    difference() {
        union() {
            translate([0, 0, z0]) linear_extrude(base_t) difference() {
                outline2d();
                // free the arm: the base stays 1 mm clear of it
                translate([ax0, side_y - 0.6]) square([200, 10]);
            }
            // rail
            for (g = [seg_a, seg_b]) translate([g[0] + 0.1, yc, -back_h - 0.01]) rotate([90, 0, 90])
                linear_extrude(g[1] - g[0] - 0.1) rail2d(rail_neck, rail_h - 0.3 + 0.01, 0);
            // latch: root block, spring arm, tooth, grip
            translate([ax0 - 8, ay, z0]) cube([8, latch_t, latch_ztop - z0]);
            translate([ax0 - 8, side_y - 0.6, z0]) cube([8, ay - side_y + 0.61, base_t]);
            translate([ax0 - 0.01, ay, z0]) cube([latch_len + 5, latch_t, latch_ztop - z0]);
            translate([0, 0, -back_h + 0.3]) linear_extrude(latch_ztop - (-back_h + 0.3))
                polygon([[latch_x1, ay + 0.01], [latch_x1, ay - latch_tooth], [latch_x1 - latch_flat, ay - latch_tooth],
                         [latch_x1 - latch_flat - latch_ramp, ay + 0.01]]);
            translate([latch_x1 + 1, ay + latch_t - 0.01, z0]) cube([4, 2.5, latch_ztop - z0]);
        }
        if (base_screws) for (x = [(seg_a[0] + seg_a[1]) / 2, (seg_b[0] + seg_b[1]) / 2]) translate([x, yc, z0 - 1]) {
            cylinder(d = 4.5, h = 20);
            translate([0, 0, 1 + base_t + rail_h - 0.3 - 2.3]) cylinder(d1 = 4.5, d2 = 8.6, h = 2.31);
        }
    }
}
module awning_base_print() translate([0, 0, back_h + base_t]) awning_base();
// arm strain when the tooth is pushed out (3 t d / 2 L^2), keep well under PETG's ~4 %
echo(str("awning_base latch arm strain ", round(1000 * 3 * latch_t * (latch_tooth + 0.1) / (2 * latch_len * latch_len)) / 10, " %"));
assert(3 * latch_t * (latch_tooth + 0.1) / (2 * latch_len * latch_len) < 0.015, "latch arm strain too high");
// countersink cone (r 4.2 at the back face, 45 deg) narrows as the channel widens, so the
// gap stays as at the back face; at the channel top check the plain screw hole (r 1.7)
// with the box set down rail_travel short of home: the ADXL rail sits in the opening and the USB
// rail is still outside the box
assert(seg_a[0] + 0.1 + rail_travel >= seg_a[1] && seg_a[1] + rail_travel <= seg_a[1] + rail_open_l
       && seg_b[0] + 0.1 > seg_b[1] - rail_travel, "rail segments: drop-in does not work");
assert(rail_neck / 2 + rail_clr < yc - 3 - 4.2 - 1 && rail_flare <= 45
       && (cover_screw_head != "cylinder" || rail_neck / 2 + rail_h * tan(rail_flare) + rail_clr < yc - 3 - cbore_d / 2 - 1)
       && rail_neck / 2 + rail_h * tan(rail_flare) + rail_clr < yc - 3 - 1.7 - 1, "rail channel hits the cover screws");

// ======================================================================= parts

module plate() {
    difference() {
        translate([0, 0, -back_h]) linear_extrude(back_h + floor_t) outline2d();
        translate([0, 0, floor_t - gasket_depth]) linear_extrude(gasket_depth + 1) groove2d();
        if (sled_h > 0) {
            // cover screws from the back, heads recessed in the sled
            for (s = cover_screws) translate([s[0], s[1], -sled_h - 1]) {
                cylinder(d = 3.4, h = sled_h + floor_t + 2);
                cylinder(d = 5.8, h = 3 + 1);
            }
            // tapered dovetail channel, open at the -X end
            translate([0, yc, -sled_h - 0.01]) dove_taper(-end_wall - 1, -end_wall + mount_len + 0.5,
                dt_neck + dt_taper * (mount_len + 1) / mount_len, dt_neck - dt_taper * 0.5 / mount_len,
                dt_h + 0.01, dt_clr);
            // lock-screw insert in the sled end face
            translate([-end_wall - 0.01, yc, lock_z]) rotate([0, 90, 0]) cylinder(d = insert_d, h = 5);
        } else {
            // cover screws from the back: low cylinder heads in counterbores, or countersunk
            // M3 (DIN 7991) flush with the back
            for (s = cover_screws) translate([s[0], s[1], -back_h - 1]) {
                cylinder(d = 3.4, h = back_h + floor_t + 2);
                if (cover_screw_head == "cylinder") cylinder(d = cbore_d, h = cbore_depth + 1);
                else cylinder(d1 = 6.4 + 2 * 1, d2 = 3.4, h = 1.5 + 1);
            }
            rail_channel();
            latch_notch();
        }
        // pockets for the board clips
        if (esp_hold == "clips") for (s = [-1, 1]) clip_pocket(s);
        else hook_moats();
        adxl_holes();
    }
    if (esp_hold == "hooks") hooks_all();
    adxl_mount();
    battery_holder();
    esp_mount();
    if (cable_guides) cable_guides_all();
}

// Slanted column from above the LED (led_ch_bot) up to the window under the front.
// skin > 0: stop that far below the outer face; skin = 0: stop at the inner face of the front.
module led_channel(d_top, d_bot, skin) {
    ztop = skin > 0 ? top_z - skin : front_z;
    hull() {
        translate([led_bot[0], led_bot[1], led_ch_bot - 0.01]) cylinder(d = d_bot, h = 0.01);
        translate([led_win[0], led_win[1], ztop - 0.01]) cylinder(d = d_top, h = 0.01);
    }
}

// Outer cover block, from the split line to the front, with the front outer edges chamfered.
module cover_shell() {
    b = cover_bevel;
    if (b <= 0) translate([0, 0, floor_t]) linear_extrude(top_z - floor_t) outline2d();
    else hull() {
        translate([0, 0, floor_t]) linear_extrude(top_z - floor_t - b) outline2d();
        translate([0, 0, top_z - 0.01]) linear_extrude(0.01) offset(delta = -b) outline2d();
    }
}

// Modelled in box coordinates (front up); printed front face down, see cover_print().
module cover() {
    boss_len = front_z - boss_bot;
    difference() {
        union() {
            difference() {
                cover_shell();
                translate([0, 0, floor_t - 1]) linear_extrude(front_z - floor_t + 1) cavity2d();
            }
            for (b = buttons) {
                p = esp(b[0]);
                translate([p[0], p[1], boss_bot]) cylinder(d = 8, h = boss_len + 0.01);
            }
            // LED light shroud (keeps the light in one column, so the window is bright)
            if (led_window != "none") led_channel(led_window_d + 2 * led_wall, led_bot_d + 2 * led_wall, 0);
            // hold-down rib on the ESP32 board (free strip, |y| <= 9.5)
            p = esp([press_x, 0]);
            translate([p[0] - 0.8, yc - 9.5, esp_z0 + pcb_t - press]) cube([1.6, 19, front_z - esp_z0 - pcb_t + press + 0.01]);
            // ribs that keep the cell in its clips
            for (f = [0.08, 0.5, 0.92]) translate([x_b0 + bat_end_t + f * bat_l - 0.8, yc - 3, floor_t + bat_lift + 18.6])
                cube([1.6, 6, front_z - floor_t - bat_lift - 18.6 + 0.01]);
        }
        // cover screw inserts (from the open back side)
        for (s = cover_screws) translate([s[0], s[1], floor_t - 0.01]) cylinder(d = insert_d, h = insert_len);
        // USB port tunnel and the groove for the plug's seal bead
        translate([inner_x - 1, port_yc, port_z]) rotate([90, 0, 90])
            linear_extrude(end_wall + 2) rrect(port_w, usb_plug_h, 2);
        translate([inner_x + end_wall - gasket_depth, port_yc, port_z]) rotate([90, 0, 90])
            linear_extrude(gasket_depth + 1) port_ring2d(gasket_w);
        // plunger bores, pockets, O-ring entry chamfers
        for (b = buttons) {
            p = esp(b[0]);
            translate([p[0], p[1], 0]) {
                translate([0, 0, boss_bot - 1]) cylinder(d = 4.3, h = top_z);
                translate([0, 0, top_z - b[1] - 1]) cylinder(d = b[2], h = b[1] + 2);
                translate([0, 0, boss_bot - 0.01]) cylinder(d1 = 5.3, d2 = 4.3, h = 0.81);
            }
            if (button_marks) translate([p[0] + max(b[2], 6) / 2 + 2, p[1], top_z - label_depth])
                linear_extrude(label_depth + 1)
                    text(b[3], size = mark_size, font = label_font, halign = "left", valign = "center");
        }
        // LED light channel + window
        if (led_window != "none") {
            led_channel(led_window_d, led_bot_d, led_window == "thin" ? led_skin : 0);
            if (led_window == "hole") translate([led_win[0], led_win[1], front_z - 1])
                cylinder(d = led_rod_d, h = front_t + 2);
            // ring sits over the shroud wall, outside the light channel, so the skin stays intact
            if (led_mark) translate([led_win[0], led_win[1], top_z - 0.4])
                linear_extrude(1) difference() {
                    circle(d = led_window_d + 1.6);
                    circle(d = led_window_d + 0.8);
                }
        }
        // engraved label in the centre of the front
        if (label != "") translate([inner_x / 2, yc, top_z - label_depth])
            linear_extrude(label_depth + 1)
                text(label, size = label_size, font = label_font, halign = "center", valign = "center");
    }
}

module cover_print() { translate([0, inner_y, top_z]) rotate([180, 0, 0]) cover(); }

// Plunger, printed standing on its flange. Pin 4.0 mm in a 4.3 mm bore; O-ring 2.5 x 1 mm.
// recess = how far the tip sits below the front surface.
module plunger(recess = user_recess) {
    l = top_z - recess - plunger_bot;
    g = oring_z - plunger_bot;
    difference() {
        union() {
            cylinder(d = 6.5, h = 1.5);
            cylinder(d = 4.0, h = l);
        }
        translate([0, 0, g - 0.65]) difference() {
            cylinder(d = 5, h = 1.3);
            cylinder(d = 2.6, h = 1.3);
        }
    }
}

// Captive button pin, in its rest position frame: z = 0 at the pin end (plunger_bot, 0.4 mm
// above the switch; the stub reaches pin_reach lower). Pushed into the bore from the front; the head sits in the pocket. The
// collar (glued onto the stub from inside) rests against the boss, so the pin cannot come out.
// Pressing moves it down until the head reaches the pocket floor (pin_travel).
function pin_head_bot(b) = top_z - b[1] - 1 + pin_travel - plunger_bot; // pocket floor = top_z - recess - 1
module button_pin(b) {
    hb = pin_head_bot(b);
    hd = b[2] - 2 * pin_head_clr;
    translate([0, 0, -pin_reach]) cylinder(d = pin_stub_d, h = collar_h + pin_reach + 0.01); // stub: collar goes up to the shoulder
    translate([0, 0, collar_h]) cylinder(d = 4.0, h = hb - collar_h + 0.01);
    translate([0, 0, hb]) {                                               // head, chamfered top
        cylinder(d = hd, h = pin_head_t - 0.3);
        translate([0, 0, pin_head_t - 0.3 - 0.01]) cylinder(d1 = hd, d2 = hd - 0.6, h = 0.31);
    }
}
module button_pin_print(b) {   // head on the bed, stub up
    translate([0, 0, pin_head_bot(b) + pin_head_t]) rotate([180, 0, 0]) button_pin(b);
}
module button_collar() {
    difference() {
        cylinder(d = collar_d, h = collar_h);
        translate([0, 0, -1]) cylinder(d = pin_stub_d + collar_clr, h = collar_h + 2);
    }
}
for (b = buttons) assert(pin_head_bot(b) + plunger_bot + pin_head_t <= top_z, "button head sticks out of its pocket");
assert(pin_reach >= 0 && pin_reach < 0.4 + pin_travel, "pin_reach: the pin would press the switch at rest beyond its travel");

// TPU push-in USB plug, printed cap down (outer face on the bed), stem up.
// Local X = box Z, local Y = box Y - port_yc, local Z = depth toward the box.
module usb_plug() {
    cap_t = 2.5;
    c = port_seal + gasket_w / 2;
    // cap: 3 mm past the groove top and bottom; across (Y) only as wide as the flat part of the
    // end face (the corners are rounded), but always covering the seal bead
    bead_half = (port_w + 2 * c + plug_bead_w) / 2;
    cap_half_y = max(port_yc - (-wall + 2) + 0.2, bead_half + 0.15);
    cap = [usb_plug_h + 2 * c + gasket_w + 6, 2 * cap_half_y];
    body = [usb_plug_h - 2 * plug_clr, port_w - 2 * plug_clr];
    rib = [usb_plug_h + 2 * plug_rib_int, port_w + 2 * plug_rib_int];
    r_body = 2 - plug_clr;
    module rr(sz, r) rrect(sz[0], sz[1], r);
    // one barb: flares out over 0.4 mm, tapers back over 0.8 mm toward the tip (easy in, hard out)
    module slice(z, sz, r) translate([0, 0, z]) linear_extrude(0.01) rr(sz, r);
    module barb(z) {
        hull() { slice(z, body, r_body); slice(z + 0.4, rib, 2 + plug_rib_int); }
        hull() { slice(z + 0.4, rib, 2 + plug_rib_int); slice(z + 1.2, body, r_body); }
    }
    difference() {
        union() {
            translate([port_z, 0, 0]) {
                linear_extrude(cap_t) rr(cap, 3.5);
                // seal bead into the cover's port groove
                translate([0, 0, cap_t - 0.01]) mirror([1, 1, 0])
                    linear_extrude(gasket_depth + plug_squeeze + 0.01) port_ring2d(plug_bead_w);
                // stem with 2 barbed ribs
                translate([0, 0, cap_t - 0.01]) linear_extrude(plug_stem_l + 0.01) rr(body, r_body);
                for (z = [cap_t + 1.2, cap_t + 3.2]) barb(z);
            }
            // pull tab at the top, standing off the box face so a fingernail gets behind it
            translate([port_z + cap[0] / 2 - 1, -4, 0]) cube([4, 8, 1.6]);
        }
        // hollow stem
        translate([port_z, 0, cap_t - 1]) linear_extrude(plug_stem_l + 2)
            rr(body - [2, 2] * plug_wall, max(r_body - plug_wall, 0.3));
    }
}
assert(plug_stem_l <= end_wall - 2, "USB plug stem would reach the USB-C socket");

// Saddle over both USB-C shells (J4, J6), in box coordinates. Pockets fit the shells (open
// toward the port end), the top is pressed by the cover front. Skirts hang 2 mm down the shell
// sides, above the 0603 LEDs D5..D10 and the 0402 parts between the ports.
module usb_clamp() {
    c = 0.2; sk = 1.0; skirt = 2;
    x0 = -usb_overhang; x1 = x0 + usb_shell[0];
    yo = max([for (y = usb_y) abs(y)]) + usb_shell[1] / 2 + c + sk;
    translate([esp_x0, yc, 0]) rotate([0, 0, 180]) difference() {
        translate([x0, -yo, usb_shell_top - skirt])
            cube([x1 + c + sk - x0, 2 * yo, front_z + usb_clamp_press - usb_shell_top + skirt]);
        for (y = usb_y) translate([x0 - 1, y - usb_shell[1] / 2 - c, usb_shell_top - skirt - 1])
            cube([x1 + c - x0 + 1, usb_shell[1] + 2 * c, skirt + 1]);
    }
}
module usb_clamp_print() {   // top on the bed, pockets up
    translate([0, 0, front_z + usb_clamp_press]) rotate([180, 0, 0])
        translate([-esp_x0, -yc, 0]) usb_clamp();
}

// TPU gasket for the plate groove, printed flat (groove side on the bed, ridge up).
module gasket() {
    linear_extrude(gasket_base_h) band2d(gasket_base_w);
    translate([0, 0, gasket_base_h - 0.01]) linear_extrude(gasket_ridge_h + 0.01) band2d(gasket_ridge_w);
}

// Awning bracket. Plate x 0 = box outer -X face. Hose clamps run through the two tunnels
// around the front bar, or use the two countersunk holes for M4 screws / rivets.
module mount() {
    plate_t = 2 + strap_t + 2;
    pw = inner_y + 2 * wall;
    tab_t = 5;
    difference() {
        union() {
            translate([0, -pw / 2, -plate_t]) cube([mount_len, pw, plate_t]);
            dove_taper(0, mount_len, dt_neck + dt_taper, dt_neck, dt_h - 0.4, 0);
            // end stop + lock-screw tab
            translate([-tab_t, -5.5, -plate_t]) cube([tab_t, 11, plate_t + sled_h + lock_z + 3.5]);
            translate([-tab_t, -pw / 2, -plate_t]) cube([tab_t, pw, plate_t]);
        }
        for (f = [0.2, 0.8]) translate([f * mount_len - strap_w / 2, -pw / 2 - 1, -plate_t + 2])
            cube([strap_w, pw + 2, strap_t]);
        for (x = [mount_len / 2 - 10, mount_len / 2 + 10]) translate([x, 0, -plate_t - 1]) {
            cylinder(d = 4.5, h = plate_t + dt_h + 2);
            translate([0, 0, plate_t + 1 + dt_h - 0.4 - 2.3]) cylinder(d1 = 4.5, d2 = 8, h = 2.31);
        }
        translate([-tab_t - 1, 0, sled_h + lock_z]) rotate([0, 90, 0]) {
            cylinder(d = 3.4, h = tab_t + 2);
            cylinder(d = 6.2, h = 2.5);
        }
    }
}

// ======================================================================= views

module ghosts() {
    %translate([x_b0 + bat_end_t, yc, floor_t + bat_lift + 9.2]) rotate([0, 90, 0]) cylinder(d = 18.4, h = 65);
    %translate([esp_x0, yc, esp_z0]) rotate([0, 0, 180]) {
        translate([0, -pcb_w / 2, 0]) cube([pcb_l, pcb_w, pcb_t]);
        translate([43.64, -6.6, pcb_t]) cube([16.6, 13.2, 2.4]);                  // ESP32-H2 module
        for (y = usb_y) translate([-usb_overhang, y - 4.5, 0.77]) cube([6.5, 9, 4.07]); // USB-C
        translate([9.13, -12.22, pcb_t]) cube([7.9, 8.6, 5.5]);                   // battery JST
    }
    %translate([x_s0, yc - adxl_len / 2, floor_t + adxl_standoff]) cube([adxl_dep, adxl_len, 1.6]);
}

module assembly() {
    color("lightgray") plate();
    ghosts();
    color("white", 0.45) translate([0, 0, 25]) cover();
    for (b = buttons) {
        p = esp(b[0]);
        color("orange") translate([p[0], p[1], plunger_bot + 25]) if (button_style == "captive") {
            button_pin(b);
            translate([0, 0, -8]) button_collar();
        } else plunger(b[1]);
    }
    color("black") translate([0, 0, floor_t - gasket_depth + 12]) gasket();
    if (!usb_bridge) color("orange") translate([0, 0, 12]) usb_clamp();
    if (esp_hold == "clips") color("orange") for (s = [-1, 1]) translate([0, 0, 6]) board_clip(s);
    color("tomato") translate([inner_x + end_wall + 12, port_yc, 0]) rotate([0, -90, 0]) usb_plug();
    if (sled_h > 0) color("dimgray") translate([-end_wall, yc, -sled_h - 15]) mount();
    else color("dimgray") translate([0, 0, -15]) awning_base();
    if (adxl_fix == "pins") for (dy = [-1, 1]) color("orange")
        translate([x_s0 + adxl_hole_inset, yc + dy * adxl_hole_spacing / 2, floor_t + adxl_standoff + adxl_pcb_t + 10 + apin_head[1]])
            mirror([0, 0, 1]) adxl_pin();
}

module print_plate() {
    translate([0, 0, back_h]) plate();
    translate([0, inner_y + 2 * wall + 8, 0]) cover_print();
    for (i = [0 : len(buttons) - 1]) translate([10 + i * 10, -15, 0])
        if (button_style == "captive") { button_pin_print(buttons[i]); translate([0, -10, 0]) button_collar(); }
        else plunger(buttons[i][1]);
    translate([40, -15, 0]) usb_plug();
    if (sled_h > 0) translate([90, -30, 2 + strap_t + 2]) mount();
}

if (part == "plate") translate([0, 0, back_h]) plate();
else if (part == "cover") cover_print();
else if (part == "plunger") plunger(user_recess);
else if (part == "reset_plunger") plunger(reset_recess);
else if (part == "gasket") gasket();
else if (part == "usb_plug") usb_plug();
else if (part == "usb_clamp") usb_clamp_print();
else if (part == "board_clips") board_clips_print();
else if (part == "awning_base") awning_base_print();
else if (part == "adxl_pins") adxl_pins_print();
else if (part == "adxl_pins_test") adxl_pins_print(adxl_pin_test_d);
else if (part == "button_pin") button_pin_print(user_btn);
else if (part == "reset_button_pin") button_pin_print(reset_btn);
else if (part == "button_collar") button_collar();
else if (part == "mount") translate([0, 0, 2 + strap_t + 2]) mount();
else if (part == "print_plate") print_plate();
else if (part == "none") { }   // for test files that include this one
else assembly();
