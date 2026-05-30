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

typedef struct {
    uint32_t magic;
    uint8_t carousel_slot;
    uint8_t carousel_order[UX_NAVIGABLE_COUNT];
    uint8_t last_subpage[UX_PAGE_COUNT];
} ux_nav_persisted_state_v3_t;

typedef struct {
    uint8_t order[UX_NAVIGABLE_COUNT];
} nav_order_cmd_t;

static const char *TAG = "UI_NAV";
static const uint32_t NAV_STATE_MAGIC_LEGACY = 0x554E4156; /* UNAV */
static const uint32_t NAV_STATE_MAGIC_V2 = 0x554E5632; /* UNV2 */
static const uint32_t NAV_STATE_MAGIC_V3 = 0x554E5633; /* UNV3 */
/** Persisted layout before boot logo joined the carousel (UX_NAVIGABLE_COUNT was 5). */
#define UX_NAVIGABLE_COUNT_V3_LEGACY 5

typedef struct {
    uint32_t magic;
    uint8_t carousel_slot;
    uint8_t carousel_order[UX_NAVIGABLE_COUNT_V3_LEGACY];
    uint8_t last_subpage[UX_PAGE_COUNT];
} ux_nav_persisted_state_v3_legacy_t;

static const uint8_t s_default_carousel_order[UX_NAVIGABLE_COUNT] = {
    UX_PAGE_CLOCK,
    UX_PAGE_SPEED,
    UX_PAGE_ACCELERATION,
    UX_PAGE_WEATHER,
    UX_PAGE_STATUS,
    UX_PAGE_BOOT_LOGO,
};

static const ux_page_descriptor_t s_pages[] = {
    { UX_PAGE_BOOT_LOGO, "Boot Logo", page_boot_logo_render, page_boot_logo_get_subpage_count },
    { UX_PAGE_CLOCK, "Clock", page_clock_render, page_clock_get_subpage_count },
    { UX_PAGE_SPEED, "Speed", page_speed_render, page_speed_get_subpage_count },
    { UX_PAGE_ACCELERATION, "Acceleration", page_acceleration_render, page_acceleration_get_subpage_count },
    { UX_PAGE_WEATHER, "Weather", page_weather_render, page_weather_get_subpage_count },
    { UX_PAGE_STATUS, "Status", page_status_render, page_status_get_subpage_count },
};

static QueueHandle_t s_event_queue;
static QueueHandle_t s_order_cmd_queue;
static uint8_t s_current_page_id;
static uint8_t s_current_subpage_index;
static uint8_t s_carousel_slot;
static uint8_t s_carousel_order[UX_NAVIGABLE_COUNT];
static uint8_t s_last_subpage[UX_PAGE_COUNT];
static bool s_initialized;

static uint8_t s_last_rendered_page_id = 0xFFU;

static bool s_boot_logo_use_startup_timing;
static bool s_ui_ready;
static lv_timer_t *s_post_boot_timer;
static bool s_wireless_started;

static bool nav_page_is_boot_logo(uint8_t page_id)
{
    return page_id == (uint8_t)UX_PAGE_BOOT_LOGO;
}

static bool nav_page_is_navigable(uint8_t page_id)
{
    return nav_page_is_boot_logo(page_id) ||
           (page_id >= (uint8_t)UX_PAGE_CLOCK && page_id <= (uint8_t)UX_PAGE_STATUS);
}

static void nav_set_default_carousel_order(void)
{
    (void)memcpy(s_carousel_order, s_default_carousel_order, sizeof(s_carousel_order));
}

