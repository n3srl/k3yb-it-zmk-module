/*
 * Copyright (c) 2026 k3yb.it
 * SPDX-License-Identifier: MIT
 *
 * Parameters for the k3yb,behavior-led binding (&ledctl <action>).
 */

#pragma once

#define K3YB_LED_BL_TOGGLE 0 /* backlight on/off, keeps the level */
#define K3YB_LED_BL_UP 1     /* next brightness step */
#define K3YB_LED_BL_DOWN 2   /* previous brightness step (0 = off) */
#define K3YB_LED_BL_FLAME 3  /* flame effect on the backlight only */
#define K3YB_LED_ST_FLAME 4  /* flame effect on the 4 status LEDs only */

/* offline keystroke recorder (src/recorder.c) - dispatched by &ledctl so
 * the recorder lives in the same local layer; only PLAYBACK emits HID */
#define K3YB_LED_REC_TOGGLE 5 /* start/stop recording key presses to flash */
#define K3YB_LED_REC_PLAY 6   /* replay the buffer via HID (press again = stop) */
#define K3YB_LED_REC_CLEAR 7  /* erase the buffer */
