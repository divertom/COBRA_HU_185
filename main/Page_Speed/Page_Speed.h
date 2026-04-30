#ifndef PAGE_SPEED_H
#define PAGE_SPEED_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t page_speed_render(uint8_t subpage_index);
uint8_t page_speed_get_subpage_count(void);

#endif
