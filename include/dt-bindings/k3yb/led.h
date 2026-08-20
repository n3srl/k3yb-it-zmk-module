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
