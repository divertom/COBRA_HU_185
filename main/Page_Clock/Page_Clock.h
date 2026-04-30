#ifndef PAGE_CLOCK_H
#define PAGE_CLOCK_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t page_clock_render(uint8_t subpage_index);
uint8_t page_clock_get_subpage_count(void);

#endif