static uint8_t nav_slot_for_page_id(uint8_t page_id)
{
    for (uint8_t i = 0; i < UX_NAVIGABLE_COUNT; i++) {
        if (s_carousel_order[i] == page_id) {
            return i;
        }
    }
    return 0xFFU;
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
    uint8_t total_subpages = s_pages[s_current_page_id].subpage_count();
    if (total_subpages == 0U) {
        total_subpages = 1U;
    }

    if (nav_page_is_boot_logo(s_current_page_id)) {
        ESP_LOGI(TAG, "%s -> page=%s, subpage=%u/%u",
                 reason,
                 s_pages[s_current_page_id].name,
                 (unsigned)(s_current_subpage_index + 1U),
                 (unsigned)total_subpages);
        return;
    }

    ESP_LOGI(TAG,
             "%s -> page=%s (slot %u/%u), subpage=%u/%u",
             reason,
             s_pages[s_current_page_id].name,
             (unsigned)(s_carousel_slot + 1U),
             (unsigned)UX_NAVIGABLE_COUNT,
             (unsigned)(s_current_subpage_index + 1U),
             (unsigned)total_subpages);
}

static uint8_t nav_max_subpages_for(uint8_t page_id)
{
    uint8_t n = s_pages[page_id].subpage_count();
    return (n == 0U) ? 1U : n;
}

