/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI stub (M5): the app's half of fm1-base's contract (template/app.h) for a tiny app that runs the real UI
 * (instrument/ui/) against a fake engine and a fake object builder. Host only. Same screen geometry and canvas as
 * the instrument's app.h, so the UI behaves exactly as it will there. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "CYMA-UI"
#define APP_TAGLINE "A PLAYABLE CHLADNI PLATE"
#define APP_SPLASH_RGB RGB(255, 243, 222)
#define APP_BLOCK 128u
#define APP_CANVAS_PX (240u * 36u)       /* as the instrument: gfx.c's canvas is one knob-strip band */
#define APP_SPLASH
void app_splash(void);
#define APP_REDRAW
void app_redraw(void);
#define APP_BLE_AT_BOOT 0

enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
