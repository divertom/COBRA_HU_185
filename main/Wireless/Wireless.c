#include "Wireless.h"

#include "Config_Portal.h"
#include "Storage_Manager.h"
#include "tpms_manager.h"
#include "cJSON.h"
#if ENABLE_TESLA_TPMS_DEBUG
#include "tesla_tpms_ble.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "esp_bt_defs.h"
#include "esp_gatt_common_api.h"
#include <strings.h>

uint16_t BLE_NUM = 0;
uint16_t WIFI_NUM = 0;
bool Scan_finish = 0;

bool WiFi_Scan_Finish = 0;
bool BLE_Scan_Finish = 0;

#define GATTC_TAG "BT_REMOTE"
#define REMOTE_NAME "SmartRemote"
#define REMOTE_APP_ID 0
#define HID_SERVICE_UUID 0x1812
#define HID_REPORT_CHAR_UUID 0x2A4D
#define BAT_SERVICE_UUID 0x180F
#define BAT_LEVEL_CHAR_UUID 0x2A19
#define CCCD_UUID 0x2902
#define MAX_BONDED_DEVICES 8

#define REMOTE_BAT_PCT_UNKNOWN 255u

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static bool s_is_connecting = false;
static bool s_is_connected = false;
static bt_remote_event_handler_t s_remote_event_handler = NULL;
static esp_bd_addr_t s_remote_bda = {0};
static esp_ble_addr_type_t s_remote_addr_type = BLE_ADDR_TYPE_PUBLIC;
static uint16_t s_conn_id = 0;
static uint16_t s_hid_service_start = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_hid_service_end = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_hid_report_char_handle = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_bat_service_start = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_bat_service_end = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_bat_level_char_handle = ESP_GATT_ILLEGAL_HANDLE;
static uint8_t s_remote_battery_percent = REMOTE_BAT_PCT_UNKNOWN;
static char s_remote_display_name[64];
static esp_bd_addr_t s_bonded_bda[MAX_BONDED_DEVICES];
static uint16_t s_bonded_count = 0;

typedef enum {
    DISC_PHASE_HID = 0,
    DISC_PHASE_BAT,
} disc_phase_t;

static disc_phase_t s_disc_phase = DISC_PHASE_HID;
static bool s_ble_scan_active = false;

static const esp_bt_uuid_t s_hid_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = HID_SERVICE_UUID},
};
static const esp_bt_uuid_t s_bat_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = BAT_SERVICE_UUID},
};

static void store_battery_level_byte(uint8_t raw)
{
    if (raw <= 100u) {
        s_remote_battery_percent = raw;
    }
}

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
};

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void start_ble_scan(void);
static bool match_target_remote_name(const esp_ble_gap_cb_param_t *scan_rst);
static bool is_bonded_device(const esp_bd_addr_t bda);
static void refresh_bonded_device_list(void);
static bt_remote_event_t usage_to_event(uint16_t usage);
static bool equals_ignore_case_ascii(const char *a, const char *b, size_t len);
static void stash_remote_adv_display_name(const esp_ble_gap_cb_param_t *gap_param);
static void write_cccd_enable_notify(esp_gatt_if_t gattc_if, uint16_t char_handle,
                                     uint16_t svc_start, uint16_t svc_end);
static void try_discover_battery_and_read(esp_gatt_if_t gattc_if);
static void start_hid_service_discovery(esp_gatt_if_t gattc_if);
static void finalize_hid_characteristics(esp_gatt_if_t gattc_if);

void Wireless_Init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Start BLE first; WiFi is brought up from BLE_Init after the controller is ready. */
    xTaskCreatePinnedToCore(
        BLE_Init,
        "BLE task",
        4096,
        NULL,
        2,
        NULL,
        0);
}

#define DEFAULT_PORTAL_SSID "Cobra HU Service"
#define DEVICE_CONFIG_PATH  "/config/device_config.json"

static char s_ap_ssid[33];
static bool s_ap_ssid_loaded;

