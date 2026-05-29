#ifndef BOOT_LOGO_API_H
#define BOOT_LOGO_API_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/**
 * Registers LVGL filesystem letter 'A:' → POSIX `/storage/` (ESP-IDF SPIFFS).
 * Safe to call more than once. Call early (e.g. from LVGL_Init after lv_init()).
 */
void lvgl_spiffs_assets_fs_register(void);

/** Read the first 4 bytes of an LVGL 8 .bin from SPIFFS into `header` (same layout as on-disk). */
bool lvgl_bin_read_header_from_spiffs(const char *spiffs_path, lv_img_header_t *header);

/**
 * Load a full .bin into SPIRAM and fill `dsc` for LV_IMG_SRC_VARIABLE (fast redraw).
 * On success, `*ram_out` owns the allocation (header + pixels); keep until reset.
 */
esp_err_t lvgl_bin_load_to_dsc_from_spiffs(const char *spiffs_path, lv_img_dsc_t *dsc, uint8_t **ram_out);

/** startup_timing: full splash (backlight sequence + decode waits). False = carousel page. */
esp_err_t boot_logo_display(lv_obj_t *scr, bool startup_timing);
/** Free splash PSRAM (~250 KiB) after handoff so BLE/WiFi can allocate. */
void boot_logo_release_ram_cache(void);
void boot_logo_enable_backlight(uint8_t brightness);

#endif
