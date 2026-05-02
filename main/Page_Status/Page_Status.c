#include "Page_Status.h"

#include <stdio.h>
#include <string.h>

#include "gauge_bg_lvgl.h"
#include "UI_Navigation.h"
#include "Wireless.h"
#include "lvgl.h"

static lv_timer_t *s_status_ble_timer;

static lv_obj_t *s_conn_label;
static lv_obj_t *s_name_label;
static lv_obj_t *s_mac_label;
static lv_obj_t *s_batt_label;

static void status_refresh_timer(lv_timer_t *t)
{
    if (ux_navigation_get_active_page() != UX_PAGE_STATUS) {
        if (t != NULL) {
            lv_timer_del(t);
        }
        s_status_ble_timer = NULL;
        s_conn_label = NULL;
        s_name_label = NULL;
        s_mac_label = NULL;
        s_batt_label = NULL;
        return;
    }

    if (s_conn_label == NULL) {
        return;
    }

    bt_remote_conn_state_t st = Wireless_GetRemoteConnectionState();
    const bool is_connected = (st == BT_REMOTE_CONN_CONNECTED);
    const char *conn_str = is_connected ? "Connected" : "Not Connected";

    lv_label_set_text(s_conn_label, conn_str);

    if (!is_connected) {
        lv_obj_add_flag(s_name_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mac_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_name_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_mac_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_batt_label, LV_OBJ_FLAG_HIDDEN);

    char name[72];
    Wireless_GetRemoteDisplayName(name, sizeof(name));
    if (name[0] == '\0') {
        strncpy(name, "---", sizeof(name) - 1U);
        name[sizeof(name) - 1U] = '\0';
    }
    lv_label_set_text(s_name_label, name);

    char mac[24];
    (void)Wireless_FormatRemoteMac(mac, sizeof(mac));
    lv_label_set_text(s_mac_label, mac);

    char batt_line[40];
    uint8_t pct = 0;
    if (Wireless_GetRemoteBatteryPercent(&pct)) {
        (void)snprintf(batt_line, sizeof(batt_line), "Battery: %u%%", (unsigned)pct);
    } else {
        (void)snprintf(batt_line, sizeof(batt_line), "Battery: N/A");
    }
    lv_label_set_text(s_batt_label, batt_line);
}

static void style_status_label(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
}

void page_status_prepare_leave(void)
{
    if (s_status_ble_timer != NULL) {
        lv_timer_del(s_status_ble_timer);
        s_status_ble_timer = NULL;
    }
    s_conn_label = NULL;
    s_name_label = NULL;
    s_mac_label = NULL;
    s_batt_label = NULL;
}

esp_err_t page_status_render(uint8_t subpage_index, lv_obj_t *root)
{
    (void)subpage_index;

    if (s_status_ble_timer != NULL) {
        lv_timer_del(s_status_ble_timer);
        s_status_ble_timer = NULL;
    }
    s_conn_label = NULL;
    s_name_label = NULL;
    s_mac_label = NULL;
    s_batt_label = NULL;

    (void)create_gauge_background(root);

    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    /* Content constrained to inscribed square inside ring + cardinal ticks (+ margin). */
    lv_obj_t *outer = lv_obj_create(root);
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, GAUGE_USABLE_SQUARE_SIDE, GAUGE_USABLE_SQUARE_SIDE);
    lv_obj_align(outer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_layout(outer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(outer, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_left(outer, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(outer, 4, LV_PART_MAIN);

    lv_obj_t *inner = lv_obj_create(outer);
    lv_obj_remove_style_all(inner);
    lv_obj_set_width(inner, lv_pct(100));
    lv_obj_set_height(inner, LV_SIZE_CONTENT);
    lv_obj_set_layout(inner, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(inner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(inner, 4, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(inner);
    style_status_label(title);
    lv_label_set_text(title, "BT");

    s_conn_label = lv_label_create(inner);
    style_status_label(s_conn_label);

    s_name_label = lv_label_create(inner);
    style_status_label(s_name_label);

    s_mac_label = lv_label_create(inner);
    style_status_label(s_mac_label);

    s_batt_label = lv_label_create(inner);
    style_status_label(s_batt_label);

    lv_obj_t *rule = lv_obj_create(outer);
    lv_obj_remove_style_all(rule);
    lv_obj_set_width(rule, lv_pct(90));
    lv_obj_set_height(rule, 2);
    lv_obj_set_style_bg_color(rule, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(rule, 0, LV_PART_MAIN);

    status_refresh_timer(NULL);

    s_status_ble_timer = lv_timer_create(status_refresh_timer, 800, NULL);

    return ESP_OK;
}

uint8_t page_status_get_subpage_count(void)
{
    return 1;
}
