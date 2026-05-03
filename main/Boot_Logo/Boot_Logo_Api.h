#ifndef BOOT_LOGO_API_H
#define BOOT_LOGO_API_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/** Registers LVGL filesystem drive 'A:' -> POSIX /storage/... Safe to call more than once. */
void boot_logo_lvgl_fs_register(void);

/** startup_timing: full splash (backlight sequence + long decode waits). False = carousel page. */
esp_err_t boot_logo_display(lv_obj_t *scr, bool startup_timing);
void boot_logo_enable_backlight(uint8_t brightness);

#endif