static const char *portal_ssid_from_json(cJSON *root)
{
    if (root == NULL) {
        return NULL;
    }

    cJSON *portal = cJSON_GetObjectItem(root, "service_portal");
    cJSON *ssid = portal != NULL ? cJSON_GetObjectItem(portal, "ap_ssid") : NULL;
    if (cJSON_IsString(ssid)) {
        const char *v = cJSON_GetStringValue(ssid);
        if (v != NULL && v[0] != '\0') {
            return v;
        }
    }

    /* Legacy: wifi.ssid before service_portal.ap_ssid existed. */
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    ssid = wifi != NULL ? cJSON_GetObjectItem(wifi, "ssid") : NULL;
    if (cJSON_IsString(ssid)) {
        const char *v = cJSON_GetStringValue(ssid);
        if (v != NULL && v[0] != '\0') {
            return v;
        }
    }

    return NULL;
}

static void load_ap_ssid_from_config(void)
{
    if (s_ap_ssid_loaded) {
        return;
    }

    strncpy(s_ap_ssid, DEFAULT_PORTAL_SSID, sizeof(s_ap_ssid) - 1);
    s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';

    if (storage_file_exists(DEVICE_CONFIG_PATH)) {
        char buf[1024];
        size_t bytes_read = 0;
        if (storage_read_file(DEVICE_CONFIG_PATH, buf, sizeof(buf) - 1, &bytes_read) == ESP_OK) {
            buf[bytes_read] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root != NULL) {
                const char *configured = portal_ssid_from_json(root);
                if (configured != NULL) {
                    strncpy(s_ap_ssid, configured, sizeof(s_ap_ssid) - 1);
                    s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
                }
                cJSON_Delete(root);
            }
        }
    }

    s_ap_ssid_loaded = true;
}

void Wireless_GetApSsid(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    load_ap_ssid_from_config();
    strncpy(out, s_ap_ssid, out_len - 1U);
    out[out_len - 1U] = '\0';
}

void WIFI_Init(void *arg)
{
    load_ap_ssid_from_config();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(GATTC_TAG, "Service portal AP up, starting captive portal (free heap=%u)",
             (unsigned)esp_get_free_heap_size());
    esp_err_t portal_ret = config_portal_start();
    if (portal_ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "Config portal start failed: %s (free heap=%u)",
                 esp_err_to_name(portal_ret), (unsigned)esp_get_free_heap_size());
    }

    /* Skip blocking scan at boot — it contends with BLE GATT discovery for internal heap. */
    WiFi_Scan_Finish = 1;
    WIFI_NUM = 0;
    if (BLE_Scan_Finish == 1) {
        Scan_finish = 1;
    }
    ESP_LOGI(GATTC_TAG, "Service portal AP \"%s\" up, open (no password)", s_ap_ssid);

    vTaskDelete(NULL);
}

uint16_t WIFI_Scan(void)
{
    ESP_LOGW(GATTC_TAG, "WIFI_Scan ignored (AP-only mode)");
    WiFi_Scan_Finish = 1;
    if (BLE_Scan_Finish == 1) {
        Scan_finish = 1;
    }
    return 0;
}

void BLE_Init(void *arg)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTC_TAG, "controller init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTC_TAG, "controller enable failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTC_TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTC_TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    refresh_bonded_device_list();

    /* Request bonding/security so the remote will authenticate before we subscribe notifications. */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND; /* Just Works style (no MITM) */
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;        /* No input/output */
    uint8_t key_size = 16;                           /* 7..16 bytes */
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(REMOTE_APP_ID));
    tpms_manager_init();
    tesla_tpms_init();

    xTaskCreatePinnedToCore(
        WIFI_Init,
        "WIFI task",
        4096,
        NULL,
        1,
        NULL,
        0);

    vTaskDelete(NULL);
}

uint16_t BLE_Scan(void)
{
    BLE_NUM = 0;
    start_ble_scan();
    return BLE_NUM;
}

void Wireless_LogRemoteEvent(uint16_t usage, bt_remote_event_t event)
{
    uint8_t low = (uint8_t)(usage & 0x00FF);
    switch (event) {
        case BT_REMOTE_EVENT_VOL_UP:
            ESP_LOGI(GATTC_TAG, "%02X: Vol+", low);
            break;
        case BT_REMOTE_EVENT_VOL_DOWN:
            ESP_LOGI(GATTC_TAG, "%02X: Vol-", low);
            break;
        case BT_REMOTE_EVENT_NEXT_TRACK:
            ESP_LOGI(GATTC_TAG, "%02X: Forward", low);
            break;
        case BT_REMOTE_EVENT_PREV_TRACK:
            ESP_LOGI(GATTC_TAG, "%02X: Back", low);
            break;
        case BT_REMOTE_EVENT_PLAY_PAUSE:
            ESP_LOGI(GATTC_TAG, "%02X: Play", low);
            break;
        default:
            break;
    }
}

