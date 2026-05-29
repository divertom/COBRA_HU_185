#include "UI_Navigation.h"

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "Storage_Manager.h"
#include "Page_BootLogo.h"
#include "Page_Clock.h"
#include "Page_Speed.h"
#include "Page_Acceleration.h"
#include "Page_Weather.h"
#include "Page_Status.h"
#include "esp_log.h"
#include "lvgl.h"
#include "Boot_Logo_Api.h"
#include "Wireless.h"

#define NAV_STATE_FILE_PATH "/ui_nav_state.bin"

typedef esp_err_t (*ux_page_render_fn_t)(uint8_t subpage_index, lv_obj_t *root);
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
} ux_nav_persisted_legacy_t;

typedef struct {
    uint32_t magic;
    uint8_t page_index;
    uint8_t last_subpage[UX_PAGE_COUNT];
} ux_nav_persisted_state_v2_t;

static const char *TAG = "UI_NAV";
static const uint32_t NAV_STATE_MAGIC_LEGACY = 0x554E4156; /* UNAV */
static const uint32_t NAV_STATE_MAGIC_V2 = 0x554E5632; /* UNV2 */

/* Human-editable page order: adjust this table to reorder or insert pages. */
static const ux_page_descriptor_t s_pages[] = {
    { UX_PAGE_BOOT_LOGO, "Boot Logo", page_boot_logo_render, page_boot_logo_get_subpage_count },
    { UX_PAGE_CLOCK, "Clock", page_clock_render, page_clock_get_subpage_count },
    { UX_PAGE_SPEED, "Speed", page_speed_render, page_speed_get_subpage_count },
    { UX_PAGE_ACCELERATION, "Acceleration", page_acceleration_render, page_acceleration_get_subpage_count },
    { UX_PAGE_WEATHER, "Weather", page_weather_render, page_weather_get_subpage_count },
    { UX_PAGE_STATUS, "Status", page_status_render, page_status_get_subpage_count },
};

static QueueHandle_t s_event_queue;
static uint8_t s_current_page_index;
static uint8_t s_current_subpage_index;
static uint8_t s_last_subpage[UX_PAGE_COUNT];
static bool s_initialized;

/** Index into s_pages for the page last shown on the active screen (255 = none yet). */
static uint8_t s_last_rendered_page_index = 0xFFU;

static bool s_boot_logo_use_startup_timing;
/** False until boot splash is replaced (main loop may process HID queue). */
static bool s_ui_ready;
static lv_timer_t *s_post_boot_timer;
static bool s_wireless_started;

/** Boot logo index: skipped when restoring persisted UI state (cold boot uses show_boot_logo). */
static bool nav_page_is_boot_logo(uint8_t page_index)
{
    return s_pages[page_index].page_id == UX_PAGE_BOOT_LOGO;
}

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

static uint8_t nav_max_subpages_for(uint8_t page_index)
{
    uint8_t n = s_pages[page_index].subpage_count();
    return (n == 0U) ? 1U : n;
}

static uint8_t nav_clamp_subpage(uint8_t page_index, uint8_t candidate)
{
    uint8_t max_sp = nav_max_subpages_for(page_index);
    if (candidate >= max_sp) {
        return 0U;
    }
    return candidate;
}

static void nav_clamp_all_last_subpages(void)
{
    for (uint8_t i = 0; i < (uint8_t)UX_PAGE_COUNT; i++) {
        s_last_subpage[i] = nav_clamp_subpage(i, s_last_subpage[i]);
    }
}

static void nav_reset_defaults(void)
{
    (void)memset(s_last_subpage, 0, sizeof(s_last_subpage));
    /* Boot logo is startup-only; default persisted page is Clock. */
    s_current_page_index = (uint8_t)UX_PAGE_CLOCK;
    s_current_subpage_index = 0;
}

static esp_err_t nav_save_state(void)
{
    ux_nav_persisted_state_v2_t state = {
        .magic = NAV_STATE_MAGIC_V2,
        .page_index = s_current_page_index,
    };

    for (uint8_t i = 0; i < (uint8_t)UX_PAGE_COUNT; i++) {
        state.last_subpage[i] = s_last_subpage[i];
    }

    return storage_write_file(NAV_STATE_FILE_PATH, &state, sizeof(state));
}

