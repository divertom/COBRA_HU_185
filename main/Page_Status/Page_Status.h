#ifndef PAGE_STATUS_H
#define PAGE_STATUS_H

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

void page_status_prepare_leave(void);
esp_err_t page_status_render(uint8_t subpage_index, lv_obj_t *root);
uint8_t page_status_get_subpage_count(void);

#endif
