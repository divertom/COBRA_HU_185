#ifndef PAGE_ACCELERATION_H
#define PAGE_ACCELERATION_H

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

esp_err_t page_acceleration_render(uint8_t subpage_index, lv_obj_t *root);
uint8_t page_acceleration_get_subpage_count(void);

#endif
