#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * Validate and write date/time to PCF85063; updates global `datetime`.
 */
esp_err_t datetime_set(uint16_t year, uint8_t month, uint8_t day,
                       uint8_t hour, uint8_t minute, uint8_t second);
