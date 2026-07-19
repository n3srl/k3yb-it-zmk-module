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
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/keymap.h>
#include <zmk/endpoints.h>

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

extern const lv_img_dsc_t n3_logo;    /* 128x128 */
extern const lv_img_dsc_t n3_logo_32; /* 128x32 */

/* HID keyboard LED usage bits */
#define IND_NUMLOCK BIT(0)
#define IND_CAPSLOCK BIT(1)
#define IND_SCROLLLOCK BIT(2)

static lv_obj_t *logo_label;
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
     * charge bolt right after when powered */
    {
        static char trans[24];

        trans[0] = '\0';
#if IS_ENABLED(CONFIG_ZMK_USB)
        if (zmk_usb_is_powered()) {
            strcat(trans, LV_SYMBOL_USB " ");
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (zmk_ble_active_profile_is_connected()) {
            strcat(trans, LV_SYMBOL_BLUETOOTH " ");
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
        if (zmk_usb_is_powered()) {
            strcat(trans, LV_SYMBOL_CHARGE);
        }
#endif
        lv_label_set_text(trans_label, trans);
    }

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    {
        /* battery symbol + percent + voltage, top-right */
        uint8_t soc = zmk_battery_state_of_charge();
        const char *bsym = (soc > 87)   ? LV_SYMBOL_BATTERY_FULL
                           : (soc > 62) ? LV_SYMBOL_BATTERY_3
                           : (soc > 37) ? LV_SYMBOL_BATTERY_2
                           : (soc > 12) ? LV_SYMBOL_BATTERY_1
                                        : LV_SYMBOL_BATTERY_EMPTY;
        static char batt[40];
        int mv = -1;

#if DT_HAS_CHOSEN(zmk_battery)
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
            snprintf(batt, sizeof(batt), "%s %d%% %d.%02dV", bsym, soc, mv / 1000,
                     (mv % 1000) / 10);
        } else {
            snprintf(batt, sizeof(batt), "%s %d%%", bsym, soc);
        }
        lv_label_set_text(batt_label, batt);
    }
#endif
}

static void logo_done_cb(lv_timer_t *timer) {
    lv_obj_add_flag(logo_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(locks_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(trans_label, LV_OBJ_FLAG_HIDDEN);

    lv_timer_del(timer);
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

    /* boot logo, sized per panel; ALPHA_1BIT images take their colour
     * from the recolor style */
    logo_label = lv_img_create(screen);
    lv_img_set_src(logo_label, tall ? &n3_logo : &n3_logo_32);
    lv_obj_set_style_img_recolor(logo_label, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(logo_label, LV_OPA_COVER, 0);
    lv_obj_align(logo_label, LV_ALIGN_CENTER, 0, 0);

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
    lv_timer_create(logo_done_cb, LOGO_MS, NULL);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    return screen;
}
