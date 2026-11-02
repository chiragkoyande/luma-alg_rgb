/* SPDX-License-Identifier: GPL-2.0
 *
 * Shared, compile-time constants for the Acer ALG CLV0001 RGB interface.
 *
 * Only _DSM function 0x67 is used.  The neighbouring functions 0x68-0x6A
 * are deliberately not exposed: one of them was observed to hard-lock an
 * AL15G-53 during reverse engineering.
 */
#pragma once

#define ALG_DSM_UUID_STR "93f224e4-fbdc-4bbf-add6-db71bdc0afad"
#define ALG_DSM_FUNCTION 0x67

/* Zone / command byte values in the fourth payload byte. */
#define ALG_ZONE_MAIN 0xF0
#define ALG_ZONE_LEFT 0xF0
#define ALG_ZONE_MID 0xF1
#define ALG_ZONE_RIGHT 0xF2
#define ALG_ZONE_BAR 0xF3
#define ALG_ZONE_BRIGHTNESS 0xF4
#define ALG_ZONE_LOGO 0xF6

#define ALG_BRIGHTNESS_MAX 4

/* Not exported to userspace as raw packets; listed for documentation. */
#define ALG_EFFECT_RANDOM 0x70
#define ALG_EFFECT_FLASH 0xA0
#define ALG_EFFECT_TEMPO 0x90
#define ALG_EFFECT_DANCE 0x80
#define ALG_EFFECT_WAVE 0xB0
