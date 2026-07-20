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

/* icon-mode variant: raw 16x16 bitmap icons on the top row instead of the
 * text markers.  Painted straight through display_write like the boot logo:
 * every font path was a dead end on this pipeline (LV_SYMBOL glyphs render
 * as boxes in unscii, Montserrat's 4bpp anti-aliased glyphs crash the
 * display thread). */
#define ICONS IS_ENABLED(CONFIG_K3YB_STATUS_ICONS)

#if IS_ENABLED(CONFIG_K3YB_STATUS_ICONS)

/* 16x16 1bpp icons, one uint16 per row, MSB = leftmost pixel */
static const uint16_t icon_usb[16] = {
    0x0100, 0x0380, 0x07C0, 0x0100, 0x0100, 0x1110, 0x1110, 0x3118,
    0x3118, 0x0100, 0x0100, 0x0100, 0x0380, 0x0380, 0x0000, 0x0000,
};
static const uint16_t icon_bt[16] = {
    0x0080, 0x00C0, 0x00A0, 0x0890, 0x0488, 0x02A0, 0x01C0, 0x0080,
    0x01C0, 0x02A0, 0x0488, 0x0890, 0x00A0, 0x00C0, 0x0080, 0x0000,
};
static const uint16_t icon_bolt[16] = {
    0x03C0, 0x0780, 0x0F00, 0x1E00, 0x3FF0, 0x7FE0, 0x03C0, 0x0780,
    0x0F00, 0x0E00, 0x1C00, 0x1800, 0x3000, 0x2000, 0x0000, 0x0000,
};
static const uint16_t icon_no_batt[16] = {
    0x0000, 0x0000, 0x4004, 0x600C, 0x3018, 0x1830, 0x0C60, 0x06C0,
    0x0380, 0x06C0, 0x0C60, 0x1830, 0x3018, 0x600C, 0x4004, 0x0000,
};

/* battery outline; interior filled at runtime based on charge level */
static void icon_battery(uint16_t rows[16], uint8_t soc) {
    /* body cols 0..12, tip cols 13..14 on the middle rows */
    const uint16_t side = 0x8008, tip = 0x0006;
    /* 11 fillable columns inside the body */
    const uint8_t w = (soc > 100 ? 100 : soc) * 11 / 100;
    const uint16_t fill = (uint16_t)(0x7FF0u << (11 - w)) & 0x7FF0;

    memset(rows, 0, 16 * sizeof(uint16_t));
    rows[3] = 0xFFF8;
    rows[4] = side;
    rows[5] = side | tip | fill;
    rows[6] = side | tip | fill;
    rows[7] = side | tip | fill;
    rows[8] = side | fill;
    rows[9] = 0xFFF8;
}

/* top-row canvas: 128 px wide, 16 px tall, 1bpp MSB-first */
static uint8_t icon_row[16 * 16];
static bool icons_ready; /* don't paint over the boot logo */

static void icon_blit(int x, const uint16_t rows[16]) {
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < 16; c++) {
            if (rows[r] & (0x8000 >> c)) {
                icon_row[r * 16 + (x + c) / 8] |= 0x80 >> ((x + c) % 8);
            }
        }
    }
}

