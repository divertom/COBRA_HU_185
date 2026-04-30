#ifndef PAGE_BOOT_LOGO_H
#define PAGE_BOOT_LOGO_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t page_boot_logo_render(uint8_t subpage_index);
uint8_t page_boot_logo_get_subpage_count(void);

#endif
