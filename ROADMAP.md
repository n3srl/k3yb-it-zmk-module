# k3yb.it — Roadmap

## v1.0 (current)

Feature-complete firmware for the current PCB: full 105-key matrix,
Italian accent layers with auto-repeat, flame status LEDs, three display
variants (SSD1306 128x32, SSD1327 128x128, SH1107 128x128) with text and
icon status screens, LED-control layer (Scroll Lock + Pause combo) with
persistent settings. Per-key backlight is state-machine-only (no drive
hardware on this PCB).

## v2 (next PCB revision + firmware)

### LED mux replacement / expansion (hardware)

Replace the CD4052 with a larger mux (or LED driver) with more channels:

- solves the CD4052 X/Y coupling that blocked the per-key backlight
  (shared A/B/Inh make Xk conduct exactly when Yk does);
- dedicated channel(s) for the **105-key backlight** — the firmware side
  is already in place (`backlight_apply()` in `src/led_mux.c` is the
  single hook, state machine / keymap / persistence are final);
- spare channels for future indicators or lighting zones.

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

### PCB errata from v1 (documented in README)

- 4067 COM routed to +3.3V natively (no rework);
- optional: 4067 E-bar to a dedicated pin (superseded by the firmware
  parked-address masking, keep only if pins allow);
- optional: 10k row pull-downs.

### Firmware ideas

- OLED page for LED/backlight status (getter API already exposed);
- custom 1bpp icon/text fonts (LVGL Montserrat 4bpp crashes this
  pipeline — any custom font must be converted at bpp 1);
- richer battery telemetry screen.
