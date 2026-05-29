#include "Boot_Logo.h"

#include "ST77916.h"

#include "Storage_Manager.h"

#include "esp_heap_caps.h"

#include "esp_log.h"

#include "lvgl.h"

#include "freertos/FreeRTOS.h"

#include "freertos/task.h"



#include <stdbool.h>

#include <stdint.h>

#include <stdio.h>

#include <string.h>



static const char *TAG = "Boot_Logo";



static const char k_boot_logo_spiffs_path[] = "/boot/cobra_boot.bin";

static const char k_boot_logo_lvgl_path[]  = "A:/boot/cobra_boot.bin";



static uint8_t *s_boot_logo_ram;

static lv_img_dsc_t s_boot_logo_dsc;

static bool s_boot_logo_ram_ready;



static void *fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)

{

    (void)drv;

    const char *path_clean = path;

    while (*path_clean == '/') {

        path_clean++;

    }



    char full_path[256];

    (void)snprintf(full_path, sizeof(full_path), "/storage/%s", path_clean);



    const char *mode_str = "rb";

    if (mode == LV_FS_MODE_WR) {

        mode_str = "wb";

    } else if (mode == (LV_FS_MODE_WR | LV_FS_MODE_RD)) {

        mode_str = "r+b";

    }



    FILE *f = fopen(full_path, mode_str);

    if (f == NULL) {

        ESP_LOGE(TAG, "Failed to open %s", full_path);

    }

    return (void *)f;

}



static lv_fs_res_t fs_close(lv_fs_drv_t *drv, void *file_p)

{

    (void)drv;

    fclose((FILE *)file_p);

    return LV_FS_RES_OK;

}



static lv_fs_res_t fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)

{

    (void)drv;

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

    (void)drv;

    FILE *f = (FILE *)file_p;

    int w = SEEK_SET;

    if (whence == LV_FS_SEEK_CUR) {

        w = SEEK_CUR;

    } else if (whence == LV_FS_SEEK_END) {

        w = SEEK_END;

    }

    fseek(f, pos, w);

    return LV_FS_RES_OK;

}



static lv_fs_res_t fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)

{

    (void)drv;

    FILE *f = (FILE *)file_p;

    *pos_p = (uint32_t)ftell(f);

    return LV_FS_RES_OK;

}



void lvgl_spiffs_assets_fs_register(void)

{

    static bool s_registered;

    if (s_registered) {

        return;

    }



    static lv_fs_drv_t fs_drv;

    lv_fs_drv_init(&fs_drv);

    fs_drv.letter     = 'A';

    fs_drv.cache_size = 0;

    fs_drv.open_cb    = fs_open;

    fs_drv.close_cb   = fs_close;

    fs_drv.read_cb    = fs_read;

    fs_drv.seek_cb    = fs_seek;

    fs_drv.tell_cb    = fs_tell;

    lv_fs_drv_register(&fs_drv);



    s_registered = true;

}



bool lvgl_bin_read_header_from_spiffs(const char *spiffs_path, lv_img_header_t *header)

{

    uint8_t buf[4];

    size_t bytes_read = 0;



    if (spiffs_path == NULL || header == NULL) {

        return false;

    }



    lv_memset_00(header, sizeof(*header));

    if (storage_read_file(spiffs_path, buf, sizeof(buf), &bytes_read) != ESP_OK ||

        bytes_read < sizeof(buf)) {

        return false;

    }



    memcpy(header, buf, sizeof(*header));

    return (header->cf == LV_IMG_CF_TRUE_COLOR || header->cf == LV_IMG_CF_TRUE_COLOR_ALPHA) &&

           header->w > 0 && header->h > 0;

}



esp_err_t lvgl_bin_load_to_dsc_from_spiffs(const char *spiffs_path, lv_img_dsc_t *dsc, uint8_t **ram_out)

