/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * LED-control behavior: &ledctl <action>.  Purely local - drives the
 * k3yb LED controller API, never emits HID events to the host.
 * Actions are defined in include/dt-bindings/k3yb/led.h.
 *
 * The K3YB_LED_REC_* actions forward to the offline recorder
 * (src/recorder.c); the behavior itself still emits nothing - only the
 * recorder's playback, once started, sends HID.
 */

#define DT_DRV_COMPAT k3yb_behavior_led

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

#include <dt-bindings/k3yb/led.h>
#include <k3yb/led_ctrl.h>
#if IS_ENABLED(CONFIG_K3YB_RECORDER)
#include <k3yb/recorder.h>
#elif DT_NODE_EXISTS(DT_NODELABEL(recorder_partition))
/* the keymap binds K3YB_LED_REC_* and the flash is carved out: a silently
 * disabled recorder would turn R/P/DEL into no-ops */
#error "recorder_partition defined but CONFIG_K3YB_RECORDER is off"
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_led_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    switch (binding->param1) {
    case K3YB_LED_BL_TOGGLE:
        k3yb_backlight_toggle();
        break;
    case K3YB_LED_BL_UP:
        k3yb_backlight_level_up();
        break;
    case K3YB_LED_BL_DOWN:
        k3yb_backlight_level_down();
        break;
    case K3YB_LED_BL_FLAME:
        k3yb_backlight_flame_toggle();
        break;
    case K3YB_LED_ST_FLAME:
        k3yb_status_flame_toggle();
        break;
#if IS_ENABLED(CONFIG_K3YB_RECORDER)
    case K3YB_LED_REC_TOGGLE:
        k3yb_recorder_toggle();
        break;
    case K3YB_LED_REC_PLAY:
        k3yb_recorder_playback();
        break;
    case K3YB_LED_REC_CLEAR:
        k3yb_recorder_clear();
        break;
#endif
    default:
        LOG_WRN("unknown led action %d", binding->param1);
        break;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_led_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_led_driver_api = {
    .binding_pressed = on_led_binding_pressed,
    .binding_released = on_led_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int behavior_led_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#define LED_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_led_init, NULL, NULL, NULL, POST_KERNEL,                   \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_led_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LED_INST)

#endif
