#ifndef PAGE_STATUS_H
#define PAGE_STATUS_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t page_status_render(uint8_t subpage_index);
uint8_t page_status_get_subpage_count(void);

#endif
