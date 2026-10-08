"""Compile the real scanner against fake GPIOs and exercise noisy key traces.

Run: python tests/test_kscan.py (requires GCC; no Zephyr installation).
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

STUBS = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
struct device { const void *config; void *data; };
struct gpio_dt_spec { const struct device *port; int pin; };
typedef void (*kscan_callback_t)(const struct device *, uint32_t, uint32_t, bool);
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; };
struct k_timer { int unused; };
struct kscan_driver_api {
    int (*config)(const struct device *, kscan_callback_t);
    int (*enable_callback)(const struct device *);
    int (*disable_callback)(const struct device *);
};
#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define LEN_input_gpios 7
#define LEN_output_gpios 4
#define LEN_direct_gpios 4
#define DT_INST_PROP_LEN(n, prop) LEN_##prop
#define PROP_polling_interval_msec 25
#define PROP_settle_time_us 100
#define PROP_debounce_period 10
#define PROP_active_discharge 0
#define DT_INST_PROP(n, prop) PROP_##prop
#define DT_DRV_INST(n) n
#define SPECS_input_gpios {NULL,0},{NULL,1},{NULL,2},{NULL,3},{NULL,4},{NULL,5},{NULL,6}
#define SPECS_output_gpios {NULL,7},{NULL,8},{NULL,9},{NULL,10}
#define SPECS_direct_gpios {NULL,11},{NULL,12},{NULL,13},{NULL,14}
#define DT_FOREACH_PROP_ELEM(n, prop, fn) SPECS_##prop
#define DEVICE_DT_INST_DEFINE(n, init, pm, data, config, level, priority, api) \
    static struct device test_dev = {config, data};
#define DT_INST_FOREACH_STATUS_OKAY(fn) fn(0)
#define LOG_MODULE_DECLARE(...)
#define LOG_DBG(...)
#define LOG_ERR(...)
#define GPIO_INPUT 0
#define GPIO_OUTPUT_INACTIVE 1
#define GPIO_OUTPUT_ACTIVE 2
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
static uint32_t test_time;
static unsigned sample_step;
static int scheduled;
static bool keys[7][20];
static bool pin_state[15];
static bool tab_transient;
static bool gpio_error;
static int gpio_pin_get_dt(const struct gpio_dt_spec *spec) {
    if (gpio_error) return -EIO;
    int address = 0;
    for (int bit = 0; bit < 4; bit++) address |= pin_state[7 + bit] << bit;
    if (tab_transient && spec->pin == 2 && address == 8 && (sample_step % 2)) return 1;
    bool pressed = keys[spec->pin][address];
    for (int d = 0; d < 4; d++) pressed |= pin_state[11 + d] && keys[spec->pin][16 + d];
    return pressed;
}
static int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value) {
    pin_state[spec->pin] = value;
    return 0;
}
static int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags) { return 0; }
static bool device_is_ready(const struct device *dev) { return true; }
static void k_busy_wait(int us) { sample_step++; }
static uint32_t k_uptime_get_32(void) { return test_time; }
static int k_work_schedule(struct k_work_delayable *work, int delay) { scheduled++; return 0; }
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
static void k_work_init_delayable(struct k_work_delayable *work, void (*handler)(struct k_work *)) {}
static void k_timer_init(struct k_timer *timer, void (*handler)(struct k_timer *), void *stop) {}
static void k_timer_start(struct k_timer *timer, int duration, int period) {}
static void k_timer_stop(struct k_timer *timer) {}
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "scanner.c"
static unsigned events[7][20][2];
static void event(const struct device *dev, uint32_t row, uint32_t col, bool pressed) {
    events[row][col][pressed]++;
}
static void reset(void) {
    memset(keys, 0, sizeof(keys));
    memset(pin_state, 0, sizeof(pin_state));
    memset(events, 0, sizeof(events));
    memset(&kscan_gpio_data_0, 0, sizeof(kscan_gpio_data_0));
    tab_transient = gpio_error = false;
    scheduled = 0;
    assert(kscan_gpio_init_0(&test_dev) == 0);
    assert(kscan_gpio_configure_0(&test_dev, event) == 0);
}
static void scan(uint32_t now) {
    test_time = now;
    sample_step = 0;
    assert(kscan_gpio_read_0(&test_dev) == 0);
}
int main(void) {
    /* S press and release bounce must produce exactly one pair of edges. */
    reset();
    keys[3][10] = true; scan(0);
    keys[3][10] = false; scan(5);
    keys[3][10] = true; scan(10); scan(15); scan(20);
    assert(events[3][10][1] == 1);
    keys[3][10] = false; scan(25);
    keys[3][10] = true; scan(30);
    keys[3][10] = false; scan(35); scan(40); scan(45);
    assert(events[3][10][1] == 1 && events[3][10][0] == 1);
    assert(scheduled > 0); /* A pending final release must keep scanning. */
    /* A later intentional second tap still works. */
    keys[3][10] = true; scan(50); scan(60);
    keys[3][10] = false; scan(65); scan(75);
    assert(events[3][10][1] == 2 && events[3][10][0] == 2);

    /* A changing row at TAB's address never becomes a TAB event. */
    reset(); tab_transient = true; keys[2][15] = true;
    for (int t = 0; t <= 50; t += 5) scan(t);
    assert(events[2][8][1] == 0 && events[2][15][1] == 1);
    tab_transient = false; keys[2][8] = true;
    scan(55); scan(65);
    assert(events[2][8][1] == 1);

    /* Every row: parked mux keys must not ghost into any direct column. */
    for (int r = 0; r < 7; r++) {
        reset(); keys[r][15] = true;
        scan(0); scan(5); scan(10); scan(15);
        assert(events[r][15][1] == 1);
        for (int d = 16; d < 20; d++) assert(events[r][d][1] == 0);
        keys[r][15] = false; scan(20); scan(30);
        keys[r][19] = true; scan(35); scan(45);
        assert(events[r][19][1] == 1);
        keys[r][19] = false; scan(50); scan(60);
        assert(events[r][19][0] == 1);
    }

    /* Ambiguous parked samples cannot advance a pending direct press. */
    reset(); keys[4][19] = true; scan(0);
    keys[4][15] = true; scan(5); scan(20);
    assert(events[4][19][1] == 0);
    keys[4][15] = false; scan(25); scan(30);
    assert(events[4][19][1] == 0);
    scan(35); assert(events[4][19][1] == 1);
    /* A held direct key stays held during masking, then releases. */
    keys[4][15] = true; keys[4][19] = false; scan(40); scan(55);
    assert(events[4][19][0] == 0);
    keys[4][15] = false; scan(60); scan(70);
    assert(events[4][19][0] == 1);

    /* Independent same-row keys, including two numpad keys, remain usable. */
    reset(); keys[2][9] = keys[2][10] = keys[4][16] = keys[4][19] = true;
    scan(0); scan(10);
    assert(events[2][9][1] == 1 && events[2][10][1] == 1);
    assert(events[4][16][1] == 1 && events[4][19][1] == 1);

    /* GPIO failures do not fabricate releases or finish a pending press. */
    gpio_error = true; scan(15); scan(30);
    assert(events[2][9][0] == 0);
    gpio_error = false; keys[2][9] = false; scan(35); scan(45);
    assert(events[2][9][0] == 1);

    /* Timer wraparound and explicit debounce-off configuration. */
    struct k3yb_debounce state = {0};
    assert(!k3yb_debounce_update(&state, false, true, true, UINT32_MAX - 4, 10));
    assert(k3yb_debounce_update(&state, false, true, true, 5, 10));
    assert(!k3yb_debounce_update(&state, true, false, true, 6, 0));
    puts("PASS: bounce, TAB transients, all-row mux masking, independent keys, GPIO errors, timer wrap");
}
'''


def check_keymap():
    import re
    keymap = (ROOT / 'boards/shields/k3yb_it/k3yb_it.keymap').read_text(encoding='utf-8')
    keymap = re.sub(r'/\*.*?\*/', '', keymap, flags=re.S)
    layers = keymap.split('keymap {', 1)[1]
    bindings = re.findall(r'bindings\s*=\s*<(.*?)>;', layers, re.S)
    assert len(bindings) == 5
    for layer in bindings:
        assert layer.count('&') + (17 if 'NP_TRANS' in layer else 0) == 105
    base = re.findall(r'&\w+(?:\s+[^&]+)?', bindings[0])
    assert base[63].strip() == '&mo 2'  # Backslash beside left Shift.
    assert base[82].strip() == '&none'  # Old AltGr layer key.
    assert base[85].strip() == '&mo 3'  # Right Ctrl acute layer.
    print('PASS: all layer lengths and accent activation positions')


if __name__ == '__main__':
    check_keymap()
    gcc = shutil.which('gcc')
    if not gcc:
        raise SystemExit('GCC is required')
    with tempfile.TemporaryDirectory(prefix='k3yb-kscan-') as temp:
        path = Path(temp)
        (path / 'stubs.h').write_text(STUBS)
        for name in ['device.h', 'drivers/kscan.h', 'drivers/gpio.h', 'kernel.h', 'logging/log.h']:
            header = path / 'zephyr' / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "stubs.h"\n')
        shutil.copyfile(ROOT / 'drivers/kscan/kscan_gpio_demux_settle.c', path / 'scanner.c')
        shutil.copyfile(ROOT / 'drivers/kscan/kscan_debounce.h', path / 'kscan_debounce.h')
        (path / 'test.c').write_text(HARNESS)
        executable = path / 'kscan-test.exe'
        subprocess.run([gcc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-const-variable',
                        '-I', str(path), str(path / 'test.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
