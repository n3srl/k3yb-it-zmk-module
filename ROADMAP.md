# k3yb.it — Roadmap

## v1.0 (current)

Feature-complete firmware for the current PCB: full 105-key matrix,
Italian accent layers with auto-repeat, flame status LEDs, three display
variants (SSD1306 128x32, SSD1327 128x128, SH1107 128x128) with text and
icon status screens, LED-control layer (Scroll Lock + Pause combo) with
persistent settings, offline note recorder (128 KiB internal-flash
keystroke log with HID playback). Per-key backlight is state-machine-only (no drive
hardware on this PCB).

## v2 (next PCB revision + firmware)

### Per-key backlight via IS31FL3733 on I2C (hardware + firmware)

Monochrome per-key backlight driven by an **ISSI IS31FL3733** matrix LED
driver on the existing I2C bus (P1.01/P1.02, shared with the display at
a different address) — **zero extra nice!nano pins, no extra mux**:

- 12 SW x 16 CS matrix (192 LEDs); 105 keys fit on SW1-12 x CS1-9;
- 256-step PWM per LED: per-key levels and per-key flame become possible;
- software shutdown via register (few uA) — SDB tied high through a
  resistor (optionally to P1.07 for a hard off), INTB unused;
- ADDR1/ADDR2 strapped to an address clear of the display;
- R_ISET sets the peak LED current; aim for ~1 mA average per LED at
  full level (1/12 matrix duty), capped further by
  `CONFIG_K3YB_BACKLIGHT_MAX`;
- package QFN-48: order the board PCBA-assembled (JLCPCB/PCBWay);
- firmware: small custom driver in this module (the Zephyr in-tree
  `is31fl3733` driver arrived in Zephyr 3.6, ZMK v0.3 is on 3.5), wired
  into `backlight_apply()` in `src/led_mux.c` — state machine, keymap
  and persistence are already final.

**LEDs:** single-colour **white** (preferred) or **blue**, one per key.

- white gives the most perceived light per mA (blue at 465-470 nm looks
  dimmer to the eye for the same current), so it reaches a usable level
  at the lowest current; pick a high-efficiency part rated for good
  brightness at 1-2 mA;
- switches are Kailh BOX White (IP56 housing, translucent top): V1 BOX
  accepts SMD LEDs only, V2 also 2-pin THT — so use **1206 reverse-mount
  SMD** on the PCB bottom, shining through a cutout under the switch LED
  window (switch footprint must include the cutout);
- chosen part: **TUOZHAN P2-1206WYCS2-0.9T-F** (LCSC C2827252), white,
  reverse-mount 1206, 1 cd @ 20 mA, Vf 3.4 V; alternative **MEIHUA
  MHT151WDT** (LCSC C401114), 900 mcd @ 20 mA, Vf 3.65 V (less PVCC
  headroom). At the ~1-2 mA backlight operating point both are far
  brighter than needed under keycaps;
- **headroom:** white/blue Vf is ~2.8-3.2 V; the driver needs roughly
  Vf + 0.5-0.7 V on PVCC. Straight from VBAT the backlight dims below
  ~3.7 V battery. Either accept it (backlight fades in the lower half of
  the discharge curve) or feed PVCC from a small 5 V boost converter
  gated off in shutdown;
- budget: ~105 mA at full level from the LED side alone vs a few hundred
  uA for the rest of the keyboard — keep the level cap low and add
  idle auto-off.

The CD4052 stays for the 4 status LEDs.

### Numpad into the mux matrix (hardware)

The main matrix has 24 unused (row, mux address) positions (row 1:
0,1; row 4: 1,2; row 5: 10; row 6: 10). Wiring the 17 numpad keys
electrically into those positions (physical placement unchanged):

- frees the 4 direct-column pins (D10, D14, D15, D16);
- removes the parked-mux ghosting at the root (no direct columns while
  the 4067 stays enabled), making the firmware masking unnecessary;
- firmware change is limited to the matrix transform and dropping
  `direct-gpios`.

### Keyboard heater (hardware + firmware)

Heating resistor (or resistive trace area) to keep the keyboard usable
at very low ambient temperatures:

- power budget and drive to be sized (likely MOSFET + PWM);
- firmware: thermostat control — the nRF52840 die temperature sensor
  can provide a rough ambient reading at no BOM cost, or add a proper
  I2C temperature sensor on the existing bus;
- candidate controls in the existing `led_control` layer or a new combo.

### Conformal coating (hardware)

Tropicalized/conformal-coated PCB for humidity and condensation
resistance (pairs with the low-temperature use case above).

### PCB items deferred from the v1.1 respin

The v1.1 production run (2026-08) fixes the 4067 COM→3.3V routing
(pin 12 stays at GND!), the 2 misplaced and 3 rotated switch footprints,
and moves the display to a JST-style SMD connector (pin order handled by
the cable, not the PCB). Deferred to a later revision:

- silkscreen aid for diode orientation (col→row, cathode at the row);
- 10k row pull-downs (internal nRF pull-downs work in practice);
- 4067 E-bar to a dedicated pin (superseded by the firmware
  parked-address masking);
- I2C pull-ups on the PCB (4.7k) — today the OLED module provides
  them; **required** once the IS31FL3733 is on the bus, since the
  no-display build would otherwise have none;
- test points on 4067 COM, one row, one select, SDA/SCL.

### Firmware ideas

- OLED page for LED/backlight status (getter API already exposed);
- recorder on the OLED: REC marker + events used/capacity
  (`k3yb_recorder_count()` / `_capacity()` already exposed);
- recorder: optional ring-buffer mode (overwrite oldest page) instead of
  stop-at-full;
- custom 1bpp icon/text fonts (LVGL Montserrat 4bpp crashes this
  pipeline — any custom font must be converted at bpp 1);
- richer battery telemetry screen.
