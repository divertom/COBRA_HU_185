#include "Datetime_Set.h"

#include "PCF85063.h"

static bool is_leap_year(unsigned y)
{
    return ((y % 4U == 0U) && (y % 100U != 0U)) || (y % 400U == 0U);
}

static unsigned days_in_month(unsigned y, unsigned m)
{
    static const unsigned md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1U || m > 12U) {
        return 0U;
    }
    if (m == 2U && is_leap_year(y)) {
        return 29U;
    }
    return md[m - 1U];
}

esp_err_t datetime_set(uint16_t year, uint8_t month, uint8_t day,
                       uint8_t hour, uint8_t minute, uint8_t second)
{
    if (hour > 23U || minute > 59U || second > 59U || month < 1U || month > 12U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (year < (uint16_t)YEAR_OFFSET || year > (uint16_t)(YEAR_OFFSET + 99)) {
        return ESP_ERR_INVALID_ARG;
    }

    const unsigned dim = days_in_month(year, month);
    if (dim == 0U || day < 1U || day > dim) {
        return ESP_ERR_INVALID_ARG;
    }

    datetime_t t = {0};
    t.year   = year;
    t.month  = month;
    t.day    = day;
    t.hour   = hour;
    t.minute = minute;
    t.second = second;
    t.dotw   = PCF85063_Weekday_Sunday0(t.year, t.month, t.day);

    PCF85063_Set_All(t);
    datetime = t;
    return ESP_OK;
}
