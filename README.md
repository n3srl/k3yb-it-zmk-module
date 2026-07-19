ZMK Support For K3yb.it! Keyboards by N3 S.r.l.

www.n-3.it

---

# k3yb.it — ZMK module

Custom ZMK firmware module for the **k3yb.it** keyboard: a 105-key full-size
custom board built around a **nice!nano v2** (nRF52840), with the main key
matrix multiplexed through a CD74HC4067, a direct-wired numpad section,
four status LEDs driven through a CD4052, and interchangeable I2C OLED
displays.

Built on [ZMK](https://zmk.dev) v0.3 (Zephyr 3.5) via the standard
`build-user-config.yml` GitHub Actions workflow. Every push builds all
firmware variants; download them from the **Actions** tab.

## Firmware variants

| Artifact | Display | Notes |
|---|---|---|
| `k3yb_it` | none | base variant |
| `k3yb_it_SSD1306_128x32` | 0.91" SSD1306 128×32 I2C | compact status layout |
| `k3yb_it_SSD1327_128x128` | 1.5" SSD1327 128×128 I2C | full status layout |
| `*_debug` | as above | + USB serial logging (CDC ACM, 115200) |
| `k3yb_it_SSD1327_128x128_testpattern` | SSD1327 | GDDRAM probe patterns for panel bring-up |

Flashing: double-tap RST→GND on the nice!nano (or hold the **ù key + ESC**
once this firmware is running) to enter the UF2 bootloader, then copy the
`.uf2` onto the `NICENANO` drive.

Adding a new display = one variant overlay + one `config/<shield>.conf` +
one `build.yaml` entry (see `k3yb_it_oled32.overlay` as template).

## Hardware pinout (nice!nano v2, pro-micro numbering)

### Key matrix — main section (7 rows × 16 columns via CD74HC4067)

| Signal | Pin | Notes |
|---|---|---|
| Row 0..6 (sense) | D8 D7 D6 D5 D4 D3 D2 | inputs, internal pull-down, active-high |
| MUX S0 (LSB) | D21 | `NANO_MUX 1` |
| MUX S1 | D20 | `NANO_MUX 2` |
| MUX S2 | D19 | `NANO_MUX 3` |
| MUX S3 (MSB) | D18 | `NANO_MUX 4` |
| MUX COM (pin 1) | **+3.3 V** | see errata below |
| MUX E̅ (pin 15) | GND | always enabled |

MUX address → physical column: `I0..I7 = Col 15..8`, `I8..I15 = Col 0..7`
(handled by the matrix transform in `k3yb_it.dtsi`).

Diode orientation: **anode at the switch/column, cathode (white bar) at the
row** — current flows column → row, hence the selected column sources 3.3 V
and rows idle low with pull-downs.

### Numpad section (5 rows × 4 direct columns, rows shared with main)

| Signal | Pin |
|---|---|
| Pad Col 0 (NUM 7 4 1 0) | D10 |
| Pad Col 1 (/ 8 5 2 .) | D16 |
| Pad Col 2 (* 9 6 3) | D14 |
| Pad Col 3 (- + Enter) | D15 |

### Status LEDs (CD4052 dual 4:1 analog mux)

| Signal | Pin | Notes |
|---|---|---|
| A (select LSB) | D1 | `LED_MUX 1` |
| B (select MSB) | D0 | `LED_MUX 2` |
| Inh | D9 | `LED_OUTPUT`, **low = LED lit** |
| X0..X3 | +3.3 V | X common shorted to Y common |
| Y0 | Num Lock LED | select 00 |
| Y1 | Caps Lock LED | select 01 |
| Y2 | Scroll Lock LED | select 10 |
| Y3 | accent-layer LED | select 11 (was "Mic On" — mic state is not observable over HID) |

### Display (I2C, 4-pin modules)

| OLED pin | nice!nano pad |
|---|---|
| SDA | P1.01 (inner pad "101") |
| SCL | P1.02 (inner pad "102") |
| VCC | 3.3 V |
| GND | GND |

P1.07 is unused/spare. I2C address 0x3C.

### Hardware errata (apply to current PCB / next revision)

1. **4067 COM (pin 1) must go to +3.3 V, not GND** — with column→row
   diodes the selected column must *source* current. Wiring COM to GND
   (original design) makes the matrix electrically impossible.
   ⚠️ When reworking: pin 1 is at the dot corner; **pin 12 is GND and must
   stay grounded** — a 4067 with its GND pin lifted to 3.3 V has no ground
   reference and all channels conduct at once.
2. Next revision: route 4067 **E̅ to P1.07** instead of GND, so the scanner
   can disable the mux while scanning the direct numpad columns (removes
   the parked-address ghost-masking workaround in the kscan driver).
3. Optional: 10 kΩ external pull-downs on the row lines for extra noise
   margin.

## Custom components

### `drivers/kscan/kscan_gpio_demux_settle.c` — unified matrix scanner

Compatible: `k3yb,kscan-gpio-demux`. Fork of ZMK's `kscan-gpio-demux`
extended to scan, **sequentially within a single pass**:

1. the 16 demux-selected columns (scan columns 0–15), then
2. the direct-driven numpad columns (`direct-gpios`, scan columns 16+)

on the same shared row pins — the two column sets are never driven at the
same time, which is what allows rows to be shared safely.

Properties: `settle-time-us` (default 10; we use 30) is the wait between
column change and row sampling. `active-discharge` (bool, **off** by
default) actively drives rows low between columns; it kills residual-charge
ghosting but briefly shorts a driven-high column into driven-low row pins
through any held key — beyond nRF52840 pin specs — so leave it off unless
ghosting is proven on healthy hardware.

Because the 4067 cannot be disabled (E̅ hardwired low), it stays parked on
address 15 during the numpad scan; a held key on that column (F7/U/J/N)
would ghost onto the numpad columns. The driver masks this by freezing the
direct-column state of any row that reads pressed on the parked address.

### `drivers/display/ssd1327.c` — SSD1327 OLED driver

Compatible: `k3yb,ssd1327`. Zephyr 3.5 has no SSD1327 driver, so this
module provides a minimal I2C one, exposing the 4-bit grayscale panel to
LVGL as 1-bpp monochrome. A full shadow framebuffer (width/2 × height
bytes) lets LVGL flush regions with odd X coordinates merge into the
2-pixels-per-byte GDDRAM layout. `remap-value` (register 0xA0, default
0x51) controls orientation/mirroring.

`CONFIG_K3YB_SSD1327_TEST_PATTERN=y` (used by the `testpattern` artifact)
replaces normal operation with cycling raw-GDDRAM probes (full fill, nibble
placement, column pitch, row advance) — photograph them to verify a new
panel's memory mapping. Note: the cheap 1.5" panels show a strong physical
row structure (inter-pixel gaps); it looks like interlacing but is not an
addressing artifact.

### `src/led_mux.c` — time-multiplexed status LEDs

One LED at a time is selected through the CD4052 (A/B) and gated by Inh,
rotating every 1 ms (≈250 Hz full cycle — flicker-free, uniform 25% duty
per LED). LED states refresh every 40 ms: Num/Caps/Scroll from the HID
indicators reported by the host, Y3 from the accent layers.

With `flame-mode` set on the `k3yb,led-mux` node (default in this shield),
each *active* LED flickers like a small flame: its slot is lit with
probability `level/256`, and each LED's `level` random-walks on its own
50–230 ms cadence, so the four flames are never in sync.

### `src/behavior_repeat.c` — auto-repeat wrapper behavior

Compatible: `k3yb,behavior-repeat`. Wraps another behavior (here the
accent mod-morphs) and re-fires it while the key is held — initial
`delay-ms` (400), then every `rate-ms` (80) — like OS key repeat. Needed
because the accent keys are macros: the host cannot auto-repeat a macro.

### `src/status_screen.c` — custom status screen (LVGL)

Shared by both display variants, layout adapts at runtime:

- boot: **N3 logo** (from `boards/shields/k3yb_it/logo/*.png`, converted
  to LVGL `ALPHA_1BIT` C arrays in `src/n3_logo*.c`) for 2.5 s
- top-left: transport symbols — USB and Bluetooth shown simultaneously
  when both active, charge bolt when USB-powered
- top-right: battery symbol + percent + pack voltage (from the
  `zmk,battery` sensor)
- mid-right: **active** locks only, uppercase (`NUM CAPS SCRL`)
- bottom-left: active layer (`BASE` / `PAD` / `GRAVE` / `ACUTO`)
- bottom-right: words per minute

## Layout and Italian accents

Base layout is **US ASCII** (set the host OS layout to English-US).
Accented vowels are produced with **Windows Alt+numpad codes**, so they
need **NumLock ON** and work on Windows hosts.

- Hold the **ù key** (right of the quote key, taps as `#`) + vowel →
  **grave**: à è ì ò ù — with Shift: À È Ì Ò Ù
- Hold the **menu key** (right of AltGr, taps as context-menu) + vowel →
  **acute**: á é í ó ú — with Shift: Á É Í Ó Ú
- For Italian you mostly need grave (è cioè città) plus **é** (perché) from
  the acute layer.
- Holding a vowel repeats the accent (firmware-side auto-repeat).

Bootloader shortcut: hold **ù + ESC** to enter the UF2 bootloader.

## Debugging

- Flash a `_debug` artifact → a USB CDC serial port appears; open it at
  115200 (e.g. PuTTY). Key events log as `position_state_changed`; scan
  events as `kscan_gpio_read`. A key that scans but is missing from the
  matrix transform logs `Not found in transform: row R, col C` — the
  fastest way to map physical keys.
- `k3yb_it_SSD1327_128x128_testpattern` for display bring-up.
- Double-tap RST→GND always returns to the bootloader.