void Wireless_RegisterRemoteEventHandler(bt_remote_event_handler_t handler)
{
    s_remote_event_handler = handler;
}

static void write_cccd_enable_notify(esp_gatt_if_t gattc_if, uint16_t char_handle,
                                     uint16_t svc_start, uint16_t svc_end)
{
    uint16_t count = 0;
    esp_bt_uuid_t cccd_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {.uuid16 = CCCD_UUID}
    };

    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
        gattc_if,
        s_conn_id,
        ESP_GATT_DB_DESCRIPTOR,
        svc_start,
        svc_end,
        char_handle,
        &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGE(GATTC_TAG, "CCCD count failed char=0x%04x", char_handle);
        return;
    }

    esp_gattc_descr_elem_t *descr = (esp_gattc_descr_elem_t *)calloc(count, sizeof(esp_gattc_descr_elem_t));
    if (descr == NULL) {
        ESP_LOGE(GATTC_TAG, "No memory for descriptors");
        return;
    }

    status = esp_ble_gattc_get_descr_by_char_handle(
        gattc_if,
        s_conn_id,
        char_handle,
        cccd_uuid,
        descr,
        &count);
    if (status == ESP_GATT_OK && count > 0) {
        uint16_t notify_en = 1;
        esp_ble_gattc_write_char_descr(
            gattc_if,
            s_conn_id,
            descr[0].handle,
            sizeof(notify_en),
            (uint8_t *)&notify_en,
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);
        ESP_LOGI(GATTC_TAG, "Notify CCCD enabled (char handle 0x%04x)", char_handle);
    } else {
        ESP_LOGE(GATTC_TAG, "CCCD descr not found (char handle 0x%04x)", char_handle);
    }
    free(descr);
}

static void try_discover_battery_and_read(esp_gatt_if_t gattc_if)
{
    if (s_bat_service_start == ESP_GATT_ILLEGAL_HANDLE) {
        return;
    }

    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid = {.uuid16 = BAT_LEVEL_CHAR_UUID}};
    uint16_t count = 0;
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
        gattc_if,
        s_conn_id,
        ESP_GATT_DB_CHARACTERISTIC,
        s_bat_service_start,
        s_bat_service_end,
        ESP_GATT_ILLEGAL_HANDLE,
        &count);
    if (status != ESP_GATT_OK || count == 0) {
        return;
    }

    esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)calloc(count, sizeof(esp_gattc_char_elem_t));
    if (chars == NULL) {
        return;
    }

    status = esp_ble_gattc_get_char_by_uuid(
        gattc_if,
        s_conn_id,
        s_bat_service_start,
        s_bat_service_end,
        uuid,
        chars,
        &count);
    if (status != ESP_GATT_OK || count == 0) {
        free(chars);
        return;
    }

    s_bat_level_char_handle = chars[0].char_handle;
    free(chars);

    ESP_LOGI(GATTC_TAG, "Battery level char handle=0x%04x", s_bat_level_char_handle);
    esp_err_t err = esp_ble_gattc_read_char(gattc_if, s_conn_id, s_bat_level_char_handle, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(GATTC_TAG, "Battery read failed: %s", esp_err_to_name(err));
    }
    err = esp_ble_gattc_register_for_notify(gattc_if, s_remote_bda, s_bat_level_char_handle);
    if (err != ESP_OK) {
        ESP_LOGW(GATTC_TAG, "Battery notify register failed: %s", esp_err_to_name(err));
    }
}

static bool is_bonded_device(const esp_bd_addr_t bda)
{
    for (uint16_t i = 0; i < s_bonded_count; i++) {
        if (memcmp(s_bonded_bda[i], bda, sizeof(esp_bd_addr_t)) == 0) {
            return true;
        }
    }
    return false;
}

