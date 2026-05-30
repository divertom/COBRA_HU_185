#include "Console_SetTime.h"

#include "Datetime_Set.h"

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

    esp_err_t err = datetime_set((uint16_t)yyyy, (uint8_t)mo, (uint8_t)dd,
                                 (uint8_t)hh, (uint8_t)mm, 0U);
    if (err != ESP_OK) {
        printf("settime: invalid date/time\r\n");
        return;
    }

    printf("settime: OK %04u-%02u-%02u %02u:%02u\r\n",
           yyyy, mo, dd, hh, mm);
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
