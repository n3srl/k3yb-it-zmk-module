/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Fork of ZMK's kscan_gpio_demux driver (v0.3) for the k3yb.it shield.
 *
 * Changes vs upstream:
 *  - Inputs are actively discharged (driven to inactive level, then released
 *    back to input) after each demux address change.  With high-impedance
 *    pull-downs alone, residual charge on the row nets survives into the next
 *    address slot and reads as ghost keys on scan-order neighbour columns.
 *  - Settle time between address change and read is configurable via the
 *    settle-time-us devicetree property (default 10).
 */

#define DT_DRV_COMPAT k3yb_kscan_gpio_demux

#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Helper macro
#define PWR_TWO(x) (1 << (x))

// Define row and col cfg
#define _KSCAN_GPIO_CFG_INIT(n, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(n, prop, idx),

// Check debounce config
#define CHECK_DEBOUNCE_CFG(n, a, b) COND_CODE_0(DT_INST_PROP(n, debounce_period), a, b)

// Define the row and column lengths
#define INST_MATRIX_INPUTS(n) DT_INST_PROP_LEN(n, input_gpios)
#define INST_DEMUX_GPIOS(n) DT_INST_PROP_LEN(n, output_gpios)
#define INST_MATRIX_OUTPUTS(n) PWR_TWO(INST_DEMUX_GPIOS(n))
#define POLL_INTERVAL(n) DT_INST_PROP(n, polling_interval_msec)
#define SETTLE_TIME_US(n) DT_INST_PROP(n, settle_time_us)

