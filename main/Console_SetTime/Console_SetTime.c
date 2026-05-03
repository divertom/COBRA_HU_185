#include "Console_SetTime.h"

#include "PCF85063.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SETTIME_PREFIX "settime:"
#define SETTIME_PREFIX_LEN (sizeof(SETTIME_PREFIX) - 1U)
#define SETTIME_DIGITS 12U
#define LINE_BUF_MAX 128U

static bool is_digits12(const char *s)
{
    for (size_t i = 0; i < SETTIME_DIGITS; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

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

static int parse_uint(const char *s, size_t n, unsigned *out)
{
    unsigned v = 0U;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return -1;
        }
        v = v * 10U + (unsigned)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static void try_handle_settime(const char *line)
{
    if (strncmp(line, SETTIME_PREFIX, SETTIME_PREFIX_LEN) != 0) {
        return;
    }

    const char *payload = line + SETTIME_PREFIX_LEN;
    size_t rest = strlen(payload);
    while (rest > 0U && (payload[rest - 1U] == '\r' || payload[rest - 1U] == '\n')) {
        rest--;
    }
    if (rest != SETTIME_DIGITS || !is_digits12(payload)) {
        printf("settime: need exactly 12 digits after colon (HHMMYYYYMMDD), got %u chars\r\n",
               (unsigned)rest);
        return;
    }

    unsigned hh = 0, mm = 0, yyyy = 0, mo = 0, dd = 0;
    if (parse_uint(payload, 2U, &hh) != 0 || parse_uint(payload + 2U, 2U, &mm) != 0 ||
        parse_uint(payload + 4U, 4U, &yyyy) != 0 || parse_uint(payload + 8U, 2U, &mo) != 0 ||
        parse_uint(payload + 10U, 2U, &dd) != 0) {
        printf("settime: parse error\r\n");
        return;
    }

    if (hh > 23U || mm > 59U || mo < 1U || mo > 12U) {
        printf("settime: invalid time or month\r\n");
        return;
    }

    if (yyyy < (unsigned)YEAR_OFFSET || yyyy > (unsigned)YEAR_OFFSET + 99U) {
        printf("settime: year must be %d..%d for PCF85063\r\n", YEAR_OFFSET, YEAR_OFFSET + 99);
        return;
    }

    const unsigned dim = days_in_month(yyyy, mo);
    if (dim == 0U || dd < 1U || dd > dim) {
        printf("settime: invalid day for calendar date\r\n");
        return;
    }

    datetime_t t = {0};
    t.year   = (uint16_t)yyyy;
    t.month  = (uint8_t)mo;
    t.day    = (uint8_t)dd;
    t.hour   = (uint8_t)hh;
    t.minute = (uint8_t)mm;
    t.second = 0U;
    t.dotw   = PCF85063_Weekday_Sunday0(t.year, t.month, t.day);

    PCF85063_Set_All(t);
    /* Do not call PCF85063_Read_Time here: Set_All holds the same non-recursive mutex. */
    datetime = t;

    printf("settime: OK %04u-%02u-%02u %02u:%02u dotw=%u\r\n",
           (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
           (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.dotw);
}

static void console_settime_task(void *arg)
{
    (void)arg;
    char buf[LINE_BUF_MAX];
    size_t len = 0U;

    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len > 0U) {
                buf[len] = '\0';
                try_handle_settime(buf);
                len = 0U;
            }
            continue;
        }
        if (len + 1U < LINE_BUF_MAX) {
            buf[len++] = (char)c;
        } else {
            len = 0U;
        }
    }
}

void console_settime_task_start(void)
{
    (void)xTaskCreatePinnedToCore(console_settime_task, "console_settime", 4096, NULL, 1, NULL, 0);
}
