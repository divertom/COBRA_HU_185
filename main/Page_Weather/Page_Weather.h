#ifndef PAGE_WEATHER_H
#define PAGE_WEATHER_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t page_weather_render(uint8_t subpage_index);
uint8_t page_weather_get_subpage_count(void);

#endif
