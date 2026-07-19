/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Auto-repeat wrapper behavior: fires the wrapped behavior once when
 * pressed, then keeps re-firing it (press+release) while held, like OS
 * key repeat.  Used for the Italian accent macros, which being macros
 * produce no HID key the host could auto-repeat.
 */

#define DT_DRV_COMPAT k3yb_behavior_repeat

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_repeat_config {
    struct zmk_behavior_binding binding;
    int delay_ms;
    int rate_ms;
};

struct behavior_repeat_data {
    const struct device *dev;
    struct k_work_delayable work;
    struct zmk_behavior_binding_event event;
    bool active;
};

static void fire_once(const struct device *dev) {
    const struct behavior_repeat_config *cfg = dev->config;
    struct behavior_repeat_data *data = dev->data;
    struct zmk_behavior_binding_event event = data->event;

    event.timestamp = k_uptime_get();
    zmk_behavior_invoke_binding((struct zmk_behavior_binding *)&cfg->binding, event, true);
    zmk_behavior_invoke_binding((struct zmk_behavior_binding *)&cfg->binding, event, false);
}

static void repeat_work_cb(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct behavior_repeat_data *data = CONTAINER_OF(d_work, struct behavior_repeat_data, work);
    const struct behavior_repeat_config *cfg = data->dev->config;

    if (!data->active) {
        return;
    }
    fire_once(data->dev);
    k_work_schedule(&data->work, K_MSEC(cfg->rate_ms));
}

static int on_repeat_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_repeat_config *cfg = dev->config;
    struct behavior_repeat_data *data = dev->data;

    data->event = event;
    data->active = true;
    fire_once(dev);
    k_work_schedule(&data->work, K_MSEC(cfg->delay_ms));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_repeat_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_repeat_data *data = dev->data;

    data->active = false;
    k_work_cancel_delayable(&data->work);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_repeat_driver_api = {
    .binding_pressed = on_repeat_binding_pressed,
    .binding_released = on_repeat_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int behavior_repeat_init(const struct device *dev) {
    struct behavior_repeat_data *data = dev->data;

    data->dev = dev;
    k_work_init_delayable(&data->work, repeat_work_cb);
    return 0;
}

#define _TRANSFORM_ENTRY(idx, node)                                                                \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(node, bindings, idx)),               \
        .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0),       \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param1))),                  \
        .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0),       \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param2))),                  \
    }

#define REPEAT_INST(n)                                                                             \
    static const struct behavior_repeat_config behavior_repeat_config_##n = {                      \
        .binding = _TRANSFORM_ENTRY(0, n),                                                         \
        .delay_ms = DT_INST_PROP(n, delay_ms),                                                     \
        .rate_ms = DT_INST_PROP(n, rate_ms),                                                       \
    };                                                                                             \
    static struct behavior_repeat_data behavior_repeat_data_##n = {};                              \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_repeat_init, NULL, &behavior_repeat_data_##n,              \
                            &behavior_repeat_config_##n, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_repeat_driver_api);

DT_INST_FOREACH_STATUS_OKAY(REPEAT_INST)

#endif
