#include "Wireless.h"

#include <stdlib.h>

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
#define CCCD_UUID 0x2902
#define MAX_BONDED_DEVICES 8

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
static esp_bd_addr_t s_bonded_bda[MAX_BONDED_DEVICES];
static uint16_t s_bonded_count = 0;

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

void Wireless_Init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    xTaskCreatePinnedToCore(
        WIFI_Init,
        "WIFI task",
        4096,
        NULL,
        1,
        NULL,
        0);

    xTaskCreatePinnedToCore(
        BLE_Init,
        "BLE task",
        4096,
        NULL,
        2,
        NULL,
        0);
}

void WIFI_Init(void *arg)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    WIFI_NUM = WIFI_Scan();
    printf("WIFI:%d\r\n", WIFI_NUM);

    vTaskDelete(NULL);
}

uint16_t WIFI_Scan(void)
{
    uint16_t ap_count = 0;
    esp_wifi_scan_start(NULL, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    esp_wifi_scan_stop();
    WiFi_Scan_Finish = 1;
    if (BLE_Scan_Finish == 1 || WiFi_Scan_Finish == 1) {
        Scan_finish = 1;
    }
    return ap_count;
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

static void start_ble_scan(void)
{
    if (s_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(GATTC_TAG, "GATTC not ready yet");
        return;
    }
    if (s_is_connected || s_is_connecting) {
        return;
    }
    esp_err_t err = esp_ble_gap_start_scanning(0);
    if (err != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "start scanning failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(GATTC_TAG, "BLE scanning started for %s", REMOTE_NAME);
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
            start_ble_scan();
            break;
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(GATTC_TAG, "scan start failed, status=%d", param->scan_start_cmpl.status);
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                BLE_NUM++;
                bool name_match = match_target_remote_name(param);
                bool bond_match = is_bonded_device(param->scan_rst.bda);
                if (!s_is_connecting && !s_is_connected && (name_match || bond_match)) {
                    BLE_Scan_Finish = 1;
                    if (WiFi_Scan_Finish == 1) {
                        Scan_finish = 1;
                    }
                    memcpy(s_remote_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
                    s_remote_addr_type = param->scan_rst.ble_addr_type;
                    s_is_connecting = true;
                    if (name_match) {
                        ESP_LOGI(GATTC_TAG, "Found %s by name, connecting...", REMOTE_NAME);
                    } else {
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
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTC_REG_EVT:
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
            ESP_LOGI(GATTC_TAG, "Connected to %s", REMOTE_NAME);
            esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                param->search_res.srvc_id.uuid.uuid.uuid16 == HID_SERVICE_UUID) {
                s_hid_service_start = param->search_res.start_handle;
                s_hid_service_end = param->search_res.end_handle;
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT: {
            if (s_hid_service_start == ESP_GATT_ILLEGAL_HANDLE) {
                ESP_LOGW(GATTC_TAG, "HID service not found (remote may be Classic-only)");
                esp_ble_gattc_close(gattc_if, s_conn_id);
                break;
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
                break;
            }

            esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)calloc(count, sizeof(esp_gattc_char_elem_t));
            if (chars == NULL) {
                ESP_LOGE(GATTC_TAG, "No memory for char discovery");
                esp_ble_gattc_close(gattc_if, s_conn_id);
                break;
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
            break;
        }
        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGE(GATTC_TAG, "register notify failed: 0x%x", param->reg_for_notify.status);
                break;
            }
            uint16_t count = 0;
            esp_bt_uuid_t cccd_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid = {.uuid16 = CCCD_UUID}
            };

            esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                gattc_if,
                s_conn_id,
                ESP_GATT_DB_DESCRIPTOR,
                s_hid_service_start,
                s_hid_service_end,
                s_hid_report_char_handle,
                &count);
            if (status != ESP_GATT_OK || count == 0) {
                ESP_LOGE(GATTC_TAG, "CCCD count failed");
                break;
            }

            esp_gattc_descr_elem_t *descr = (esp_gattc_descr_elem_t *)calloc(count, sizeof(esp_gattc_descr_elem_t));
            if (descr == NULL) {
                ESP_LOGE(GATTC_TAG, "No memory for descriptors");
                break;
            }
            status = esp_ble_gattc_get_descr_by_char_handle(
                gattc_if,
                s_conn_id,
                s_hid_report_char_handle,
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
                ESP_LOGI(GATTC_TAG, "Notification enabled");
            } else {
                ESP_LOGE(GATTC_TAG, "CCCD descriptor not found");
            }
            free(descr);
            break;
        }
        case ESP_GATTC_NOTIFY_EVT:
            Wireless_DecodeHidReport(param->notify.value, param->notify.value_len);
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            s_is_connected = false;
            s_is_connecting = false;
            s_hid_service_start = ESP_GATT_ILLEGAL_HANDLE;
            s_hid_service_end = ESP_GATT_ILLEGAL_HANDLE;
            s_hid_report_char_handle = ESP_GATT_ILLEGAL_HANDLE;
            ESP_LOGW(GATTC_TAG, "Remote disconnected, restarting scan");
            start_ble_scan();
            break;
        default:
            break;
    }
}