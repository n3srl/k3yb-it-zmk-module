/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Custom ZMK status screen for the 128x128 SSD1327 OLED.
 *
 * Layout:
 *   boot:   "N3" logo for ~2.5 s
 *   status: uptime clock, active layer, NUM/CAPS/SCRL lock states,
 *           WPM, battery percentage
 *
 * State is polled from a 500 ms LVGL timer running on the display work
 * queue, which keeps the code free of event-listener boilerplate.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>

#include <zephyr/logging/log.h>
#include <zmk/display.h>
#include <zmk/keymap.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif

#define LOGO_MS 2500
#define REFRESH_MS 500

/* raw display-native logo bitmaps (see src/n3_logo*.c); written straight
 * through display_write, bypassing LVGL image decoding entirely */
extern const uint8_t n3_logo128_raw[]; /* 128x128 row-major MSB-first */
extern const uint8_t n3_logo32_raw[];  /* 128x32 SSD1306 vtiled */

/* HID keyboard LED usage bits */
#define IND_NUMLOCK BIT(0)
#define IND_CAPSLOCK BIT(1)
#define IND_SCROLLLOCK BIT(2)

static lv_obj_t *screen_root;
static lv_obj_t *layer_label;
static lv_obj_t *locks_label;
static lv_obj_t *wpm_label;
static lv_obj_t *batt_label;
static lv_obj_t *trans_label;

static const char *layer_display_name(uint8_t idx) {
    switch (idx) {
    case 0:
        return "BASE";
    case 1:
        return "PAD";
    case 2:
        return "GRAVE `";
    case 3:
        return "ACUTO '";
    default:
        return "?";
    }
}

static void refresh_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    LOG_DBG("ui tick");
#if IS_ENABLED(CONFIG_K3YB_STATUS_MINIMAL)
    /* bisect mode: plain-text only, no symbols, no sensors */
    lv_label_set_text(layer_label, layer_display_name(zmk_keymap_highest_layer_active()));
    lv_label_set_text_fmt(batt_label, "B %d", zmk_battery_state_of_charge());
    return;
#endif
    lv_label_set_text(layer_label, layer_display_name(zmk_keymap_highest_layer_active()));

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    {
        /* only the ACTIVE locks, uppercase; empty when none */
        uint8_t ind = zmk_hid_indicators_get_current_profile();
        static char locks[20];

        locks[0] = '\0';
        if (ind & IND_NUMLOCK) {
            strcat(locks, "NUM ");
        }
        if (ind & IND_CAPSLOCK) {
            strcat(locks, "CAPS ");
        }
        if (ind & IND_SCROLLLOCK) {
            strcat(locks, "SCRL");
        }
        lv_label_set_text(locks_label, locks);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
    lv_label_set_text_fmt(wpm_label, "WPM %d", zmk_wpm_get_state());
#endif

    /* transports, top-left: USB and BT can show at the same time,
     * charge marker right after when powered.
     * NOTE: plain text on purpose - rendering LV_SYMBOL_* glyphs
     * crashes the display thread on this setup. */
    {
        static char trans[24];

        trans[0] = '\0';
#if IS_ENABLED(CONFIG_ZMK_USB)
        if (zmk_usb_is_powered()) {
            strcat(trans, "USB ");
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (zmk_ble_active_profile_is_connected()) {
            strcat(trans, "BT ");
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
        if (zmk_usb_is_powered()) {
            strcat(trans, "*");
        }
#endif
        lv_label_set_text(trans_label, trans);
    }

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    {
        /* battery percent + voltage, top-right (text only) */
        uint8_t soc = zmk_battery_state_of_charge();
        const char *bsym = "";
        static char batt[40];
        int mv = -1;

#if DT_HAS_CHOSEN(zmk_battery) && !IS_ENABLED(CONFIG_K3YB_STATUS_NO_VOLTAGE)
        {
            static const struct device *batt_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
            struct sensor_value val;

            if (device_is_ready(batt_dev) &&
                sensor_channel_get(batt_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &val) == 0) {
                mv = val.val1 * 1000 + val.val2 / 1000;
            }
        }
#endif
        if (mv > 0) {
            snprintf(batt, sizeof(batt), "%s%d%% %d.%02dV", bsym, soc, mv / 1000,
                     (mv % 1000) / 10);
        } else {
            snprintf(batt, sizeof(batt), "%s%d%%", bsym, soc);
        }
        lv_label_set_text(batt_label, batt);
    }
#endif
}

/* paint the boot logo straight into the panel, after LVGL's first flush */
static void logo_paint_cb(lv_timer_t *timer) {
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    const bool tall = lv_disp_get_ver_res(NULL) >= 128;
    struct display_buffer_descriptor desc = {
        .width = 128,
        .height = tall ? 128 : 32,
        .pitch = 128,
        .buf_size = tall ? 2048 : 512,
    };

    if (device_is_ready(disp)) {
        display_write(disp, 0, 0, &desc, tall ? n3_logo128_raw : n3_logo32_raw);
    }
    lv_timer_del(timer);
}

static void logo_done_cb(lv_timer_t *timer) {
    LOG_DBG("logo_done: step 1 (clear flags)");
    lv_obj_clear_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(locks_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(trans_label, LV_OBJ_FLAG_HIDDEN);

    /* NOTE: full-screen lv_obj_invalidate(screen_root) removed for now -
     * the full redraw kills the display thread even with a 4k stack.
     * The un-hidden labels invalidate their own areas; leftover logo
     * pixels get wiped by a raw clear instead. */
    LOG_DBG("logo_done: step 2 (raw clear)");
    {
        const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
        const bool tall = lv_disp_get_ver_res(NULL) >= 128;
        static const uint8_t zeros[2048];
        struct display_buffer_descriptor desc = {
            .width = 128,
            .height = tall ? 128 : 32,
            .pitch = 128,
            .buf_size = tall ? 2048 : 512,
        };

        if (device_is_ready(disp)) {
            display_write(disp, 0, 0, &desc, zeros);
        }
    }

    LOG_DBG("logo_done: step 3 (timer del)");
    lv_timer_del(timer);
    LOG_DBG("logo_done: done");
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    const bool tall = lv_disp_get_ver_res(NULL) >= 128;

    layer_label = lv_label_create(screen);
    lv_label_set_text(layer_label, "BASE");
    locks_label = lv_label_create(screen);
    lv_label_set_text(locks_label, "");
    wpm_label = lv_label_create(screen);
    lv_label_set_text(wpm_label, "WPM 0");
    batt_label = lv_label_create(screen);
    lv_label_set_text(batt_label, "--%");
    trans_label = lv_label_create(screen);
    lv_label_set_text(trans_label, "");

    screen_root = screen;

    /* Same scheme on both panels:
     *   top-left:  USB / BT symbols (+ charge bolt)
     *   top-right: battery symbol + percent + voltage
     *   mid-right: active locks (uppercase, empty when none)
     *   bottom-left:  active layer
     *   bottom-right: WPM
     */
    lv_obj_align(trans_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(batt_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_align(locks_label, LV_ALIGN_RIGHT_MID, 0, tall ? 0 : 2);
    lv_obj_align(layer_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_align(wpm_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_add_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(locks_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(trans_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(logo_paint_cb, 300, NULL);
    lv_timer_create(logo_done_cb, LOGO_MS, NULL);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    return screen;
}
