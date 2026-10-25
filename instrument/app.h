/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1: a playable Chladni plate. This header is the app's half of the contract with fm1-base's platform
 * (device/unity.c, host/plat_host.c, web/emu/emu_web.c), as template/app.h lays it out, plus CYMA-1's own
 * accessors for the page (web.c). FROZEN CONTRACT: docs/CYMA-1-spec.md §9.7 (M7 owns it). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "CYMA-1"
#define APP_TAGLINE "A PLAYABLE CHLADNI PLATE"
#define APP_SPLASH_RGB RGB(255, 243, 222)
#define APP_BLOCK 128u                   /* == CY_BLOCK: 2.9 ms at 44.1 kHz, four control sub-blocks of 32 */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h): pressure IS the bow */
#define APP_CANVAS_PX (240u * 36u)       /* gfx.c's canvas: one knob-strip band (the plate box has its own strips) */
#define APP_GAIN(m) (m)                  /* MASTER reaches the render linear; fx.c squares it in float after the
                                          * limiter, so the taper costs no resolution in the 24-bit word */
#define APP_OVERLOAD
void app_overload(void);                 /* the render took over 90 % of its block: the governor sheds modes */
#define APP_SPLASH
void app_splash(void);                   /* the logo on paper, instead of fm1-base's name screen */
#define APP_REDRAW
void app_redraw(void);                   /* draw the whole screen again at the next frame */
#define APP_WEB                          /* web.c: the page's exports (web/emu/emu_web.c includes it) */
#define APP_BLE_AT_BOOT 0                /* fm1.toml ble = false: no radio */

/* the store: the settings (Felucca's settings pair), the panel calibration (the platform's), the user presets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_USER, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDA000u}
/* backup and restore over USB (fm1.toml backup = true; tools/fm1_backup.py, web/backup): the settings and the 32
 * user presets, all or nothing, never the panel's calibration (the unit's own). Back them up before installing
 * other firmware: the user bank sits inside V15's VM region */
#define APP_BACKUP_OBJS {OBJ_SETTINGS, OBJ_USER}
#define APP_BACKUP_MAGIC 0x314D5943u           /* "CYM1" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces user presets"
int app_backup_playing(void);            /* 1: a voice sounds or a key is held (STOP FIRST) */
void app_backup_flush(void);             /* a backup starts: what is not saved yet, saved */
void app_backup_hold(int on);            /* no flash writes while a backup streams */
void app_backup_restored(void);          /* everything from the flash again */
#define APP_SYSEX                        /* app_sounds.c: the sound transfer over USB (docs/CYMA-1-sounds-spec.md §5) */
int app_sysex(const uint8_t *p, uint32_t n);

/* the contract: the platform calls these */
void app_init(void);                     /* boot: geometry, fx, engine, then the saved settings (or defaults) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* input, MIDI, modulation, patch, LEDs, the UI: ~30-60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the host simulator's checks (host/plat_host.c "expect") and commands (spec §8.1) */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);

/* CYMA-1's accessors for the page (web.c, spec §7.5) and the host tests */
const uint8_t *cy_led_levels(void);       /* 42: keys 0..26, buttons 27..40 (B_* order), [41] PLAY green; 0..3 */
const uint8_t *cy_key_pressure(void);     /* 27: 0..127 */
const char *cy_info_json(uint32_t *seq);
uint32_t cy_demo_hash(int id);
