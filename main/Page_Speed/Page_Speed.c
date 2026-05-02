#include "Page_Speed.h"
#include "lvgl.h"
#include <stdio.h>

esp_err_t page_speed_render(uint8_t subpage_index, lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(root);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);

    char text[48];
    snprintf(text, sizeof(text), "Speed\nSubpage %u", (unsigned)(subpage_index + 1U));
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return ESP_OK;
}

uint8_t page_speed_get_subpage_count(void)
{
    return 3;
}
