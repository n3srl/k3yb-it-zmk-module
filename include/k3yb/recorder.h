/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Offline keystroke recorder (implemented in src/recorder.c).
 *
 * Records key presses (modifier snapshot + HID usage, no timing) to a
 * dedicated internal-flash partition, so notes typed on battery with no
 * host attached survive power-off, reset and battery removal.  Playback
 * re-sends the raw keycodes through the normal ZMK HID path.
 *
 * Driven by &ledctl K3YB_LED_REC_* (include/dt-bindings/k3yb/led.h).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

void k3yb_recorder_toggle(void);   /* REC on/off */
void k3yb_recorder_playback(void); /* start playback, or stop a running one */
void k3yb_recorder_clear(void);    /* erase the buffer (stops REC first) */

/* read-only state, for display/debug */
bool k3yb_recorder_is_recording(void);
bool k3yb_recorder_is_playing(void);
uint32_t k3yb_recorder_count(void);    /* events stored */
uint32_t k3yb_recorder_capacity(void); /* events that fit */