static void nav_restore_or_default(void)
{
    ux_nav_persisted_state_v2_t v2 = {0};
    ux_nav_persisted_legacy_t leg = {0};
    size_t bytes_read = 0;

    nav_reset_defaults();

    if (!storage_file_exists(NAV_STATE_FILE_PATH)) {
        return;
    }

    if (storage_read_file(NAV_STATE_FILE_PATH, &v2, sizeof(v2), &bytes_read) == ESP_OK &&
        bytes_read == sizeof(v2) && v2.magic == NAV_STATE_MAGIC_V2 &&
        v2.page_index < (uint8_t)UX_PAGE_COUNT) {
        s_current_page_index = v2.page_index;
        (void)memcpy(s_last_subpage, v2.last_subpage, sizeof(s_last_subpage));
        nav_clamp_all_last_subpages();
        if (nav_page_is_boot_logo(s_current_page_index)) {
            s_current_page_index = (uint8_t)UX_PAGE_CLOCK;
        }
        s_current_subpage_index = s_last_subpage[s_current_page_index];
        return;
    }

    if (storage_read_file(NAV_STATE_FILE_PATH, &leg, sizeof(leg), &bytes_read) == ESP_OK &&
        bytes_read == sizeof(leg) && leg.magic == NAV_STATE_MAGIC_LEGACY &&
        leg.page_index < (uint8_t)UX_PAGE_COUNT) {
        s_current_page_index = leg.page_index;
        if (nav_page_is_boot_logo(s_current_page_index)) {
            s_current_page_index = (uint8_t)UX_PAGE_CLOCK;
        }
        s_current_subpage_index = nav_clamp_subpage(s_current_page_index, leg.subpage_index);
        s_last_subpage[s_current_page_index] = s_current_subpage_index;
        return;
    }

    ESP_LOGW(TAG, "Invalid nav state file, using defaults");
}

static esp_err_t nav_render_current_page(void)
{
    if (s_last_rendered_page_index != 0xFFU &&
        s_last_rendered_page_index == (uint8_t)UX_PAGE_STATUS) {
        page_status_prepare_leave();
    }

    /* Cold boot: draw logo on the active screen like pre-navigation builds.
     * lv_refr_now() only composites the active screen — off-screen builds were
     * refreshing the wrong (white) framebuffer and caused a flash after lv_scr_load(). */
    const bool cold_boot_logo =
        s_boot_logo_use_startup_timing &&
        (s_current_page_index == (uint8_t)UX_PAGE_BOOT_LOGO);

    if (cold_boot_logo) {
        lv_obj_t *scr = lv_scr_act();
        esp_err_t ret =
            s_pages[s_current_page_index].render(s_current_subpage_index, scr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Render failed for page %s", s_pages[s_current_page_index].name);
            return ret;
        }
        lv_refr_now(lv_disp_get_default());
        boot_logo_enable_backlight(70);
        s_last_rendered_page_index = s_current_page_index;
        nav_log_state("Render");
        return ESP_OK;
    }

    const bool leaving_boot_splash =
        (s_last_rendered_page_index == (uint8_t)UX_PAGE_BOOT_LOGO);

    lv_obj_t *old_scr = lv_scr_act();
    if (leaving_boot_splash) {
        lv_img_cache_invalidate_src("A:/boot/cobra_boot.bin");
        lv_img_cache_invalidate_src(NULL);
    } else {
        lv_img_cache_invalidate_src(NULL);
    }

    lv_obj_t *new_scr = lv_obj_create(NULL);
    if (new_scr == NULL) {
        ESP_LOGE(TAG, "Failed to allocate screen for page %s", s_pages[s_current_page_index].name);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(new_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(new_scr, LV_OPA_COVER, LV_PART_MAIN);

    ESP_LOGI(TAG, "Rendering %s on new screen", s_pages[s_current_page_index].name);
    esp_err_t ret =
        s_pages[s_current_page_index].render(s_current_subpage_index, new_scr);
    if (ret != ESP_OK) {
        lv_obj_del(new_scr);
        ESP_LOGE(TAG, "Render failed for page %s", s_pages[s_current_page_index].name);
        return ret;
    }

    lv_scr_load(new_scr);
    /* Sync delete of a file-backed splash lv_img can block; defer teardown. */
    if (leaving_boot_splash) {
        lv_obj_del_async(old_scr);
    } else {
        lv_obj_del(old_scr);
    }
    lv_refr_now(lv_disp_get_default());

    s_last_rendered_page_index = s_current_page_index;
    nav_log_state("Render");
    return ESP_OK;
}

static void nav_next_page(void)
{
    s_last_subpage[s_current_page_index] = s_current_subpage_index;

    s_current_page_index = (uint8_t)((s_current_page_index + 1U) % (uint8_t)UX_PAGE_COUNT);

    s_current_subpage_index = nav_clamp_subpage(s_current_page_index,
                                                s_last_subpage[s_current_page_index]);
    s_last_subpage[s_current_page_index] = s_current_subpage_index;

    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after next");
    }
    (void)nav_save_state();
}

static void nav_prev_page(void)
{
    s_last_subpage[s_current_page_index] = s_current_subpage_index;

    if (s_current_page_index == 0U) {
        s_current_page_index = (uint8_t)UX_PAGE_COUNT - 1U;
    } else {
        s_current_page_index--;
    }

    s_current_subpage_index = nav_clamp_subpage(s_current_page_index,
                                                s_last_subpage[s_current_page_index]);
    s_last_subpage[s_current_page_index] = s_current_subpage_index;

    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after prev");
    }
    (void)nav_save_state();
}

