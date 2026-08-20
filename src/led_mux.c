/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Time-multiplexed status LEDs through a CD4052 (see the k3yb,led-mux
 * binding).  A fast timer walks the 4 mux positions (2 ms each, ~125 Hz
 * full cycle - flicker free) and gates the inhibit pin; a slow work item
 * refreshes the desired LED states from ZMK.
 *
 *   Y0 (A=0,B=0)  Num Lock
 *   Y1 (A=1,B=0)  Caps Lock
 *   Y2 (A=0,B=1)  Scroll Lock
 *   Y3 (A=1,B=1)  accent layer active (grave or acute held)
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif

#include <k3yb/led_ctrl.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_MUX_NODE DT_INST(0, k3yb_led_mux)

#define STEP_MS 1
#define STATE_REFRESH_MS 40

#define IND_NUMLOCK BIT(0)
#define IND_CAPSLOCK BIT(1)
#define IND_SCROLLLOCK BIT(2)

#define FLAME_MODE DT_PROP(LED_MUX_NODE, flame_mode)
#define FLAME_MIN 90 /* lowest flame brightness, 0-255 */

static const struct gpio_dt_spec sel_a = GPIO_DT_SPEC_GET(LED_MUX_NODE, a_gpios);
static const struct gpio_dt_spec sel_b = GPIO_DT_SPEC_GET(LED_MUX_NODE, b_gpios);
static const struct gpio_dt_spec inh = GPIO_DT_SPEC_GET(LED_MUX_NODE, inh_gpios);

static volatile uint8_t led_states;          /* bit n = LED Yn on */
static volatile uint8_t led_level[4];        /* per-LED brightness 0-255 (flame) */
static int16_t flame_countdown[4];           /* ms until this LED picks a new level */

/* ---- LED controller state (see include/k3yb/led_ctrl.h) ----------------
 * Kept strictly separate, as independent knobs:
 *   - status_flame: flame effect on the 4 indicators (runtime, DT default)
 *   - bl_on / bl_idx / bl_flame: the per-key backlight
 * The backlight has NO physical output yet: the LEDs arrive with the next
 * PCB revision on the CD4052 X section, whose exact topology is still to
 * be decided (X and Y channels of the same address conduct together, see
 * README).  backlight_apply() is the single hook where the output will be
 * wired in; everything else (state machine, keymap, persistence) is final.
 */

/* brightness steps, 0-255 scale; edit freely, keep index 0 == 0 */
static const uint8_t bl_levels[] = {0, 8, 16, 32, 48, 64};
#define BL_LEVELS ARRAY_SIZE(bl_levels)
#define BL_INITIAL_IDX 3 /* 32/255 */

static bool status_flame = FLAME_MODE;
static bool bl_on;
static uint8_t bl_idx = BL_INITIAL_IDX;      /* current step */
static uint8_t bl_last_idx = BL_INITIAL_IDX; /* last non-zero step, for toggle */
static bool bl_flame;

/* tiny LCG, ISR-safe */
static inline uint8_t prand(void) {
    static uint32_t seed = 0x2a651b7d;

    seed = seed * 1664525u + 1013904223u;
    return seed >> 24;
}

static void led_mux_tick(struct k_timer *timer) {
    static uint8_t idx;

    ARG_UNUSED(timer);
    idx = (idx + 1) & 0x3;

    /* blank while switching the selects, then gate on if this LED is lit.
     * In flame mode the slot is lit with probability level/256, which
     * time-averages into a flickering brightness. */
    gpio_pin_set_dt(&inh, 0);
    gpio_pin_set_dt(&sel_a, idx & 0x1);
    gpio_pin_set_dt(&sel_b, (idx >> 1) & 0x1);
    if (led_states & BIT(idx)) {
        if (!status_flame || prand() < led_level[idx]) {
            gpio_pin_set_dt(&inh, 1); /* logical 1 = enabled (inh is active-low wired) */
        }
    }
}

static K_TIMER_DEFINE(led_mux_timer, led_mux_tick, NULL);

static void led_mux_refresh(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_mux_refresh_work, led_mux_refresh);

static void led_mux_refresh(struct k_work *work) {
    uint8_t states = 0;

    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    {
        uint8_t ind = zmk_hid_indicators_get_current_profile();

        if (ind & IND_NUMLOCK) {
            states |= BIT(0);
        }
        if (ind & IND_CAPSLOCK) {
            states |= BIT(1);
        }
        if (ind & IND_SCROLLLOCK) {
            states |= BIT(2);
        }
    }
#endif
    /* Y3: accent layer (grave = 2 or acute = 3) explicitly active.
     * NOT highest_layer >= 2: the led_control layer (4) must not light
     * the accent indicator. */
    if (zmk_keymap_layer_active(2) || zmk_keymap_layer_active(3)) {
        states |= BIT(3);
    }

    led_states = states;

    /* flame effect: every LED wanders its own brightness on its own
     * cadence, so the four flames flicker out of sync */
    if (status_flame) {
        for (int i = 0; i < 4; i++) {
            flame_countdown[i] -= STATE_REFRESH_MS;
            if (flame_countdown[i] <= 0) {
                led_level[i] = FLAME_MIN + (prand() % (256 - FLAME_MIN));
                flame_countdown[i] = 50 + (prand() % 180); /* 50-230 ms */
            }
        }
    }

    k_work_schedule(&led_mux_refresh_work, K_MSEC(STATE_REFRESH_MS));
}

