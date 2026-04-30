#include "UI_Navigation.h"

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "Storage_Manager.h"
#include "Page_BootLogo.h"
#include "Page_Clock.h"
#include "Page_Speed.h"
#include "Page_Acceleration.h"
#include "Page_Weather.h"
#include "esp_log.h"

#define NAV_STATE_FILE_PATH "/ui_nav_state.bin"

typedef esp_err_t (*ux_page_render_fn_t)(uint8_t subpage_index);
typedef uint8_t (*ux_page_subpage_count_fn_t)(void);

typedef struct {
    ux_page_id_t page_id;
    const char *name;
    ux_page_render_fn_t render;
    ux_page_subpage_count_fn_t subpage_count;
} ux_page_descriptor_t;

typedef struct {
    uint32_t magic;
    uint8_t page_index;
    uint8_t subpage_index;
    uint8_t reserved[2];
} ux_nav_persisted_state_t;

static const char *TAG = "UI_NAV";
static const uint32_t NAV_STATE_MAGIC = 0x554E4156; /* UNAV */

/* Human-editable page order: adjust this table to reorder or insert pages. */
static const ux_page_descriptor_t s_pages[] = {
    { UX_PAGE_BOOT_LOGO, "Boot Logo", page_boot_logo_render, page_boot_logo_get_subpage_count },
    { UX_PAGE_CLOCK, "Clock", page_clock_render, page_clock_get_subpage_count },
    { UX_PAGE_SPEED, "Speed", page_speed_render, page_speed_get_subpage_count },
    { UX_PAGE_ACCELERATION, "Acceleration", page_acceleration_render, page_acceleration_get_subpage_count },
    { UX_PAGE_WEATHER, "Weather", page_weather_render, page_weather_get_subpage_count },
};

static QueueHandle_t s_event_queue;
static uint8_t s_current_page_index;
static uint8_t s_current_subpage_index;
static bool s_initialized;

static const char *nav_event_to_string(bt_remote_event_t event)
{
    switch (event) {
        case BT_REMOTE_EVENT_VOL_UP:
            return "VOL_UP";
        case BT_REMOTE_EVENT_VOL_DOWN:
            return "VOL_DOWN";
        case BT_REMOTE_EVENT_NEXT_TRACK:
            return "FORWARD";
        case BT_REMOTE_EVENT_PREV_TRACK:
            return "BACK";
        case BT_REMOTE_EVENT_PLAY_PAUSE:
            return "PLAY_PAUSE";
        default:
            return "UNKNOWN";
    }
}

static void nav_log_state(const char *reason)
{
    uint8_t total_subpages = s_pages[s_current_page_index].subpage_count();
    if (total_subpages == 0U) {
        total_subpages = 1U;
    }

    ESP_LOGI(TAG,
             "%s -> page=%s (%u/%u), subpage=%u/%u",
             reason,
             s_pages[s_current_page_index].name,
             (unsigned)(s_current_page_index + 1U),
             (unsigned)UX_PAGE_COUNT,
             (unsigned)(s_current_subpage_index + 1U),
             (unsigned)total_subpages);
}

static esp_err_t nav_save_state(void)
{
    ux_nav_persisted_state_t state = {
        .magic = NAV_STATE_MAGIC,
        .page_index = s_current_page_index,
        .subpage_index = s_current_subpage_index,
        .reserved = {0, 0}
    };

    return storage_write_file(NAV_STATE_FILE_PATH, &state, sizeof(state));
}

static void nav_restore_or_default(void)
{
    ux_nav_persisted_state_t state = {0};
    size_t bytes_read = 0;

    s_current_page_index = 0;
    s_current_subpage_index = 0;

    if (!storage_file_exists(NAV_STATE_FILE_PATH)) {
        return;
    }

    if (storage_read_file(NAV_STATE_FILE_PATH, &state, sizeof(state), &bytes_read) != ESP_OK || bytes_read != sizeof(state)) {
        ESP_LOGW(TAG, "Invalid nav state file, using defaults");
        return;
    }

    if (state.magic != NAV_STATE_MAGIC || state.page_index >= (uint8_t)UX_PAGE_COUNT) {
        ESP_LOGW(TAG, "Nav state content invalid, using defaults");
        return;
    }

    s_current_page_index = state.page_index;
    s_current_subpage_index = state.subpage_index;

    uint8_t max_subpages = s_pages[s_current_page_index].subpage_count();
    if (max_subpages == 0) {
        max_subpages = 1;
    }
    if (s_current_subpage_index >= max_subpages) {
        s_current_subpage_index = 0;
    }
}

static esp_err_t nav_render_current_page(void)
{
    esp_err_t ret = s_pages[s_current_page_index].render(s_current_subpage_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Render failed for page %s", s_pages[s_current_page_index].name);
        return ret;
    }
    nav_log_state("Render");
    return ESP_OK;
}

static void nav_next_page(void)
{
    s_current_page_index = (uint8_t)((s_current_page_index + 1U) % (uint8_t)UX_PAGE_COUNT);
    s_current_subpage_index = 0;
    (void)nav_render_current_page();
    (void)nav_save_state();
}

static void nav_prev_page(void)
{
    if (s_current_page_index == 0U) {
        s_current_page_index = (uint8_t)UX_PAGE_COUNT - 1U;
    } else {
        s_current_page_index--;
    }
    s_current_subpage_index = 0;
    (void)nav_render_current_page();
    (void)nav_save_state();
}

static void nav_subpage_up(void)
{
    if (s_current_subpage_index == 0U) {
        nav_log_state("Subpage up (top)");
        return;
    }
    s_current_subpage_index--;
    (void)nav_render_current_page();
    (void)nav_save_state();
}

static void nav_subpage_down(void)
{
    uint8_t max_subpages = s_pages[s_current_page_index].subpage_count();
    if (max_subpages == 0U || s_current_subpage_index + 1U >= max_subpages) {
        nav_log_state("Subpage down (bottom)");
        return;
    }
    s_current_subpage_index++;
    (void)nav_render_current_page();
    (void)nav_save_state();
}

esp_err_t ux_navigation_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreate(16, sizeof(bt_remote_event_t));
    if (s_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nav_restore_or_default();
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ux_navigation_show_boot_logo(void)
{
    s_current_page_index = (uint8_t)UX_PAGE_BOOT_LOGO;
    s_current_subpage_index = 0;
    return nav_render_current_page();
}

esp_err_t ux_navigation_show_restored_page(void)
{
    nav_restore_or_default();
    esp_err_t ret = nav_render_current_page();
    if (ret == ESP_OK) {
        (void)nav_save_state();
    }
    return ret;
}

void ux_navigation_queue_remote_event(bt_remote_event_t event)
{
    if (!s_initialized || s_event_queue == NULL) {
        return;
    }

    (void)xQueueSend(s_event_queue, &event, 0);
}

void ux_navigation_process_events(void)
{
    if (!s_initialized || s_event_queue == NULL) {
        return;
    }

    bt_remote_event_t event = BT_REMOTE_EVENT_UNKNOWN;
    while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Remote event: %s", nav_event_to_string(event));
        switch (event) {
            case BT_REMOTE_EVENT_NEXT_TRACK:
                nav_next_page();
                break;
            case BT_REMOTE_EVENT_PREV_TRACK:
                nav_prev_page();
                break;
            case BT_REMOTE_EVENT_VOL_UP:
                nav_subpage_up();
                break;
            case BT_REMOTE_EVENT_VOL_DOWN:
                nav_subpage_down();
                break;
            default:
                break;
        }
    }
}
