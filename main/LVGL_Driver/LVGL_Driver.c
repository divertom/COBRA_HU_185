#include "LVGL_Driver.h"

#include "Boot_Logo_Api.h"

#include "esp_lcd_panel_io.h"

#include "esp_heap_caps.h"



static const char *TAG_LVGL = "LVGL";



lv_disp_draw_buf_t disp_buf;

lv_disp_drv_t disp_drv;

lv_disp_t *disp;



static lv_color_t *lvgl_draw_buf_alloc(void)

{

    const size_t bytes = LVGL_BUF_LEN * sizeof(lv_color_t);

    lv_color_t *p =

        (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (p == NULL) {

        p = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);

    }

    return p;

}



static bool lcd_on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,

                                    esp_lcd_panel_io_event_data_t *edata,

                                    void *user_ctx)

{

    (void)panel_io;

    (void)edata;

    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;

    if (drv != NULL) {

        lv_disp_flush_ready(drv);

    }

    return false;

}



void example_increase_lvgl_tick(void *arg)

{

    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);

}



void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)

{

    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;

    const int offsetx1 = area->x1;

    const int offsetx2 = area->x2;

    const int offsety1 = area->y1;

    const int offsety2 = area->y2;



    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle,

                                              offsetx1,

                                              offsety1,

                                              offsetx2 + 1,

                                              offsety2 + 1,

                                              color_map);

    if (err != ESP_OK) {

        ESP_LOGE(TAG_LVGL, "draw_bitmap failed: %s", esp_err_to_name(err));

        lv_disp_flush_ready(drv);

    }

    /* Else: lv_disp_flush_ready() from lcd_on_color_trans_done when SPI DMA completes. */

}



void example_lvgl_port_update_callback(lv_disp_drv_t *drv)

{

    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;



    switch (drv->rotated) {

    case LV_DISP_ROT_NONE:

        esp_lcd_panel_swap_xy(panel_handle, false);

        esp_lcd_panel_mirror(panel_handle, true, false);

        break;

    case LV_DISP_ROT_90:

        esp_lcd_panel_swap_xy(panel_handle, true);

        esp_lcd_panel_mirror(panel_handle, true, true);

        break;

    case LV_DISP_ROT_180:

        esp_lcd_panel_swap_xy(panel_handle, false);

        esp_lcd_panel_mirror(panel_handle, false, true);

        break;

    case LV_DISP_ROT_270:

        esp_lcd_panel_swap_xy(panel_handle, true);

        esp_lcd_panel_mirror(panel_handle, false, false);

        break;

    }

}



void LVGL_Init(void)

{

    ESP_LOGI(TAG_LVGL, "Initialize LVGL library");

    lv_init();

    lvgl_spiffs_assets_fs_register();



    lv_color_t *buf1 = lvgl_draw_buf_alloc();

    lv_color_t *buf2 = lvgl_draw_buf_alloc();

    assert(buf1 && buf2);

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LVGL_BUF_LEN);



    ESP_LOGI(TAG_LVGL, "Register display driver to LVGL");

    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res          = EXAMPLE_LCD_WIDTH;

    disp_drv.ver_res          = EXAMPLE_LCD_HEIGHT;

    disp_drv.flush_cb         = example_lvgl_flush_cb;

    disp_drv.drv_update_cb    = example_lvgl_port_update_callback;

    disp_drv.draw_buf         = &disp_buf;

    disp_drv.user_data        = panel_handle;

    disp                      = lv_disp_drv_register(&disp_drv);

    lv_disp_set_bg_color(disp, lv_color_black());



    if (panel_io_handle != NULL) {

        esp_lcd_panel_io_callbacks_t cbs = {

            .on_color_trans_done = lcd_on_color_trans_done,

        };

        esp_err_t err =

            esp_lcd_panel_io_register_event_callbacks(panel_io_handle, &cbs, &disp_drv);

        if (err != ESP_OK) {

            ESP_LOGE(TAG_LVGL, "panel_io_register_event_callbacks failed: %s", esp_err_to_name(err));

        }

    } else {

        ESP_LOGE(TAG_LVGL, "panel_io_handle is NULL — SPI flush sync disabled");

    }



    ESP_LOGI(TAG_LVGL, "Install LVGL tick timer");

    const esp_timer_create_args_t lvgl_tick_timer_args = {

        .callback = &example_increase_lvgl_tick,

        .name     = "lvgl_tick",

    };



    esp_timer_handle_t lvgl_tick_timer = NULL;

    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));

    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

}


