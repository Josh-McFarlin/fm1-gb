/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* FM-GB: a Game Boy (DMG) emulator for the M-VAVE FM-1, on fm1-base. The game is the player's own ROM, put into the
 * firmware package when it is installed (tools/fm1gb.py, or the installer page): it sits at the end of the app slot,
 * found by its trailer (rom.c). This header is the app's half of the contract with the platform (device/unity.c,
 * host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FM-GB"
#define APP_SPLASH_RGB RGB(155, 188, 15)          /* the DMG's lightest green */
#define APP_BLOCK 256u                            /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_CANVAS_PX (240u * 48u)                /* the menu is drawn in bands 48 rows high */
#define FM1B_ERASE_SLICED 1                       /* a save's erases in ~1 ms windows, the sound playing on */

/* the store: the settings, the panel's calibration (the platform's). The bigger slot (fm1.toml app_end 0xE0000) leaves
 * no data area: both objects sit on Felucca's pairs. The game's saves are in XDATA1 (save.c) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xE5000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

/* the host simulator: expect fps / frames / rom / sav_writes ..., and "rom FILE" to load a game */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
