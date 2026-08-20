/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Public API of the k3yb LED controller (implemented in src/led_mux.c).
 * Small on purpose: enough for the LED-control behaviors today and for a
 * future OLED status page, nothing more.
 *
 * All state lives in the LED driver; these calls never emit HID events.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* backlight (105 per-key LEDs - future hardware revision, see README) */
void k3yb_backlight_toggle(void);
void k3yb_backlight_level_up(void);
void k3yb_backlight_level_down(void);
void k3yb_backlight_flame_toggle(void);

/* flame effect on the four status indicators */
void k3yb_status_flame_toggle(void);

/* read-only state, for display/debug */
bool k3yb_backlight_is_on(void);
uint8_t k3yb_backlight_level_index(void);
uint8_t k3yb_backlight_level_pwm(void); /* 0-255, already capped */
bool k3yb_backlight_flame_enabled(void);
bool k3yb_status_flame_enabled(void);
