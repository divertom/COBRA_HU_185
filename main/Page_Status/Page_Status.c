#include "Page_Status.h"

#include <stdio.h>
#include <string.h>

#include "UI_Navigation.h"
#include "Wireless.h"
#include "lvgl.h"

static lv_timer_t *s_status_ble_timer;
static lv_obj_t *s_status_ble_label;

static void status_refresh_timer(lv_timer_t *t)
{
    if (ux_navigation_get_active_page() != UX_PAGE_STATUS) {
        if (t != NULL) {
            lv_timer_del(t);
        }
        s_status_ble_timer = NULL;
        s_status_ble_label = NULL;
        return;
    }

    if (s_status_ble_label == NULL) {
        return;
    }

    const char *conn_str;
    bt_remote_conn_state_t st = Wireless_GetRemoteConnectionState();
    switch (st) {
        case BT_REMOTE_CONN_CONNECTED:
            conn_str = "Connected";
            break;
        case BT_REMOTE_CONN_CONNECTING:
            conn_str = "Connecting";
            break;
        default:
            conn_str = "Disconnected";
            break;
    }

    char name[72];
    Wireless_GetRemoteDisplayName(name, sizeof(name));
    if (name[0] == '\0') {
        strncpy(name, "---", sizeof(name) - 1U);
        name[sizeof(name) - 1U] = '\0';
    }

    char mac[24];
    (void)Wireless_FormatRemoteMac(mac, sizeof(mac));

    char batt_line[32];
    uint8_t pct = 0;
    if (Wireless_GetRemoteBatteryPercent(&pct)) {
        (void)snprintf(batt_line, sizeof(batt_line), "%u%%", (unsigned)pct);
    } else {
        strncpy(batt_line, "N/A", sizeof(batt_line) - 1U);
        batt_line[sizeof(batt_line) - 1U] = '\0';
    }

    char body[288];
    (void)snprintf(body, sizeof(body),
                   "Bluetooth\n"
                   "\n"
                   "Connection: %s\n"
                   "\n"
                   "Name: %s\n"
                   "\n"
                   "MAC: %s\n"
                   "\n"
                   "Battery: %s",
                   conn_str, name, mac, batt_line);

    lv_label_set_text(s_status_ble_label, body);
}

esp_err_t page_status_render(uint8_t subpage_index)
{
    (void)subpage_index;

    if (s_status_ble_timer != NULL) {
        lv_timer_del(s_status_ble_timer);
        s_status_ble_timer = NULL;
    }
    s_status_ble_label = NULL;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_status_ble_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status_ble_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_long_mode(s_status_ble_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_ble_label, lv_pct(92));
    lv_obj_align(s_status_ble_label, LV_ALIGN_TOP_MID, 0, 8);

    status_refresh_timer(NULL);

    s_status_ble_timer = lv_timer_create(status_refresh_timer, 800, NULL);

    return ESP_OK;
}

uint8_t page_status_get_subpage_count(void)
{
    return 1;
}