/* ---- LED controller: persistence -------------------------------------- */

#if IS_ENABLED(CONFIG_SETTINGS)

struct led_settings {
    uint8_t bl_on;
    uint8_t bl_last_idx;
    uint8_t bl_flame;
    uint8_t status_flame;
};

/* deferred save: one flash write ~1 s after the last change, never per
 * keypress burst and never from the PWM tick */
static void led_settings_save(struct k_work *work) {
    struct led_settings s = {
        .bl_on = bl_on,
        .bl_last_idx = bl_last_idx,
        .bl_flame = bl_flame,
        .status_flame = status_flame,
    };

    ARG_UNUSED(work);
    settings_save_one("k3yb_led/state", &s, sizeof(s));
    LOG_DBG("led settings saved");
}

static K_WORK_DELAYABLE_DEFINE(led_settings_save_work, led_settings_save);

static int led_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                            void *cb_arg) {
    struct led_settings s;

    if (strcmp(name, "state") != 0 || len != sizeof(s)) {
        return -ENOENT;
    }
    if (read_cb(cb_arg, &s, sizeof(s)) != sizeof(s)) {
        return -EIO;
    }

    /* validate: out-of-table index falls back to the default */
    bl_last_idx = (s.bl_last_idx > 0 && s.bl_last_idx < BL_LEVELS) ? s.bl_last_idx
                                                                   : BL_INITIAL_IDX;
    bl_on = s.bl_on ? true : false;
    bl_idx = bl_on ? bl_last_idx : 0;
    bl_flame = s.bl_flame ? true : false;
    status_flame = s.status_flame ? true : false;
    LOG_INF("led settings loaded: bl %s idx %d flame %d, status flame %d", bl_on ? "on" : "off",
            bl_last_idx, bl_flame, status_flame);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(k3yb_led, "k3yb_led", NULL, led_settings_set, NULL, NULL);

static void led_settings_dirty(void) {
    k_work_reschedule(&led_settings_save_work, K_MSEC(800));
}

#else
static void led_settings_dirty(void) {}
#endif /* CONFIG_SETTINGS */

/* ---- LED controller: backlight output hook ----------------------------- */

/* effective PWM value, capped by the Kconfig maximum */
uint8_t k3yb_backlight_level_pwm(void) {
    uint8_t v = bl_on ? bl_levels[bl_idx] : 0;

    return MIN(v, CONFIG_K3YB_BACKLIGHT_MAX);
}

static void backlight_apply(void) {
    /* NO physical output yet: the 105 per-key LEDs arrive with the next
     * PCB revision on the CD4052 X section (topology under evaluation -
     * X/Y channels of one address conduct together, see README).  This
     * is the single place where the output gets wired in. */
    LOG_INF("backlight %s: level %d/%d (%d/255)%s", bl_on ? "on" : "off", bl_idx,
            (int)(BL_LEVELS - 1), k3yb_backlight_level_pwm(), bl_flame ? " +flame" : "");
}

/* ---- LED controller: public API ---------------------------------------- */

void k3yb_backlight_toggle(void) {
    bl_on = !bl_on;
    if (bl_on && bl_idx == 0) {
        bl_idx = bl_last_idx; /* restore the last non-zero level */
    }
    backlight_apply();
    led_settings_dirty();
}

void k3yb_backlight_level_up(void) {
    if (!bl_on) {
        bl_on = true;
        bl_idx = bl_last_idx;
    } else if (bl_idx < BL_LEVELS - 1) {
        bl_idx++;
    } else {
        return; /* already at the configured maximum, nothing to save */
    }
    bl_last_idx = bl_idx;
    backlight_apply();
    led_settings_dirty();
}

void k3yb_backlight_level_down(void) {
    if (!bl_on || bl_idx == 0) {
        return;
    }
    bl_idx--;
    if (bl_idx == 0) {
        bl_on = false; /* level 0 = off; bl_last_idx keeps the restore point */
    } else {
        bl_last_idx = bl_idx;
    }
    backlight_apply();
    led_settings_dirty();
}

void k3yb_backlight_flame_toggle(void) {
    bl_flame = !bl_flame; /* never turns the backlight itself on */
    backlight_apply();
    led_settings_dirty();
}

void k3yb_status_flame_toggle(void) {
    status_flame = !status_flame;
    LOG_INF("status flame %s", status_flame ? "on" : "off");
    led_settings_dirty();
}

bool k3yb_backlight_is_on(void) { return bl_on; }
uint8_t k3yb_backlight_level_index(void) { return bl_idx; }
bool k3yb_backlight_flame_enabled(void) { return bl_flame; }
bool k3yb_status_flame_enabled(void) { return status_flame; }

static int led_mux_init(void) {
    if (!gpio_is_ready_dt(&sel_a) || !gpio_is_ready_dt(&sel_b) || !gpio_is_ready_dt(&inh)) {
        LOG_ERR("led mux GPIOs not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&sel_a, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&sel_b, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&inh, GPIO_OUTPUT_INACTIVE); /* inactive = inhibited, LEDs off */

    k_timer_start(&led_mux_timer, K_MSEC(STEP_MS), K_MSEC(STEP_MS));
    k_work_schedule(&led_mux_refresh_work, K_MSEC(STATE_REFRESH_MS));
    LOG_DBG("led mux started");
    return 0;
}

SYS_INIT(led_mux_init, APPLICATION, 99);
