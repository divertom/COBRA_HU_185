#ifndef UI_NAVIGATION_H
#define UI_NAVIGATION_H

#include <stdint.h>
#include "esp_err.h"
#include "Wireless.h"

typedef enum {
    UX_PAGE_BOOT_LOGO = 0,
    UX_PAGE_CLOCK,
    UX_PAGE_SPEED,
    UX_PAGE_ACCELERATION,
    UX_PAGE_WEATHER,
    UX_PAGE_COUNT
} ux_page_id_t;

esp_err_t ux_navigation_init(void);
esp_err_t ux_navigation_show_boot_logo(void);
esp_err_t ux_navigation_show_restored_page(void);
void ux_navigation_queue_remote_event(bt_remote_event_t event);
void ux_navigation_process_events(void);

#endif