static uint8_t nav_clamp_subpage(uint8_t page_id, uint8_t candidate)
{
    uint8_t max_sp = nav_max_subpages_for(page_id);
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

static esp_err_t nav_validate_order(const uint8_t *order, size_t count)
{
    if (order == NULL || count != UX_NAVIGABLE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    bool seen[UX_PAGE_COUNT] = {false};
    for (size_t i = 0; i < count; i++) {
        uint8_t page_id = order[i];
        if (!nav_page_is_navigable(page_id) || seen[page_id]) {
            return ESP_ERR_INVALID_ARG;
        }
        seen[page_id] = true;
    }

    for (uint8_t id = (uint8_t)UX_PAGE_BOOT_LOGO; id <= (uint8_t)UX_PAGE_STATUS; id++) {
        if (!seen[id]) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static esp_err_t nav_validate_order_v3_legacy(const uint8_t *order, size_t count)
{
    if (order == NULL || count != UX_NAVIGABLE_COUNT_V3_LEGACY) {
        return ESP_ERR_INVALID_ARG;
    }

    bool seen[UX_PAGE_COUNT] = {false};
    for (size_t i = 0; i < count; i++) {
        uint8_t page_id = order[i];
        if (page_id < (uint8_t)UX_PAGE_CLOCK || page_id > (uint8_t)UX_PAGE_STATUS || seen[page_id]) {
            return ESP_ERR_INVALID_ARG;
        }
        seen[page_id] = true;
    }

    for (uint8_t id = (uint8_t)UX_PAGE_CLOCK; id <= (uint8_t)UX_PAGE_STATUS; id++) {
        if (!seen[id]) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static void nav_reset_defaults(void)
{
    (void)memset(s_last_subpage, 0, sizeof(s_last_subpage));
    nav_set_default_carousel_order();
    s_carousel_slot = 0U;
    s_current_page_id = s_carousel_order[s_carousel_slot];
    s_current_subpage_index = 0U;
}

static esp_err_t nav_save_state(void)
{
    ux_nav_persisted_state_v3_t state = {
        .magic = NAV_STATE_MAGIC_V3,
        .carousel_slot = s_carousel_slot,
    };

    (void)memcpy(state.carousel_order, s_carousel_order, sizeof(state.carousel_order));
    for (uint8_t i = 0; i < (uint8_t)UX_PAGE_COUNT; i++) {
        state.last_subpage[i] = s_last_subpage[i];
    }

    return storage_write_file(NAV_STATE_FILE_PATH, &state, sizeof(state));
}

static void nav_apply_restored_page(uint8_t page_id, uint8_t subpage)
{
    if (nav_page_is_boot_logo(page_id) || !nav_page_is_navigable(page_id)) {
        page_id = (uint8_t)UX_PAGE_CLOCK;
    }

    uint8_t slot = nav_slot_for_page_id(page_id);
    if (slot == 0xFFU) {
        nav_set_default_carousel_order();
        slot = 0U;
        page_id = s_carousel_order[0];
    }

    s_carousel_slot = slot;
    s_current_page_id = page_id;
    s_current_subpage_index = nav_clamp_subpage(page_id, subpage);
    s_last_subpage[page_id] = s_current_subpage_index;
}

static void nav_restore_or_default(void)
{
    ux_nav_persisted_state_v3_t v3 = {0};
    ux_nav_persisted_state_v2_t v2 = {0};
    ux_nav_persisted_legacy_t leg = {0};
    size_t bytes_read = 0;

    nav_reset_defaults();

    if (!storage_file_exists(NAV_STATE_FILE_PATH)) {
        return;
    }

    ux_nav_persisted_state_v3_legacy_t v3_legacy = {0};
    if (storage_read_file(NAV_STATE_FILE_PATH, &v3, sizeof(v3), &bytes_read) == ESP_OK &&
        bytes_read == sizeof(v3) && v3.magic == NAV_STATE_MAGIC_V3 &&
        v3.carousel_slot < UX_NAVIGABLE_COUNT &&
        nav_validate_order(v3.carousel_order, UX_NAVIGABLE_COUNT) == ESP_OK) {
        (void)memcpy(s_carousel_order, v3.carousel_order, sizeof(s_carousel_order));
        (void)memcpy(s_last_subpage, v3.last_subpage, sizeof(s_last_subpage));
        nav_clamp_all_last_subpages();
        s_carousel_slot = v3.carousel_slot;
        if (s_carousel_slot >= UX_NAVIGABLE_COUNT) {
            s_carousel_slot = 0U;
        }
        s_current_page_id = s_carousel_order[s_carousel_slot];
        if (!nav_page_is_navigable(s_current_page_id)) {
            nav_reset_defaults();
            return;
        }
        s_current_subpage_index = nav_clamp_subpage(s_current_page_id,
                                                    s_last_subpage[s_current_page_id]);
        return;
    }

    if (storage_read_file(NAV_STATE_FILE_PATH, &v3_legacy, sizeof(v3_legacy), &bytes_read) == ESP_OK &&
        bytes_read == sizeof(v3_legacy) && v3_legacy.magic == NAV_STATE_MAGIC_V3 &&
        v3_legacy.carousel_slot < UX_NAVIGABLE_COUNT_V3_LEGACY &&
        nav_validate_order_v3_legacy(v3_legacy.carousel_order, UX_NAVIGABLE_COUNT_V3_LEGACY) == ESP_OK) {
        (void)memcpy(s_carousel_order, v3_legacy.carousel_order,
                     UX_NAVIGABLE_COUNT_V3_LEGACY * sizeof(s_carousel_order[0]));
        s_carousel_order[UX_NAVIGABLE_COUNT - 1U] = (uint8_t)UX_PAGE_BOOT_LOGO;
        (void)memcpy(s_last_subpage, v3_legacy.last_subpage, sizeof(s_last_subpage));
        nav_clamp_all_last_subpages();
        s_carousel_slot = v3_legacy.carousel_slot;
        if (s_carousel_slot >= UX_NAVIGABLE_COUNT) {
            s_carousel_slot = 0U;
        }
        s_current_page_id = s_carousel_order[s_carousel_slot];
        s_current_subpage_index = nav_clamp_subpage(s_current_page_id,
                                                    s_last_subpage[s_current_page_id]);
        ESP_LOGI(TAG, "Upgraded carousel state (5 pages) -> 6 with Boot Logo last");
        return;
    }

    if (storage_read_file(NAV_STATE_FILE_PATH, &v2, sizeof(v2), &bytes_read) == ESP_OK &&
        bytes_read == sizeof(v2) && v2.magic == NAV_STATE_MAGIC_V2 &&
        v2.page_index < (uint8_t)UX_PAGE_COUNT) {
        (void)memcpy(s_last_subpage, v2.last_subpage, sizeof(s_last_subpage));
        nav_clamp_all_last_subpages();
        nav_apply_restored_page(v2.page_index, s_last_subpage[v2.page_index]);
        return;
    }

    if (storage_read_file(NAV_STATE_FILE_PATH, &leg, sizeof(leg), &bytes_read) == ESP_OK &&
        bytes_read == sizeof(leg) && leg.magic == NAV_STATE_MAGIC_LEGACY &&
        leg.page_index < (uint8_t)UX_PAGE_COUNT) {
        nav_apply_restored_page(leg.page_index, leg.subpage_index);
        return;
    }

    ESP_LOGW(TAG, "Invalid nav state file, using defaults");
}

static esp_err_t nav_render_current_page(void)
{
    if (s_last_rendered_page_id != 0xFFU &&
        s_last_rendered_page_id == (uint8_t)UX_PAGE_STATUS) {
        page_status_prepare_leave();
    }

    const bool cold_boot_logo =
        s_boot_logo_use_startup_timing &&
        nav_page_is_boot_logo(s_current_page_id);

    if (cold_boot_logo) {
        lv_obj_t *scr = lv_scr_act();
        esp_err_t ret =
            s_pages[s_current_page_id].render(s_current_subpage_index, scr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Render failed for page %s", s_pages[s_current_page_id].name);
            return ret;
        }
        lv_refr_now(lv_disp_get_default());
        boot_logo_enable_backlight(70);
        s_last_rendered_page_id = s_current_page_id;
        nav_log_state("Render");
        return ESP_OK;
    }

    const bool leaving_boot_splash =
        (s_last_rendered_page_id == (uint8_t)UX_PAGE_BOOT_LOGO);

    lv_obj_t *old_scr = lv_scr_act();
    if (leaving_boot_splash) {
        lv_img_cache_invalidate_src("A:/boot/cobra_boot.bin");
        lv_img_cache_invalidate_src(NULL);
    } else {
        lv_img_cache_invalidate_src(NULL);
    }

    lv_obj_t *new_scr = lv_obj_create(NULL);
    if (new_scr == NULL) {
        ESP_LOGE(TAG, "Failed to allocate screen for page %s", s_pages[s_current_page_id].name);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(new_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(new_scr, LV_OPA_COVER, LV_PART_MAIN);

    ESP_LOGI(TAG, "Rendering %s on new screen", s_pages[s_current_page_id].name);
    esp_err_t ret =
        s_pages[s_current_page_id].render(s_current_subpage_index, new_scr);
    if (ret != ESP_OK) {
        lv_obj_del(new_scr);
        ESP_LOGE(TAG, "Render failed for page %s", s_pages[s_current_page_id].name);
        return ret;
    }

    lv_scr_load(new_scr);
    if (leaving_boot_splash) {
        lv_obj_del_async(old_scr);
    } else {
        lv_obj_del(old_scr);
    }
    lv_refr_now(lv_disp_get_default());

    s_last_rendered_page_id = s_current_page_id;
    nav_log_state("Render");
    return ESP_OK;
}

static void nav_next_page(void)
{
    s_last_subpage[s_current_page_id] = s_current_subpage_index;

    s_carousel_slot = (uint8_t)((s_carousel_slot + 1U) % UX_NAVIGABLE_COUNT);
    s_current_page_id = s_carousel_order[s_carousel_slot];
    s_current_subpage_index = nav_clamp_subpage(s_current_page_id,
                                                s_last_subpage[s_current_page_id]);
    s_last_subpage[s_current_page_id] = s_current_subpage_index;

    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after next");
    }
    (void)nav_save_state();
}

static void nav_prev_page(void)
{
    s_last_subpage[s_current_page_id] = s_current_subpage_index;

    if (s_carousel_slot == 0U) {
        s_carousel_slot = UX_NAVIGABLE_COUNT - 1U;
    } else {
        s_carousel_slot--;
    }

    s_current_page_id = s_carousel_order[s_carousel_slot];
    s_current_subpage_index = nav_clamp_subpage(s_current_page_id,
                                                s_last_subpage[s_current_page_id]);
    s_last_subpage[s_current_page_id] = s_current_subpage_index;

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
    s_last_subpage[s_current_page_id] = s_current_subpage_index;
    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after subpage up");
    }
    (void)nav_save_state();
}

static void nav_subpage_down(void)
{
    uint8_t max_subpages = s_pages[s_current_page_id].subpage_count();
    if (max_subpages == 0U || s_current_subpage_index + 1U >= max_subpages) {
        nav_log_state("Subpage down (bottom)");
        return;
    }
    s_current_subpage_index++;
    s_last_subpage[s_current_page_id] = s_current_subpage_index;
    if (nav_render_current_page() != ESP_OK) {
        ESP_LOGE(TAG, "Page render failed after subpage down");
    }
    (void)nav_save_state();
}

static void nav_apply_page_order(const uint8_t *order)
{
    uint8_t active_page = s_current_page_id;
    if (nav_page_is_boot_logo(active_page)) {
        active_page = s_carousel_order[s_carousel_slot];
    }

    (void)memcpy(s_carousel_order, order, UX_NAVIGABLE_COUNT);

    uint8_t slot = nav_slot_for_page_id(active_page);
    if (slot == 0xFFU) {
        slot = 0U;
    }
    s_carousel_slot = slot;
    s_current_page_id = s_carousel_order[s_carousel_slot];
    s_current_subpage_index = nav_clamp_subpage(s_current_page_id,
                                                s_last_subpage[s_current_page_id]);
    s_last_subpage[s_current_page_id] = s_current_subpage_index;

    if (s_ui_ready && !nav_page_is_boot_logo(s_last_rendered_page_id)) {
        if (nav_render_current_page() != ESP_OK) {
            ESP_LOGE(TAG, "Page render failed after order change");
        }
    }

    (void)nav_save_state();
    ESP_LOGI(TAG, "Carousel order updated");
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

    s_order_cmd_queue = xQueueCreate(4, sizeof(nav_order_cmd_t));
    if (s_order_cmd_queue == NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
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
    s_current_page_id = (uint8_t)UX_PAGE_BOOT_LOGO;
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
    if (nav_page_is_boot_logo(s_current_page_id)) {
        nav_apply_restored_page((uint8_t)UX_PAGE_CLOCK, s_last_subpage[UX_PAGE_CLOCK]);
    }

    esp_err_t ret = nav_render_current_page();
    if (ret == ESP_OK) {
        (void)nav_save_state();
    } else {
        ESP_LOGE(TAG, "Restored page render failed");
    }
    s_ui_ready = true;
    return ret;
}

ux_page_id_t ux_navigation_get_active_page(void)
{
    if (!s_initialized || s_current_page_id >= (uint8_t)UX_PAGE_COUNT) {
        return UX_PAGE_BOOT_LOGO;
    }
    return (ux_page_id_t)s_current_page_id;
}

esp_err_t ux_navigation_get_page_order(uint8_t *order, size_t count)
{
    if (order == NULL || count != UX_NAVIGABLE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)memcpy(order, s_carousel_order, UX_NAVIGABLE_COUNT);
    return ESP_OK;
}

const char *ux_navigation_page_name(ux_page_id_t page_id)
{
    if ((uint8_t)page_id >= (uint8_t)UX_PAGE_COUNT) {
        return "";
    }
    return s_pages[page_id].name;
}

esp_err_t ux_navigation_request_set_page_order(const uint8_t *order, size_t count)
{
    if (!s_initialized || s_order_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nav_validate_order(order, count);
    if (err != ESP_OK) {
        return err;
    }

    nav_order_cmd_t cmd = {0};
    (void)memcpy(cmd.order, order, UX_NAVIGABLE_COUNT);
    if (xQueueSend(s_order_cmd_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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
    if (!s_initialized || !s_ui_ready) {
        return;
    }

    nav_order_cmd_t order_cmd;
    while (s_order_cmd_queue != NULL &&
           xQueueReceive(s_order_cmd_queue, &order_cmd, 0) == pdTRUE) {
        nav_apply_page_order(order_cmd.order);
    }

    if (s_event_queue == NULL) {
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
