#ifndef BOOT_LOGO_API_H
#define BOOT_LOGO_API_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t boot_logo_display(void);
void boot_logo_enable_backlight(uint8_t brightness);

#endif
