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

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_MUX_NODE DT_INST(0, k3yb_led_mux)

#define STEP_MS 1
#define STATE_REFRESH_MS 100

#define IND_NUMLOCK BIT(0)
#define IND_CAPSLOCK BIT(1)
#define IND_SCROLLLOCK BIT(2)

static const struct gpio_dt_spec sel_a = GPIO_DT_SPEC_GET(LED_MUX_NODE, a_gpios);
static const struct gpio_dt_spec sel_b = GPIO_DT_SPEC_GET(LED_MUX_NODE, b_gpios);
static const struct gpio_dt_spec inh = GPIO_DT_SPEC_GET(LED_MUX_NODE, inh_gpios);

static volatile uint8_t led_states; /* bit n = LED Yn on */

static void led_mux_tick(struct k_timer *timer) {
    static uint8_t idx;

    ARG_UNUSED(timer);
    idx = (idx + 1) & 0x3;

    /* blank while switching the selects, then gate on if this LED is lit */
    gpio_pin_set_dt(&inh, 0);
    gpio_pin_set_dt(&sel_a, idx & 0x1);
    gpio_pin_set_dt(&sel_b, (idx >> 1) & 0x1);
    if (led_states & BIT(idx)) {
        gpio_pin_set_dt(&inh, 1); /* logical 1 = enabled (inh is active-low wired) */
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
    /* Y3: accent layer (grave = 2, acute = 3) currently active */
    if (zmk_keymap_highest_layer_active() >= 2) {
        states |= BIT(3);
    }

    led_states = states;
    k_work_schedule(&led_mux_refresh_work, K_MSEC(STATE_REFRESH_MS));
}

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