static void nav_subpage_up(void)
{
    if (s_current_subpage_index == 0U) {
        nav_log_state("Subpage up (top)");
        return;
    }
    s_current_subpage_index--;
    s_last_subpage[s_current_page_index] = s_current_subpage_index;
    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after subpage up");
    }
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
    s_last_subpage[s_current_page_index] = s_current_subpage_index;
    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after subpage down");
    }
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
    s_ui_ready = false;
    s_initialized = true;
    return ESP_OK;
}

bool ux_navigation_boot_logo_startup_timing(void)
{
    return s_boot_logo_use_startup_timing;
}

esp_err_t ux_navigation_show_boot_logo(void)
{
    s_boot_logo_use_startup_timing = true;
    s_current_page_index = (uint8_t)UX_PAGE_BOOT_LOGO;
    s_current_subpage_index = 0;
    esp_err_t ret = nav_render_current_page();
    s_boot_logo_use_startup_timing = false;
    return ret;
}

static void post_boot_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ESP_LOGI(TAG, "Post-boot handoff");
    boot_logo_release_ram_cache();
    if (ux_navigation_show_restored_page() != ESP_OK) {
        ESP_LOGE(TAG, "Post-boot handoff failed");
    }
    /* Clock UI first, then BT/WiFi (avoids heap + SPIFFS contention during first paint). */
    if (!s_wireless_started) {
        Wireless_Init();
        s_wireless_started = true;
    }
    if (s_post_boot_timer != NULL) {
        lv_timer_del(s_post_boot_timer);
        s_post_boot_timer = NULL;
    }
}

void ux_navigation_schedule_restored_page(uint32_t delay_ms)
{
    if (s_post_boot_timer != NULL) {
        lv_timer_del(s_post_boot_timer);
        s_post_boot_timer = NULL;
    }
    s_post_boot_timer = lv_timer_create(post_boot_timer_cb, delay_ms, NULL);
    if (s_post_boot_timer != NULL) {
        lv_timer_set_repeat_count(s_post_boot_timer, 1);
    } else {
        ESP_LOGE(TAG, "Failed to create post-boot timer");
    }
}

esp_err_t ux_navigation_show_restored_page(void)
{
    nav_restore_or_default();
    if (nav_page_is_boot_logo(s_current_page_index)) {
        s_current_page_index = (uint8_t)UX_PAGE_CLOCK;
        s_current_subpage_index = s_last_subpage[s_current_page_index];
    }

    esp_err_t ret = nav_render_current_page();
    if (ret == ESP_OK) {
        (void)nav_save_state();
    } else {
        ESP_LOGE(TAG, "Restored page render failed");
    }
    /* Allow HID navigation even if first paint failed (unblocks main loop). */
    s_ui_ready = true;
    return ret;
}

ux_page_id_t ux_navigation_get_active_page(void)
{
    if (!s_initialized || s_current_page_index >= (uint8_t)UX_PAGE_COUNT) {
        return UX_PAGE_BOOT_LOGO;
    }
    return s_pages[s_current_page_index].page_id;
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
    if (!s_initialized || !s_ui_ready || s_event_queue == NULL) {
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