static void icons_paint(void) {
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    const bool tall = lv_disp_get_ver_res(NULL) >= 128;
    struct display_buffer_descriptor desc = {
        .width = 128,
        .height = 16,
        .pitch = 128,
        .buf_size = sizeof(icon_row),
    };
    int x = 2;

    if (!icons_ready || !device_is_ready(disp)) {
        return;
    }

    memset(icon_row, 0, sizeof(icon_row));
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        icon_blit(x, icon_usb);
        x += 20;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
    if (zmk_ble_active_profile_is_connected()) {
        icon_blit(x, icon_bt);
        x += 20;
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        icon_blit(x, icon_bolt);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    {
        bool no_batt = false;
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
#if IS_ENABLED(CONFIG_ZMK_USB)
        no_batt = zmk_usb_is_powered() && mv > 4300;
#endif
        if (no_batt) {
            icon_blit(110, icon_no_batt);
        } else {
            uint16_t rows[16];

            icon_battery(rows, zmk_battery_state_of_charge());
            icon_blit(110, rows);
        }
    }
#endif

    display_write(disp, 0, tall ? 2 : 0, &desc, icon_row);
}

#endif /* CONFIG_K3YB_STATUS_ICONS */

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

    lv_label_set_text(layer_label, layer_display_name(zmk_keymap_highest_layer_active()));

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    {
        /* only the ACTIVE locks, uppercase; empty when none */
        uint8_t ind = zmk_hid_indicators_get_current_profile();
        static char locks[32]; /* 3 padded slots: up to 26 bytes incl. NUL */

        /* inactive locks become blanks so the row keeps its full width
         * and the remaining indicators don't shift around */
        locks[0] = '\0';
        strcat(locks, (ind & IND_NUMLOCK) ? "NUM " : "        ");
        strcat(locks, (ind & IND_CAPSLOCK) ? "CAPS " : "         ");
        strcat(locks, (ind & IND_SCROLLLOCK) ? "SCRL" : "        ");
        lv_label_set_text(locks_label, locks);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
    lv_label_set_text_fmt(wpm_label, "WPM %d", zmk_wpm_get_state());
#endif

    /* transports, top-left: USB and BT can show at the same time,
     * charge marker right after when powered.
     * Text markers by default; LV_SYMBOL_* glyphs behind
     * CONFIG_K3YB_STATUS_ICONS (glyph rendering crashed the display
     * thread before the stack/layout fixes - icons are a separate
     * firmware variant until proven stable). */
    if (!ICONS) {
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
            strcat(trans, "CH");
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
        bool no_batt = false;

#if IS_ENABLED(CONFIG_ZMK_USB)
        /* a real LiPo never exceeds ~4.2V; USB-powered with >4.3V on the
         * rail means the charger sees no battery */
        no_batt = zmk_usb_is_powered() && mv > 4300;
#endif
        if (no_batt) {
            snprintf(batt, sizeof(batt), "NO BATT");
        } else if (mv > 0) {
            snprintf(batt, sizeof(batt), "%s%d%% %d.%02dV", bsym, soc, mv / 1000,
                     (mv % 1000) / 10);
        } else {
            snprintf(batt, sizeof(batt), "%s%d%%", bsym, soc);
        }
        if (!ICONS) {
            lv_label_set_text(batt_label, batt);
        }
    }
#endif

#if IS_ENABLED(CONFIG_K3YB_STATUS_ICONS)
    icons_paint();
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
    lv_obj_clear_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(locks_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    if (!ICONS) {
        /* in icons mode the top row is raw-painted instead */
        lv_obj_clear_flag(batt_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(trans_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* NOTE: full-screen lv_obj_invalidate(screen_root) removed for now -
     * the full redraw kills the display thread even with a 4k stack.
     * The un-hidden labels invalidate their own areas; leftover logo
     * pixels get wiped by a raw clear instead. */
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

#if IS_ENABLED(CONFIG_K3YB_STATUS_ICONS)
    icons_ready = true;
#endif

    lv_timer_del(timer);
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    const bool tall = lv_disp_get_ver_res(NULL) >= 128;

    /* colors come from the default theme + CONFIG_ZMK_DISPLAY_INVERT
     * (manual color styles render inverted on the mono pipeline) */

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

    /* FIXED grid positions - no edge alignment.  Right/bottom-aligned
     * labels whose text changes each refresh sent LVGL's layout pass
     * into an endless loop, hanging the display thread on the first
     * render (the "only BASE shows" bug). */
    if (ICONS) {
        /* icons variant: the top row is raw-painted bitmaps (icons_paint),
         * so the trans/batt labels stay hidden and only the lower labels
         * get positions */
        if (tall) {
            lv_obj_set_pos(locks_label, 2, 44);
            lv_obj_set_pos(layer_label, 2, 66); /* montserrat 24 */
            lv_obj_set_pos(wpm_label, 2, 104);
        } else {
            lv_obj_set_pos(layer_label, 0, 18);
            lv_obj_set_pos(locks_label, 44, 18);
            lv_obj_set_pos(wpm_label, 86, 18);
        }
    } else if (tall) {
        /* 128x128 */
        lv_obj_set_pos(trans_label, 2, 2);
        lv_obj_set_pos(batt_label, 2, 22);
        lv_obj_set_pos(locks_label, 2, 44);
        lv_obj_set_pos(layer_label, 2, 66);  /* montserrat 24 */
        lv_obj_set_pos(wpm_label, 2, 104);
    } else {
        /* 128x32: two rows, left-anchored columns */
        lv_obj_set_pos(trans_label, 0, 0);
        lv_obj_set_pos(batt_label, 48, 0);
        lv_obj_set_pos(layer_label, 0, 18);
        lv_obj_set_pos(locks_label, 44, 18);
        lv_obj_set_pos(wpm_label, 86, 18);
    }

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