static void refresh_bonded_device_list(void)
{
    s_bonded_count = 0;
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num <= 0) {
        ESP_LOGI(GATTC_TAG, "No bonded BLE devices in NVS");
        return;
    }

    if (dev_num > MAX_BONDED_DEVICES) {
        dev_num = MAX_BONDED_DEVICES;
    }

    esp_ble_bond_dev_t dev_list[MAX_BONDED_DEVICES];
    memset(dev_list, 0, sizeof(dev_list));

    int list_count = dev_num;
    esp_err_t ret = esp_ble_get_bond_device_list(&list_count, dev_list);
    if (ret != ESP_OK) {
        ESP_LOGW(GATTC_TAG, "Failed reading bonded list: %s", esp_err_to_name(ret));
        return;
    }

    for (int i = 0; i < list_count; i++) {
        memcpy(s_bonded_bda[s_bonded_count], dev_list[i].bd_addr, sizeof(esp_bd_addr_t));
        s_bonded_count++;
    }

    ESP_LOGI(GATTC_TAG, "Loaded %u bonded BLE device(s)", (unsigned)s_bonded_count);
}

void Wireless_DecodeHidReport(const uint8_t *report_data, uint16_t report_len)
{
    if (report_data == NULL || report_len == 0) {
        return;
    }

    /* Many remotes report a 2-byte HID Consumer control usage:
     *   [0]=low byte, [1]=high byte
     * We only want press events, not the 0x0000 release packet. */
    if (report_len < 2) {
        return;
    }

    static uint16_t last_press_usage = 0;
    uint16_t usage = (uint16_t)report_data[0] | ((uint16_t)report_data[1] << 8);

    if (usage == 0x0000) {
        last_press_usage = 0; /* allow same button pressed again */
        return;
    }

    if (usage == last_press_usage) {
        return; /* ignore immediate duplicate */
    }
    last_press_usage = usage;

    bt_remote_event_t event = usage_to_event(usage);
    if (event == BT_REMOTE_EVENT_UNKNOWN) {
        return;
    }

    Wireless_LogRemoteEvent(usage, event);
    if (s_remote_event_handler != NULL) {
        s_remote_event_handler(event);
    }
}

static bt_remote_event_t usage_to_event(uint16_t usage)
{
    switch (usage) {
        /* Calibrated from your observed HID payload examples:
         *   10 00 => 0x0010 => Play
         *   01 00 => 0x0001 => Vol+
         *   02 00 => 0x0002 => Vol-
         *   08 00 => 0x0008 => Back
         *   04 00 => 0x0004 => Forward */
        case 0x0010:
            return BT_REMOTE_EVENT_PLAY_PAUSE;
        case 0x0001:
            return BT_REMOTE_EVENT_VOL_UP;
        case 0x0002:
            return BT_REMOTE_EVENT_VOL_DOWN;
        case 0x0008:
            return BT_REMOTE_EVENT_PREV_TRACK;
        case 0x0004:
            return BT_REMOTE_EVENT_NEXT_TRACK;
        default:
            return BT_REMOTE_EVENT_UNKNOWN;
    }
}

static void start_hid_service_discovery(esp_gatt_if_t gattc_if)
{
    s_disc_phase = DISC_PHASE_HID;
    s_hid_service_start = ESP_GATT_ILLEGAL_HANDLE;
    s_hid_service_end = ESP_GATT_ILLEGAL_HANDLE;
    s_hid_report_char_handle = ESP_GATT_ILLEGAL_HANDLE;
    esp_err_t err = esp_ble_gattc_search_service(gattc_if, s_conn_id, (esp_bt_uuid_t *)&s_hid_service_uuid);
    if (err != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "HID service search failed: %s", esp_err_to_name(err));
        esp_ble_gattc_close(gattc_if, s_conn_id);
    }
}

static void finalize_hid_characteristics(esp_gatt_if_t gattc_if)
{
    if (s_hid_service_start == ESP_GATT_ILLEGAL_HANDLE) {
        ESP_LOGW(GATTC_TAG, "HID service not found (remote may be Classic-only)");
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    uint16_t count = 0;
    esp_bt_uuid_t report_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {.uuid16 = HID_REPORT_CHAR_UUID}
    };
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
        gattc_if,
        s_conn_id,
        ESP_GATT_DB_CHARACTERISTIC,
        s_hid_service_start,
        s_hid_service_end,
        ESP_GATT_ILLEGAL_HANDLE,
        &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(GATTC_TAG, "No HID report characteristics");
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }

    esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)calloc(count, sizeof(esp_gattc_char_elem_t));
    if (chars == NULL) {
        ESP_LOGE(GATTC_TAG, "No memory for char discovery");
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }

    status = esp_ble_gattc_get_char_by_uuid(
        gattc_if,
        s_conn_id,
        s_hid_service_start,
        s_hid_service_end,
        report_uuid,
        chars,
        &count);

    if (status == ESP_GATT_OK && count > 0) {
        s_hid_report_char_handle = chars[0].char_handle;
        ESP_LOGI(GATTC_TAG, "HID report char handle=0x%04x", s_hid_report_char_handle);
        esp_ble_gattc_register_for_notify(gattc_if, s_remote_bda, s_hid_report_char_handle);
    } else {
        ESP_LOGW(GATTC_TAG, "HID report characteristic not found");
        esp_ble_gattc_close(gattc_if, s_conn_id);
    }
    free(chars);
}