{

    size_t file_size = 0;

    size_t bytes_read = 0;



    if (spiffs_path == NULL || dsc == NULL || ram_out == NULL) {

        return ESP_ERR_INVALID_ARG;

    }



    *ram_out = NULL;

    lv_memset_00(dsc, sizeof(*dsc));



    if (storage_get_file_size(spiffs_path, &file_size) != ESP_OK || file_size <= sizeof(lv_img_header_t)) {

        return ESP_ERR_NOT_FOUND;

    }



    /* Large assets must stay in PSRAM — do not fall back to internal heap (breaks BLE/WiFi). */

    uint8_t *ram = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ram == NULL) {

        ESP_LOGE(TAG, "PSRAM alloc failed for %s (%u bytes)", spiffs_path, (unsigned)file_size);

        return ESP_ERR_NO_MEM;

    }



    if (storage_read_file(spiffs_path, ram, file_size, &bytes_read) != ESP_OK ||

        bytes_read != file_size) {

        heap_caps_free(ram);

        return ESP_FAIL;

    }



    memcpy(&dsc->header, ram, sizeof(lv_img_header_t));

    dsc->data      = ram + sizeof(lv_img_header_t);

    dsc->data_size = (uint32_t)(file_size - sizeof(lv_img_header_t));



    if ((dsc->header.cf != LV_IMG_CF_TRUE_COLOR && dsc->header.cf != LV_IMG_CF_TRUE_COLOR_ALPHA) ||

        dsc->header.w <= 0 || dsc->header.h <= 0) {

        heap_caps_free(ram);

        lv_memset_00(dsc, sizeof(*dsc));

        return ESP_ERR_INVALID_VERSION;

    }



    *ram_out = ram;

    return ESP_OK;

}



static esp_err_t boot_logo_ensure_ram(void)

{

    if (s_boot_logo_ram_ready) {

        return ESP_OK;

    }



    esp_err_t err =

        lvgl_bin_load_to_dsc_from_spiffs(k_boot_logo_spiffs_path, &s_boot_logo_dsc, &s_boot_logo_ram);

    if (err == ESP_OK) {

        s_boot_logo_ram_ready = true;

        ESP_LOGI(TAG, "Boot logo cached in PSRAM (%u bytes)", (unsigned)s_boot_logo_dsc.data_size);

    }

    return err;

}



void boot_logo_release_ram_cache(void)

{

    if (s_boot_logo_ram != NULL) {

        heap_caps_free(s_boot_logo_ram);

        s_boot_logo_ram = NULL;

    }

    lv_memset_00(&s_boot_logo_dsc, sizeof(s_boot_logo_dsc));

    s_boot_logo_ram_ready = false;

    ESP_LOGI(TAG, "Boot logo PSRAM cache released");

}



esp_err_t boot_logo_display(lv_obj_t *scr, bool startup_timing)

{

    if (startup_timing) {

        Set_Backlight(0);

    }



    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);



    if (!storage_file_exists(k_boot_logo_spiffs_path)) {

        ESP_LOGW(TAG, "Boot logo missing on SPIFFS: %s", k_boot_logo_spiffs_path);

        return ESP_ERR_NOT_FOUND;

    }



    lv_obj_t *img = lv_img_create(scr);

    if (img == NULL) {

        return ESP_FAIL;

    }



    lv_obj_set_style_opa(img, LV_OPA_COVER, 0);

    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(img, LV_OBJ_FLAG_OVERFLOW_VISIBLE);



    const void *src = NULL;

    lv_img_header_t header;

    lv_memset_00(&header, sizeof(header));



    if (boot_logo_ensure_ram() == ESP_OK) {

        src    = &s_boot_logo_dsc;

        header = s_boot_logo_dsc.header;

    } else if (lv_img_decoder_get_info(k_boot_logo_lvgl_path, &header) == LV_RES_OK) {

        src = k_boot_logo_lvgl_path;

    } else if (lvgl_bin_read_header_from_spiffs(k_boot_logo_spiffs_path, &header)) {

        src = k_boot_logo_lvgl_path;

    } else {

        lv_obj_del(img);

        return ESP_ERR_NOT_FOUND;

    }



    lv_img_set_src(img, src);

    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);



    if (header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA) {

        lv_obj_set_style_bg_color(scr,

                                  startup_timing ? lv_color_black() : lv_color_white(),

                                  LV_PART_MAIN);

        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    }



    ESP_LOGI(TAG, "Boot logo %ux%u cf=%u (%s)",

             (unsigned)header.w, (unsigned)header.h, (unsigned)header.cf,

             (src == &s_boot_logo_dsc) ? "PSRAM" : "file");

    return ESP_OK;

}



void boot_logo_enable_backlight(uint8_t brightness)

{

    Set_Backlight(brightness);

    vTaskDelay(pdMS_TO_TICKS(50));

}


