#ifndef UI_NAVIGATION_H
#define UI_NAVIGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "Wireless.h"

typedef enum {
    UX_PAGE_BOOT_LOGO = 0,
    UX_PAGE_CLOCK,
    UX_PAGE_SPEED,
    UX_PAGE_ACCELERATION,
    UX_PAGE_WEATHER,
    UX_PAGE_STATUS,
    UX_PAGE_COUNT
} ux_page_id_t;

/** Carousel pages (clock … status + boot logo). */
#define UX_NAVIGABLE_COUNT 6

esp_err_t ux_navigation_init(void);

/** True only while rendering the cold-boot splash via ux_navigation_show_boot_logo(). */
bool ux_navigation_boot_logo_startup_timing(void);

/** Current active page (persisted/restored once init has run). */
ux_page_id_t ux_navigation_get_active_page(void);
esp_err_t ux_navigation_show_boot_logo(void);
/** One-shot LVGL timer: leave boot splash and show persisted page (non-blocking). */
void ux_navigation_schedule_restored_page(uint32_t delay_ms);
esp_err_t ux_navigation_show_restored_page(void);
void ux_navigation_queue_remote_event(bt_remote_event_t event);
void ux_navigation_process_events(void);

/** Copy current carousel order (UX_NAVIGABLE_COUNT entries, each a ux_page_id_t). */
esp_err_t ux_navigation_get_page_order(uint8_t *order, size_t count);

/** Human-readable page name for a page id. */
const char *ux_navigation_page_name(ux_page_id_t page_id);

/** Validate and queue a new carousel order (applied on main/LVGL thread). */
esp_err_t ux_navigation_request_set_page_order(const uint8_t *order, size_t count);

#endif