static void start_ble_scan(void)
{
    if (s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(GATTC_TAG, "GATTC not ready yet");
        return;
    }
    if ((s_is_connected || s_is_connecting) && !tpms_manager_ble_scan_required()) {
        return;
    }
    if (s_ble_scan_active) {
        return;
    }
    esp_err_t err = esp_ble_gap_start_scanning(0);
    if (err == ESP_OK) {
        ESP_LOGI(GATTC_TAG, "BLE scanning started for %s", REMOTE_NAME);
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_ble_scan_active = true;
    } else {
        ESP_LOGE(GATTC_TAG, "start scanning failed: %s", esp_err_to_name(err));
    }
}

void Wireless_EnsureBleScanActive(void)
{
    start_ble_scan();
}

bool Wireless_IsBleScanActive(void)
{
    return s_ble_scan_active;
}

void Wireless_RestartBleScanForTpms(void)
{
    if (s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(GATTC_TAG, "TPMS scan: GATTC not ready yet");
        return;
    }

    if (s_ble_scan_active) {
        esp_err_t err = esp_ble_gap_stop_scanning();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_ble_scan_active = false;
        } else {
            ESP_LOGW(GATTC_TAG, "TPMS scan stop before restart: %s", esp_err_to_name(err));
        }
    }

    esp_err_t err = esp_ble_gap_start_scanning(0);
    if (err == ESP_OK) {
        s_ble_scan_active = true;
        ESP_LOGI(GATTC_TAG, "BLE scan restarted for TPMS discovery");
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_ble_scan_active = true;
        ESP_LOGI(GATTC_TAG, "BLE scan already active (TPMS)");
    } else {
        ESP_LOGE(GATTC_TAG, "TPMS scan restart failed: %s", esp_err_to_name(err));
        start_ble_scan();
    }
}

