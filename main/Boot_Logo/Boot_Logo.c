#include "Boot_Logo.h"
#include "ST77916.h"
#include "Storage_Manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "Boot_Logo";

// LVGL file system driver for SPIFFS using POSIX
static void *fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    // LVGL passes path like "/Logos/file.png" (drive letter already stripped)
    // We need to prepend "/storage" but avoid double slashes
    // Skip any leading slashes from the path
    const char *path_clean = path;
    while (*path_clean == '/') {
        path_clean++;  // Skip all leading slashes
    }
    
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/storage/%s", path_clean);
    
    const char *mode_str = "r";
    if (mode == LV_FS_MODE_WR) {
        mode_str = "w";
    } else if (mode == (LV_FS_MODE_WR | LV_FS_MODE_RD)) {
        mode_str = "r+";
    }
    
    FILE *f = fopen(full_path, mode_str);
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s (mode: %s) - tried LVGL path: %s", full_path, mode_str, path);
    }
    // Don't log successful opens - PNG decoder opens file many times during decoding
    return (void *)f;
}

static lv_fs_res_t fs_close(lv_fs_drv_t *drv, void *file_p)
{
    FILE *f = (FILE *)file_p;
    fclose(f);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    FILE *f = (FILE *)file_p;
    if (f == NULL) {
        *br = 0;
        return LV_FS_RES_INV_PARAM;
    }
    *br = fread(buf, 1, btr, f);
    if (*br == 0 && ferror(f)) {
        return LV_FS_RES_UNKNOWN;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    FILE *f = (FILE *)file_p;
    int w = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
    else if (whence == LV_FS_SEEK_END) w = SEEK_END;
    fseek(f, pos, w);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    FILE *f = (FILE *)file_p;
    *pos_p = ftell(f);
    return LV_FS_RES_OK;
}

static void register_lvgl_fs_driver(void)
{
    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);
    
    fs_drv.letter = 'A';
    fs_drv.cache_size = 0;
    fs_drv.open_cb = fs_open;
    fs_drv.close_cb = fs_close;
    fs_drv.read_cb = fs_read;
    fs_drv.seek_cb = fs_seek;
    fs_drv.tell_cb = fs_tell;
    
    lv_fs_drv_register(&fs_drv);
}

esp_err_t boot_logo_display(void)
{
    // Ensure backlight is off
    Set_Backlight(0);
    
    // Register LVGL file system driver for SPIFFS
    register_lvgl_fs_driver();
    
    // Clear screen to black
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_invalidate(scr);

    // Process LVGL to clear the screen
    for (int i = 0; i < 10; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // Enable backlight before drawing
    Set_Backlight(70);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Find logo file - prefer raw RGB565 format, then PNG
    
    // Try to find logo file - prefer raw RGB565 format, then PNG
    const char *logo_paths[] = {
        "/Logos/Cobra_Logo_WoB_3D_redtext_360.raw",  // Raw RGB565 format (preferred)
        "/Logos/cobra_logo.raw",
        "/Logos/logo.raw",
        "/Logos/Cobra_Logo_WoB_3D_redtext_360.png",  // PNG format (fallback)
        "/Logos/cobra_logo.png",
        "/Logos/logo.png",
        NULL
    };
    
    const char *logo_path = NULL;
    bool is_raw_format = false;
    for (int i = 0; logo_paths[i] != NULL; i++) {
        if (storage_file_exists(logo_paths[i])) {
            logo_path = logo_paths[i];
            is_raw_format = (strstr(logo_path, ".raw") != NULL);
            break;
        }
    }
    
    if (logo_path == NULL) {
        ESP_LOGW(TAG, "Logo file not found in SPIFFS");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Create image object
    lv_obj_t *img = lv_img_create(scr);
    if (img == NULL) {
        ESP_LOGE(TAG, "Failed to create image object");
        return ESP_FAIL;
    }
    
    // Set image properties before setting source
    lv_obj_set_style_opa(img, LV_OPA_COVER, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    // Ensure image object doesn't clip content
    lv_obj_set_style_clip_corner(img, false, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    
    // Declare lvgl_path outside if/else for use in PNG info retrieval
    char lvgl_path[256] = {0};
    
    if (is_raw_format) {
        // Load raw RGB565 file directly
        char file_path[256];
        snprintf(file_path, sizeof(file_path), "/storage%s", logo_path);
        FILE *raw_file = fopen(file_path, "rb");
        if (raw_file == NULL) {
            ESP_LOGE(TAG, "Cannot open raw file: %s", file_path);
            lv_obj_del(img);
            return ESP_FAIL;
        }
        
        // Get file size
        fseek(raw_file, 0, SEEK_END);
        long file_size = ftell(raw_file);
        fseek(raw_file, 0, SEEK_SET);
        
        // Calculate dimensions from file size (assuming RGB565 = 2 bytes per pixel)
        int expected_width = 360;
        int expected_height = file_size / (expected_width * 2);
        if (expected_height <= 0 || expected_height * expected_width * 2 != file_size) {
            ESP_LOGE(TAG, "Invalid raw file size: %ld bytes", file_size);
            fclose(raw_file);
            lv_obj_del(img);
            return ESP_FAIL;
        }
        
        // Allocate memory for raw RGB565 data
        uint8_t *raw_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (raw_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for raw data");
            fclose(raw_file);
            lv_obj_del(img);
            return ESP_ERR_NO_MEM;
        }
        
        // Read file into memory
        size_t bytes_read = fread(raw_data, 1, file_size, raw_file);
        fclose(raw_file);
        
        if (bytes_read != file_size) {
            ESP_LOGE(TAG, "Failed to read complete file: read %zu of %ld bytes", bytes_read, file_size);
            free(raw_data);
            lv_obj_del(img);
            return ESP_FAIL;
        }
        
        // Create LVGL image descriptor for raw RGB565 (same as test image)
        static lv_img_dsc_t raw_img_dsc;
        raw_img_dsc.header.always_zero = 0;
        raw_img_dsc.header.w = expected_width;
        raw_img_dsc.header.h = expected_height;
        raw_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        raw_img_dsc.data_size = file_size;
        raw_img_dsc.data = raw_data;
        
        lv_img_set_src(img, &raw_img_dsc);
    } else {
        // Use file system path with LVGL file system driver (drive 'A' maps to /storage)
        snprintf(lvgl_path, sizeof(lvgl_path), "A:%s", logo_path);
        lv_img_set_src(img, lvgl_path);
        
        // Handle PNG alpha channel if present
        lv_img_header_t header;
        if (lv_img_decoder_get_info(lvgl_path, &header) == LV_RES_OK) {
            if (header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA) {
                lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_img_opa(img, LV_OPA_COVER, 0);
            }
        }
    }
    
    // Wait for image to load
    for (int i = 0; i < 20; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // Center and display the image
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_invalidate(img);
    lv_obj_invalidate(scr);
    
    // Process LVGL to render the image
    for (int i = 0; i < 50; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    lv_refr_now(lv_disp_get_default());
    
    // Verify image was loaded
    if (lv_obj_get_width(img) == 0 || lv_obj_get_height(img) == 0) {
        ESP_LOGW(TAG, "Image may not have loaded correctly");
        return ESP_FAIL;
    }
    
    // Backlight was already enabled before drawing, so no need to enable it again
    return ESP_OK;
}

void boot_logo_enable_backlight(uint8_t brightness)
{
    Set_Backlight(brightness);
    vTaskDelay(pdMS_TO_TICKS(50));
}

