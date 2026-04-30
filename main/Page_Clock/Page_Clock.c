#include "Page_Clock.h"
#include "lvgl.h"
#include <stdio.h>

esp_err_t page_clock_render(uint8_t subpage_index)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);

    char text[48];
    snprintf(text, sizeof(text), "Clock\nSubpage %u", (unsigned)(subpage_index + 1U));
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return ESP_OK;
}

uint8_t page_clock_get_subpage_count(void)
{
    return 3;
}