static bool match_target_remote_name(const esp_ble_gap_cb_param_t *scan_rst)
{
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data((uint8_t *)scan_rst->scan_rst.ble_adv,
                                              ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (name == NULL || name_len == 0) {
        name = esp_ble_resolve_adv_data((uint8_t *)scan_rst->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (name == NULL || name_len == 0) {
        return false;
    }
    /* Compare case-insensitively; avoid requiring exact case from the phone advertisement. */
    char tmp[64];
    size_t cplen = name_len;
    if (cplen >= sizeof(tmp)) {
        cplen = sizeof(tmp) - 1;
    }
    memcpy(tmp, name, cplen);
    tmp[cplen] = '\0';
    size_t want_len = strlen(REMOTE_NAME);
    if (cplen < want_len) {
        return false;
    }
    return equals_ignore_case_ascii(tmp, REMOTE_NAME, want_len);
}

static void stash_remote_adv_display_name(const esp_ble_gap_cb_param_t *gap_param)
{
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data((uint8_t *)gap_param->scan_rst.ble_adv,
                                             ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (name == NULL || name_len == 0) {
        name = esp_ble_resolve_adv_data((uint8_t *)gap_param->scan_rst.ble_adv,
                                        ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (name == NULL || name_len == 0) {
        s_remote_display_name[0] = '\0';
        return;
    }
    size_t cplen = name_len >= sizeof(s_remote_display_name) ? sizeof(s_remote_display_name) - 1U : (size_t)name_len;
    memcpy(s_remote_display_name, name, cplen);
    s_remote_display_name[cplen] = '\0';
}

static bool equals_ignore_case_ascii(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_SEC_REQ_EVT:
            /* Trigger pairing/bonding security response */
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            refresh_bonded_device_list();
            break;
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (tpms_manager_is_scan_active()) {
                Wireless_RestartBleScanForTpms();
            } else {
                start_ble_scan();
            }
            break;
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                s_ble_scan_active = true;
            } else if (param->scan_start_cmpl.status == ESP_BT_STATUS_BUSY) {
                s_ble_scan_active = true;
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                tpms_manager_on_gap_event(event, param);
                BLE_NUM++;
                bool name_match = match_target_remote_name(param);
                bool bond_match = is_bonded_device(param->scan_rst.bda);
                if (!s_is_connecting && !s_is_connected && (name_match || bond_match) &&
                    !tpms_manager_is_scan_active()) {
                    BLE_Scan_Finish = 1;
                    if (WiFi_Scan_Finish == 1) {
                        Scan_finish = 1;
                    }
                    memcpy(s_remote_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
                    s_remote_addr_type = param->scan_rst.ble_addr_type;
                    s_is_connecting = true;
                    if (name_match) {
                        stash_remote_adv_display_name(param);
                        ESP_LOGI(GATTC_TAG, "Found %s by name, connecting...", REMOTE_NAME);
                    } else {
                        s_remote_display_name[0] = '\0';
                        ESP_LOGI(GATTC_TAG, "Found bonded remote by address, connecting...");
                    }
                    esp_ble_gap_stop_scanning();
                } else {
                    /* Optional debug for name mismatch: only if address is valid and scan_rst is present.
                     * Keep this lightweight; remove later if desired.
                     */
                    /* no-op */
                }
            }
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            s_ble_scan_active = false;
            if (s_is_connecting && s_gattc_if != ESP_GATT_IF_NONE) {
                esp_err_t err = esp_ble_gattc_open(s_gattc_if, s_remote_bda, s_remote_addr_type, true);
                if (err != ESP_OK) {
                    ESP_LOGE(GATTC_TAG, "gattc open failed: %s", esp_err_to_name(err));
                    s_is_connecting = false;
                    start_ble_scan();
                }
            }
            break;
        default:
            break;
    }

#if ENABLE_TESLA_TPMS_DEBUG
    tesla_tpms_gap_event(event, param);
#endif
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
#if ENABLE_TESLA_TPMS_DEBUG
    if (tesla_tpms_gattc_event(event, gattc_if, param)) {
        return;
    }
#endif

    switch (event) {
        case ESP_GATTC_REG_EVT:
            if (param->reg.app_id != REMOTE_APP_ID) {
                break;
            }
            s_gattc_if = gattc_if;
            ESP_LOGI(GATTC_TAG, "GATTC app registered");
            ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&ble_scan_params));
            break;
        case ESP_GATTC_OPEN_EVT:
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGE(GATTC_TAG, "connect failed status=0x%x", param->open.status);
                s_is_connecting = false;
                start_ble_scan();
                break;
            }
            s_is_connecting = false;
            s_is_connected = true;
            s_conn_id = param->open.conn_id;
            s_hid_service_start = ESP_GATT_ILLEGAL_HANDLE;
            s_hid_service_end = ESP_GATT_ILLEGAL_HANDLE;
            s_hid_report_char_handle = ESP_GATT_ILLEGAL_HANDLE;
            s_bat_service_start = ESP_GATT_ILLEGAL_HANDLE;
            s_bat_service_end = ESP_GATT_ILLEGAL_HANDLE;
            s_bat_level_char_handle = ESP_GATT_ILLEGAL_HANDLE;
            s_remote_battery_percent = REMOTE_BAT_PCT_UNKNOWN;
            ESP_LOGI(GATTC_TAG, "Connected to %s", REMOTE_NAME);
            esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
            start_hid_service_discovery(gattc_if);
            if (tpms_manager_ble_scan_required()) {
                start_ble_scan();
            }
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                param->search_res.srvc_id.uuid.uuid.uuid16 == HID_SERVICE_UUID &&
                s_disc_phase == DISC_PHASE_HID) {
                s_hid_service_start = param->search_res.start_handle;
                s_hid_service_end = param->search_res.end_handle;
            } else if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                       param->search_res.srvc_id.uuid.uuid.uuid16 == BAT_SERVICE_UUID &&
                       s_disc_phase == DISC_PHASE_BAT) {
                s_bat_service_start = param->search_res.start_handle;
                s_bat_service_end = param->search_res.end_handle;
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (s_disc_phase == DISC_PHASE_HID) {
                finalize_hid_characteristics(gattc_if);
                s_disc_phase = DISC_PHASE_BAT;
                s_bat_service_start = ESP_GATT_ILLEGAL_HANDLE;
                s_bat_service_end = ESP_GATT_ILLEGAL_HANDLE;
                esp_err_t err = esp_ble_gattc_search_service(
                    gattc_if, s_conn_id, (esp_bt_uuid_t *)&s_bat_service_uuid);
                if (err != ESP_OK) {
                    ESP_LOGW(GATTC_TAG, "Battery service search failed: %s", esp_err_to_name(err));
                }
            } else if (s_disc_phase == DISC_PHASE_BAT) {
                try_discover_battery_and_read(gattc_if);
            }
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGE(GATTC_TAG, "register notify failed: 0x%x", param->reg_for_notify.status);
                break;
            }
            if (param->reg_for_notify.handle == s_hid_report_char_handle) {
                write_cccd_enable_notify(gattc_if, s_hid_report_char_handle, s_hid_service_start,
                                        s_hid_service_end);
            } else if (param->reg_for_notify.handle == s_bat_level_char_handle) {
                write_cccd_enable_notify(gattc_if, s_bat_level_char_handle, s_bat_service_start,
                                        s_bat_service_end);
            }
            break;
        case ESP_GATTC_READ_CHAR_EVT:
            if (param->read.status == ESP_GATT_OK && param->read.handle == s_bat_level_char_handle &&
                param->read.value_len > 0 && param->read.value != NULL) {
                store_battery_level_byte(param->read.value[0]);
            }
            break;
        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.handle == s_hid_report_char_handle) {
                Wireless_DecodeHidReport(param->notify.value, param->notify.value_len);
            } else if (param->notify.handle == s_bat_level_char_handle && param->notify.value_len > 0 &&
                       param->notify.value != NULL) {
                store_battery_level_byte(param->notify.value[0]);
            }
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            s_is_connected = false;
            s_is_connecting = false;
            s_disc_phase = DISC_PHASE_HID;
            s_hid_service_start = ESP_GATT_ILLEGAL_HANDLE;
            s_hid_service_end = ESP_GATT_ILLEGAL_HANDLE;
            s_hid_report_char_handle = ESP_GATT_ILLEGAL_HANDLE;
            s_bat_service_start = ESP_GATT_ILLEGAL_HANDLE;
            s_bat_service_end = ESP_GATT_ILLEGAL_HANDLE;
            s_bat_level_char_handle = ESP_GATT_ILLEGAL_HANDLE;
            s_remote_battery_percent = REMOTE_BAT_PCT_UNKNOWN;
            memset(s_remote_display_name, 0, sizeof(s_remote_display_name));
            memset(s_remote_bda, 0, sizeof(s_remote_bda));
            ESP_LOGW(GATTC_TAG, "Remote disconnected, restarting scan");
            start_ble_scan();
            if (!tpms_manager_is_scan_active() && !tpms_manager_is_rotation_active()) {
                tpms_manager_request_gateway_link();
            }
            break;
        default:
            break;
    }
}