#define GPIO_INST_INIT(n)                                                                          \
    struct kscan_gpio_config_##n {                                                                 \
        const struct gpio_dt_spec rows[INST_MATRIX_INPUTS(n)];                                     \
        const struct gpio_dt_spec cols[INST_DEMUX_GPIOS(n)];                                       \
    };                                                                                             \
                                                                                                   \
    struct kscan_gpio_data_##n {                                                                   \
        kscan_callback_t callback;                                                                 \
        struct k_timer poll_timer;                                                                 \
        struct CHECK_DEBOUNCE_CFG(n, (k_work), (k_work_delayable)) work;                           \
        bool matrix_state[INST_MATRIX_INPUTS(n)][INST_MATRIX_OUTPUTS(n)];                          \
        const struct device *dev;                                                                  \
    };                                                                                             \
    static const struct gpio_dt_spec *kscan_gpio_input_specs_##n(const struct device *dev) {       \
        const struct kscan_gpio_config_##n *cfg = dev->config;                                     \
        return cfg->rows;                                                                          \
    }                                                                                              \
                                                                                                   \
    static const struct gpio_dt_spec *kscan_gpio_output_specs_##n(const struct device *dev) {      \
        const struct kscan_gpio_config_##n *cfg = dev->config;                                     \
        return cfg->cols;                                                                          \
    }                                                                                              \
    static void kscan_gpio_timer_handler(struct k_timer *timer) {                                  \
        struct kscan_gpio_data_##n *data =                                                         \
            CONTAINER_OF(timer, struct kscan_gpio_data_##n, poll_timer);                           \
        k_work_submit(&data->work.work);                                                           \
    }                                                                                              \
                                                                                                   \
    static int kscan_gpio_read_##n(const struct device *dev) {                                     \
        bool submit_follow_up_read = false;                                                        \
        struct kscan_gpio_data_##n *data = dev->data;                                              \
        static bool read_state[INST_MATRIX_INPUTS(n)][INST_MATRIX_OUTPUTS(n)];                     \
        for (int o = 0; o < INST_MATRIX_OUTPUTS(n); o++) {                                         \
            for (uint8_t bit = 0; bit < INST_DEMUX_GPIOS(n); bit++) {                              \
                uint8_t state = (o & (0b1 << bit)) >> bit;                                         \
                const struct gpio_dt_spec *out_spec = &kscan_gpio_output_specs_##n(dev)[bit];      \
                gpio_pin_set_dt(out_spec, state);                                                  \
            }                                                                                      \
            /* Actively discharge the input nets: drive them to the inactive  */                   \
            /* level, then release back to input.  Kills residual charge left */                   \
            /* over from the previous address slot.                           */                   \
            for (int i = 0; i < INST_MATRIX_INPUTS(n); i++) {                                      \
                const struct gpio_dt_spec *in_spec = &kscan_gpio_input_specs_##n(dev)[i];          \
                gpio_pin_configure_dt(in_spec, GPIO_OUTPUT_INACTIVE);                              \
            }                                                                                      \
            for (int i = 0; i < INST_MATRIX_INPUTS(n); i++) {                                      \
                const struct gpio_dt_spec *in_spec = &kscan_gpio_input_specs_##n(dev)[i];          \
                gpio_pin_configure_dt(in_spec, GPIO_INPUT);                                        \
            }                                                                                      \
            /* Let the selected column charge the pressed row before reading */                    \
            k_busy_wait(SETTLE_TIME_US(n));                                                        \
                                                                                                   \
            for (int i = 0; i < INST_MATRIX_INPUTS(n); i++) {                                      \
                const struct gpio_dt_spec *in_spec = &kscan_gpio_input_specs_##n(dev)[i];          \
                read_state[i][o] = gpio_pin_get_dt(in_spec) > 0;                                   \
            }                                                                                      \
        }                                                                                          \
        for (int r = 0; r < INST_MATRIX_INPUTS(n); r++) {                                          \
            for (int c = 0; c < INST_MATRIX_OUTPUTS(n); c++) {                                     \
                bool pressed = read_state[r][c];                                                   \
                submit_follow_up_read = (submit_follow_up_read || pressed);                        \
                if (pressed != data->matrix_state[r][c]) {                                         \
                    LOG_DBG("Sending event at %d,%d state %s", r, c, (pressed ? "on" : "off"));    \
                    data->matrix_state[r][c] = pressed;                                            \
                    data->callback(dev, r, c, pressed);                                            \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        if (submit_follow_up_read) {                                                               \
            CHECK_DEBOUNCE_CFG(n, ({ k_work_submit(&data->work); }),                               \
                               ({ k_work_reschedule(&data->work, K_MSEC(5)); }))                   \
        }                                                                                          \
        return 0;                                                                                  \
    }                                                                                              \
                                                                                                   \
    static void kscan_gpio_work_handler_##n(struct k_work *work) {                                 \
        struct k_work_delayable *d_work = k_work_delayable_from_work(work);                        \
        struct kscan_gpio_data_##n *data = CONTAINER_OF(d_work, struct kscan_gpio_data_##n, work); \
        kscan_gpio_read_##n(data->dev);                                                            \
    }                                                                                              \
                                                                                                   \
    static struct kscan_gpio_data_##n kscan_gpio_data_##n = {};                                    \
                                                                                                   \
    static int kscan_gpio_configure_##n(const struct device *dev, kscan_callback_t callback) {     \
        struct kscan_gpio_data_##n *data = dev->data;                                              \
        if (!callback) {                                                                           \
            return -EINVAL;                                                                        \
        }                                                                                          \
        data->callback = callback;                                                                 \
        return 0;                                                                                  \
    };                                                                                             \
                                                                                                   \
    static int kscan_gpio_enable_##n(const struct device *dev) {                                   \
        struct kscan_gpio_data_##n *data = dev->data;                                              \
        k_timer_start(&data->poll_timer, K_MSEC(POLL_INTERVAL(n)), K_MSEC(POLL_INTERVAL(n)));      \
        return 0;                                                                                  \
    };                                                                                             \
                                                                                                   \
    static int kscan_gpio_disable_##n(const struct device *dev) {                                  \
        struct kscan_gpio_data_##n *data = dev->data;                                              \
        k_timer_stop(&data->poll_timer);                                                           \
        return 0;                                                                                  \
    };                                                                                             \
                                                                                                   \
    static int kscan_gpio_init_##n(const struct device *dev) {                                     \
        LOG_DBG("KSCAN GPIO init (k3yb demux w/ settle)");                                         \
        struct kscan_gpio_data_##n *data = dev->data;                                              \
        int err;                                                                                   \
        for (int i = 0; i < INST_MATRIX_INPUTS(n); i++) {                                          \
            const struct gpio_dt_spec *in_spec = &kscan_gpio_input_specs_##n(dev)[i];              \
            if (!device_is_ready(in_spec->port)) {                                                 \
                LOG_ERR("Unable to find input GPIO device");                                       \
                return -EINVAL;                                                                    \
            }                                                                                      \
            err = gpio_pin_configure_dt(in_spec, GPIO_INPUT);                                      \
            if (err) {                                                                             \
                LOG_ERR("Unable to configure pin %d for input", in_spec->pin);                     \
                return err;                                                                        \
            }                                                                                      \
        }                                                                                          \
        for (int o = 0; o < INST_DEMUX_GPIOS(n); o++) {                                            \
            const struct gpio_dt_spec *out_spec = &kscan_gpio_output_specs_##n(dev)[o];            \
            if (!device_is_ready(out_spec->port)) {                                                \
                LOG_ERR("Unable to find output GPIO device");                                      \
                return -EINVAL;                                                                    \
            }                                                                                      \
            err = gpio_pin_configure_dt(out_spec, GPIO_OUTPUT_ACTIVE);                             \
            if (err) {                                                                             \
                LOG_ERR("Unable to configure pin %d for output", out_spec->pin);                   \
                return err;                                                                        \
            }                                                                                      \
        }                                                                                          \
        data->dev = dev;                                                                           \
                                                                                                   \
        k_timer_init(&data->poll_timer, kscan_gpio_timer_handler, NULL);                           \
                                                                                                   \
        (CHECK_DEBOUNCE_CFG(n, (k_work_init), (k_work_init_delayable)))(                           \
            &data->work, kscan_gpio_work_handler_##n);                                             \
        return 0;                                                                                  \
    }                                                                                              \
                                                                                                   \
    static const struct kscan_driver_api gpio_driver_api_##n = {                                   \
        .config = kscan_gpio_configure_##n,                                                        \
        .enable_callback = kscan_gpio_enable_##n,                                                  \
        .disable_callback = kscan_gpio_disable_##n,                                                \
    };                                                                                             \
                                                                                                   \
    static const struct kscan_gpio_config_##n kscan_gpio_config_##n = {                            \
        .rows = {DT_FOREACH_PROP_ELEM(DT_DRV_INST(n), input_gpios, _KSCAN_GPIO_CFG_INIT)},         \
        .cols = {DT_FOREACH_PROP_ELEM(DT_DRV_INST(n), output_gpios, _KSCAN_GPIO_CFG_INIT)},        \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, kscan_gpio_init_##n, NULL, &kscan_gpio_data_##n,                      \
                          &kscan_gpio_config_##n, POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,         \
                          &gpio_driver_api_##n);

DT_INST_FOREACH_STATUS_OKAY(GPIO_INST_INIT)
