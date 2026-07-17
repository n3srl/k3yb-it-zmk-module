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

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif

#define LOGO_MS 2500
#define REFRESH_MS 500

/* HID keyboard LED usage bits */
#define IND_NUMLOCK BIT(0)
#define IND_CAPSLOCK BIT(1)
#define IND_SCROLLLOCK BIT(2)

static lv_obj_t *logo_label;
static lv_obj_t *clock_label;
static lv_obj_t *layer_label;
static lv_obj_t *locks_label;
static lv_obj_t *wpm_label;
static lv_obj_t *batt_label;

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

    /* uptime clock HH:MM:SS */
    int64_t secs = k_uptime_get() / 1000;
    lv_label_set_text_fmt(clock_label, "%02d:%02d:%02d", (int)(secs / 3600),
                          (int)((secs / 60) % 60), (int)(secs % 60));

    lv_label_set_text(layer_label, layer_display_name(zmk_keymap_highest_layer_active()));

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    {
        uint8_t ind = zmk_hid_indicators_get_current_profile();
        static char locks[20];

        snprintf(locks, sizeof(locks), "%s %s %s", (ind & IND_NUMLOCK) ? "NUM" : "num",
                 (ind & IND_CAPSLOCK) ? "CAPS" : "caps", (ind & IND_SCROLLLOCK) ? "SCRL" : "scrl");
        lv_label_set_text(locks_label, locks);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
    lv_label_set_text_fmt(wpm_label, "WPM %d", zmk_wpm_get_state());
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    lv_label_set_text_fmt(batt_label, "BAT %d%%", zmk_battery_state_of_charge());
#endif
}

static void logo_done_cb(lv_timer_t *timer) {
    lv_obj_add_flag(logo_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(locks_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_label, LV_OBJ_FLAG_HIDDEN);

    lv_timer_del(timer);
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* ---- boot logo ---- */
    logo_label = lv_label_create(screen);
    lv_label_set_text(logo_label, "N3");
#if IS_ENABLED(CONFIG_LV_FONT_MONTSERRAT_48)
    lv_obj_set_style_text_font(logo_label, &lv_font_montserrat_48, 0);
#endif
    lv_obj_align(logo_label, LV_ALIGN_CENTER, 0, 0);

    /* ---- status widgets (hidden until the logo is done) ---- */
    clock_label = lv_label_create(screen);
#if IS_ENABLED(CONFIG_LV_FONT_MONTSERRAT_24)
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_24, 0);
#endif
    lv_label_set_text(clock_label, "00:00:00");
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, 4);

    layer_label = lv_label_create(screen);
    lv_label_set_text(layer_label, "BASE");
    lv_obj_align(layer_label, LV_ALIGN_TOP_MID, 0, 38);

    locks_label = lv_label_create(screen);
    lv_label_set_text(locks_label, "num caps scrl");
    lv_obj_align(locks_label, LV_ALIGN_TOP_MID, 0, 62);

    wpm_label = lv_label_create(screen);
    lv_label_set_text(wpm_label, "WPM 0");
    lv_obj_align(wpm_label, LV_ALIGN_TOP_MID, 0, 86);

    batt_label = lv_label_create(screen);
    lv_label_set_text(batt_label, "BAT --%");
    lv_obj_align(batt_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(locks_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_label, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(logo_done_cb, LOGO_MS, NULL);
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);

    return screen;
}