bt_remote_conn_state_t Wireless_GetRemoteConnectionState(void)
{
    if (s_is_connected) {
        return BT_REMOTE_CONN_CONNECTED;
    }
    if (s_is_connecting) {
        return BT_REMOTE_CONN_CONNECTING;
    }
    return BT_REMOTE_CONN_DISCONNECTED;
}

void Wireless_GetRemoteDisplayName(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (!s_is_connected && !s_is_connecting) {
        return;
    }
    if (s_remote_display_name[0] != '\0') {
        strncpy(out, s_remote_display_name, out_len - 1U);
        out[out_len - 1U] = '\0';
    } else {
        strncpy(out, REMOTE_NAME, out_len - 1U);
        out[out_len - 1U] = '\0';
    }
}

bool Wireless_FormatRemoteMac(char *out, size_t out_len)
{
    if (out == NULL || out_len < 4U) {
        return false;
    }
    if (!s_is_connected && !s_is_connecting) {
        strncpy(out, "---", out_len - 1U);
        out[out_len - 1U] = '\0';
        return false;
    }
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                   s_remote_bda[0], s_remote_bda[1], s_remote_bda[2], s_remote_bda[3], s_remote_bda[4],
                   s_remote_bda[5]);
    return true;
}

bool Wireless_GetRemoteBatteryPercent(uint8_t *out_percent)
{
    if (out_percent == NULL || !s_is_connected) {
        return false;
    }
    if (s_remote_battery_percent > 100u) {
        return false;
    }
    *out_percent = s_remote_battery_percent;
    return true;
}