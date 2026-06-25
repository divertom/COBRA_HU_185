#include "tesla_tpms_ble.h"

#include "Wireless.h"
#include "tpms_manager.h"
#include "tpms_parser.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_gatt_common_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TPMS_LOG_TAG "TPMS"

#define TPMS_APP_ID 1
#define TPMS_DEVICE_NAME "tsTPMS"

#define TPMS_TASK_STACK 4096
#define TPMS_TASK_PRIO 1

#if ENABLE_TESLA_TPMS_DEBUG

static const uint8_t s_tpms_service_uuid128[ESP_UUID_LEN_128] = {
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00,
};
static const uint8_t s_tpms_indicate_uuid128[ESP_UUID_LEN_128] = {
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x13, 0x02, 0x00, 0x00,
};
static const uint8_t s_tpms_version_uuid128[ESP_UUID_LEN_128] = {
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x14, 0x02, 0x00, 0x00,
};

typedef enum {
    TPMS_DISC_IDLE = 0,
    TPMS_DISC_SERVICE,
    TPMS_DISC_CHARS,
} tpms_disc_phase_t;

static esp_gatt_if_t s_tpms_gattc_if = ESP_GATT_IF_NONE;
static TaskHandle_t s_tpms_task = NULL;
static bool s_tpms_running = false;
static bool s_tpms_gattc_ready = false;
static bool s_tpms_connecting = false;
static bool s_tpms_connected = false;
static bool s_tpms_scan_requested = false;
static uint16_t s_tpms_conn_id = 0;
static esp_bd_addr_t s_tpms_bda = {0};
static esp_ble_addr_type_t s_tpms_addr_type = BLE_ADDR_TYPE_PUBLIC;
static tpms_disc_phase_t s_tpms_disc_phase = TPMS_DISC_IDLE;
static uint16_t s_tpms_service_start = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_tpms_service_end = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_tpms_indicate_handle = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_tpms_version_handle = ESP_GATT_ILLEGAL_HANDLE;
static uint16_t s_tpms_cccd_handle = ESP_GATT_ILLEGAL_HANDLE;
static bool s_tpms_indications_active = false;
static bool s_tpms_mtu_done = false;
static bool s_tpms_connect_pending = false;
static bool s_tpms_scan_listener_logged = false;
static bool s_tpms_gattc_wait_logged = false;
static uint8_t s_tpms_cccd_retry_count = 0;
static bool s_tpms_scan_restart_pending = false;
static bool s_tpms_init_done = false;
static int64_t s_tpms_last_connect_fail_ms = 0;

#define TPMS_CONNECT_BACKOFF_MS 30000

static esp_bt_uuid_t s_tpms_service_uuid;
static esp_bt_uuid_t s_tpms_indicate_uuid;
static esp_bt_uuid_t s_tpms_version_uuid;

static const uint8_t s_tpms_cccd_indicate_on[2] = {0x02, 0x00};

static void tpms_log(const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ESP_LOGI(TPMS_LOG_TAG, "%s", buf);
}

static void tpms_reset_connection_state(void)
{
    s_tpms_connecting = false;
    s_tpms_connected = false;
    s_tpms_conn_id = 0;
    s_tpms_disc_phase = TPMS_DISC_IDLE;
    s_tpms_service_start = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_service_end = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_indicate_handle = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_version_handle = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_cccd_handle = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_indications_active = false;
    s_tpms_mtu_done = false;
    s_tpms_connect_pending = false;
    s_tpms_scan_listener_logged = false;
    s_tpms_gattc_wait_logged = false;
    s_tpms_cccd_retry_count = 0;
    memset(s_tpms_bda, 0, sizeof(s_tpms_bda));
}

static void tpms_format_addr(char *out, size_t out_len, const esp_bd_addr_t bda)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void tpms_format_hex(char *out, size_t out_len, const uint8_t *data, size_t len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (data == NULL || len == 0) {
        out[0] = '\0';
        return;
    }

    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (pos + 3 >= out_len) {
            break;
        }
        if (i > 0) {
            out[pos++] = ' ';
        }
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X", data[i]);
    }
    out[pos] = '\0';
}

static bool tpms_uuid128_equals(const esp_bt_uuid_t *uuid, const uint8_t *target)
{
    if (uuid == NULL || uuid->len != ESP_UUID_LEN_128) {
        return false;
    }
    return memcmp(uuid->uuid.uuid128, target, ESP_UUID_LEN_128) == 0;
}

static bool tpms_adv_blob_has_service_uuid128(const uint8_t *adv, uint8_t adv_len)
{
    if (adv == NULL || adv_len == 0) {
        return false;
    }

    uint8_t uuid_len = 0;
    uint8_t *uuid_data = esp_ble_resolve_adv_data((uint8_t *)adv, ESP_BLE_AD_TYPE_128SRV_CMPL, &uuid_len);
    if (uuid_data == NULL || uuid_len < ESP_UUID_LEN_128) {
        uuid_data = esp_ble_resolve_adv_data((uint8_t *)adv, ESP_BLE_AD_TYPE_128SRV_PART, &uuid_len);
    }
    if (uuid_data == NULL || uuid_len < ESP_UUID_LEN_128) {
        return false;
    }

    for (uint8_t offset = 0; offset + ESP_UUID_LEN_128 <= uuid_len; offset += ESP_UUID_LEN_128) {
        if (memcmp(uuid_data + offset, s_tpms_service_uuid128, ESP_UUID_LEN_128) == 0) {
            return true;
        }
    }
    return false;
}

static bool tpms_name_bytes_match(const uint8_t *name, uint8_t name_len)
{
    if (name == NULL || name_len == 0) {
        return false;
    }

    size_t want_len = strlen(TPMS_DEVICE_NAME);
    if ((size_t)name_len < want_len) {
        return false;
    }

    for (size_t i = 0; i < want_len; i++) {
        char a = (char)name[i];
        char b = TPMS_DEVICE_NAME[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool tpms_remote_blocks_gatt(void);
static bool tpms_connect_backoff_active(void);
static void tpms_request_scan(void);

static bool tpms_scan_result_matches(const esp_ble_gap_cb_param_t *param)
{
    const uint8_t total_len = (uint8_t)(param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len);
    if (total_len == 0) {
        return false;
    }

    uint8_t *adv = (uint8_t *)param->scan_rst.ble_adv;
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data_by_type(adv, total_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (name == NULL || name_len == 0) {
        name = esp_ble_resolve_adv_data_by_type(adv, total_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (tpms_name_bytes_match(name, name_len)) {
        return true;
    }

    return tpms_adv_blob_has_service_uuid128(adv, total_len);
}

static void tpms_resume_discovery_scan_if_needed(void)
{
    if (!tpms_manager_is_scan_active()) {
        return;
    }
    s_tpms_connect_pending = false;
    Wireless_RestartBleScanForTpms();
    tpms_log("[TPMS] portal discovery scan resumed");
}

static void tpms_try_begin_connect(void)
{
    if (tpms_manager_is_scan_active()) {
        return;
    }
    if (!s_tpms_connect_pending || s_tpms_connecting || s_tpms_connected) {
        return;
    }
    if (s_tpms_gattc_if == ESP_GATT_IF_NONE) {
        return;
    }
    if (tpms_remote_blocks_gatt()) {
        return;
    }
    if (tpms_connect_backoff_active()) {
        return;
    }

    char addr[24];
    tpms_format_addr(addr, sizeof(addr), s_tpms_bda);
    s_tpms_connect_pending = false;
    s_tpms_connecting = true;
    s_tpms_scan_requested = false;
    tpms_log("[TPMS] connecting addr=%s", addr);

    esp_err_t err = esp_ble_gap_stop_scanning();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_tpms_scan_restart_pending = true;
    } else {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] stop scan failed: %s", esp_err_to_name(err));
        s_tpms_connecting = false;
        s_tpms_connect_pending = true;
    }
}

static bool tpms_remote_blocks_gatt(void)
{
    return Wireless_GetRemoteConnectionState() == BT_REMOTE_CONN_CONNECTING;
}

static bool tpms_connect_backoff_active(void)
{
    if (s_tpms_last_connect_fail_ms == 0) {
        return false;
    }
    int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);
    return (now_ms - s_tpms_last_connect_fail_ms) < TPMS_CONNECT_BACKOFF_MS;
}

static void tpms_restart_shared_scan(void)
{
    if (tpms_manager_is_scan_active()) {
        tpms_resume_discovery_scan_if_needed();
        return;
    }

    if (!s_tpms_running || s_tpms_connected || s_tpms_connecting) {
        return;
    }
    if (tpms_remote_blocks_gatt() || tpms_connect_backoff_active()) {
        return;
    }

    bool after_tpms_stop = s_tpms_scan_restart_pending;
    s_tpms_scan_restart_pending = false;

    Wireless_EnsureBleScanActive();

    if (after_tpms_stop) {
        tpms_log("[TPMS] scan resumed");
    } else {
        tpms_request_scan();
    }
}

static void tpms_request_scan(void)
{
    if (!s_tpms_running || s_tpms_gattc_if == ESP_GATT_IF_NONE) {
        return;
    }
    if (s_tpms_connected || s_tpms_connecting) {
        return;
    }
    if (tpms_remote_blocks_gatt()) {
        return;
    }

    /* Piggyback on SmartRemote scan when it is already running. */
    if (!s_tpms_scan_listener_logged) {
        s_tpms_scan_listener_logged = true;
        tpms_log("[TPMS] listening on shared BLE scan for %s", TPMS_DEVICE_NAME);
    }
}

static esp_gatt_status_t tpms_write_cccd_indicate(esp_gatt_if_t gattc_if, uint16_t char_handle,
                                                uint16_t svc_start, uint16_t svc_end)
{
    esp_bt_uuid_t cccd_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG},
    };

    uint16_t count = 0;
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
        gattc_if, s_tpms_conn_id, ESP_GATT_DB_DESCRIPTOR, svc_start, svc_end, char_handle, &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] CCCD lookup failed status=0x%x count=%u", status, (unsigned)count);
        return status;
    }

    esp_gattc_descr_elem_t *descr = calloc(count, sizeof(esp_gattc_descr_elem_t));
    if (descr == NULL) {
        return ESP_GATT_NO_RESOURCES;
    }

    status = esp_ble_gattc_get_descr_by_char_handle(
        gattc_if, s_tpms_conn_id, char_handle, cccd_uuid, descr, &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] CCCD descriptor not found status=0x%x", status);
        free(descr);
        return status;
    }

    s_tpms_cccd_handle = descr[0].handle;
    status = esp_ble_gattc_write_char_descr(
        gattc_if, s_tpms_conn_id, s_tpms_cccd_handle,
        sizeof(s_tpms_cccd_indicate_on), (uint8_t *)s_tpms_cccd_indicate_on,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (status != ESP_GATT_OK) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] CCCD write request failed status=0x%x", status);
    } else {
        tpms_log("[TPMS] enabling indications on 00000213 (CCCD=0200 handle=0x%04x)",
                 s_tpms_cccd_handle);
    }

    free(descr);
    return status;
}

static void tpms_register_indicate_char(esp_gatt_if_t gattc_if)
{
    if (s_tpms_indicate_handle == ESP_GATT_ILLEGAL_HANDLE) {
        return;
    }

    esp_err_t err = esp_ble_gattc_register_for_notify(gattc_if, s_tpms_bda, s_tpms_indicate_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] register_for_notify failed: %s", esp_err_to_name(err));
    } else {
        tpms_log("[TPMS] register_for_notify on 00000213 handle=0x%04x", s_tpms_indicate_handle);
    }
}

static void tpms_discover_indicate_char(esp_gatt_if_t gattc_if)
{
    uint16_t count = 0;
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
        gattc_if, s_tpms_conn_id, ESP_GATT_DB_CHARACTERISTIC,
        s_tpms_service_start, s_tpms_service_end, ESP_GATT_ILLEGAL_HANDLE, &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] no characteristics in TPMS service");
        return;
    }

    esp_gattc_char_elem_t *chars = calloc(count, sizeof(esp_gattc_char_elem_t));
    if (chars == NULL) {
        return;
    }

    status = esp_ble_gattc_get_char_by_uuid(
        gattc_if, s_tpms_conn_id, s_tpms_service_start, s_tpms_service_end,
        s_tpms_indicate_uuid, chars, &count);
    if (status == ESP_GATT_OK && count > 0) {
        if ((chars[0].properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) == 0) {
            ESP_LOGW(TPMS_LOG_TAG, "[TPMS] char 0213 props=0x%02x (expected INDICATE)",
                     chars[0].properties);
        }
        s_tpms_indicate_handle = chars[0].char_handle;
        tpms_log("[TPMS] indication characteristic found 00000213-b2d1-43f0-9b88-960cebf8b91e handle=0x%04x",
                 s_tpms_indicate_handle);
        tpms_register_indicate_char(gattc_if);
    } else {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] indication characteristic not found status=0x%x", status);
    }

    count = 0;
    status = esp_ble_gattc_get_attr_count(
        gattc_if, s_tpms_conn_id, ESP_GATT_DB_CHARACTERISTIC,
        s_tpms_service_start, s_tpms_service_end, ESP_GATT_ILLEGAL_HANDLE, &count);
    if (status == ESP_GATT_OK && count > 0) {
        esp_gattc_char_elem_t *version_chars = calloc(count, sizeof(esp_gattc_char_elem_t));
        if (version_chars != NULL) {
            uint16_t version_count = count;
            status = esp_ble_gattc_get_char_by_uuid(
                gattc_if, s_tpms_conn_id, s_tpms_service_start, s_tpms_service_end,
                s_tpms_version_uuid, version_chars, &version_count);
            if (status == ESP_GATT_OK && version_count > 0) {
                s_tpms_version_handle = version_chars[0].char_handle;
                esp_ble_gattc_read_char(gattc_if, s_tpms_conn_id, s_tpms_version_handle,
                                        ESP_GATT_AUTH_REQ_NONE);
            }
            free(version_chars);
        }
    }

    free(chars);
}

static void tpms_start_service_discovery(esp_gatt_if_t gattc_if)
{
    s_tpms_disc_phase = TPMS_DISC_SERVICE;
    s_tpms_service_start = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_service_end = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_indicate_handle = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_version_handle = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_cccd_handle = ESP_GATT_ILLEGAL_HANDLE;
    s_tpms_indications_active = false;
    s_tpms_cccd_retry_count = 0;

    esp_err_t err = esp_ble_gattc_search_service(gattc_if, s_tpms_conn_id, &s_tpms_service_uuid);
    if (err != ESP_OK) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] service search failed: %s", esp_err_to_name(err));
        esp_ble_gattc_close(gattc_if, s_tpms_conn_id);
    }
}

static void tpms_on_connected(esp_gatt_if_t gattc_if)
{
    s_tpms_mtu_done = false;
    esp_err_t err = esp_ble_gattc_send_mtu_req(gattc_if, s_tpms_conn_id);
    if (err != ESP_OK) {
        ESP_LOGW(TPMS_LOG_TAG, "[TPMS] MTU request failed: %s, discovering anyway", esp_err_to_name(err));
        tpms_start_service_discovery(gattc_if);
    }
}

static void tpms_handle_scan_result(esp_ble_gap_cb_param_t *param)
{
    (void)param;
    /* GATT connect is driven only by tpms_manager via set_target/request_connect. */
}

static void tpms_task(void *arg)
{
    (void)arg;

    while (s_tpms_running) {
        if (s_tpms_gattc_ready && s_tpms_connect_pending && !s_tpms_connected && !s_tpms_connecting) {
            tpms_try_begin_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_tpms_task = NULL;
    vTaskDelete(NULL);
}

void tesla_tpms_init(void)
{
    if (s_tpms_init_done) {
        return;
    }
    s_tpms_init_done = true;

    s_tpms_service_uuid.len = ESP_UUID_LEN_128;
    memcpy(s_tpms_service_uuid.uuid.uuid128, s_tpms_service_uuid128, ESP_UUID_LEN_128);
    s_tpms_indicate_uuid.len = ESP_UUID_LEN_128;
    memcpy(s_tpms_indicate_uuid.uuid.uuid128, s_tpms_indicate_uuid128, ESP_UUID_LEN_128);
    s_tpms_version_uuid.len = ESP_UUID_LEN_128;
    memcpy(s_tpms_version_uuid.uuid.uuid128, s_tpms_version_uuid128, ESP_UUID_LEN_128);

    esp_err_t err = esp_ble_gattc_app_register(TPMS_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TPMS_LOG_TAG, "[TPMS] gattc app register failed: %s", esp_err_to_name(err));
    }
}

void tesla_tpms_set_target(const uint8_t bda[6], esp_ble_addr_type_t addr_type)
{
    if (bda == NULL) {
        return;
    }
    memcpy(s_tpms_bda, bda, sizeof(esp_bd_addr_t));
    s_tpms_addr_type = addr_type;
    s_tpms_connect_pending = true;
    tpms_log("[TPMS] target set addr=%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

void tesla_tpms_request_connect(void)
{
    if (tpms_manager_is_scan_active()) {
        return;
    }
    if (!s_tpms_running || !s_tpms_connect_pending) {
        return;
    }
    tpms_try_begin_connect();
}

void tesla_tpms_start(void)
{
    if (tpms_manager_is_scan_active()) {
        return;
    }
    if (s_tpms_running) {
        return;
    }
    s_tpms_running = true;
    /* Do not reset gattc_if — registration may already be in flight. */
    s_tpms_connecting = false;
    s_tpms_connected = false;
    tpms_log("[TPMS] GATT client started (target name=%s)", TPMS_DEVICE_NAME);

    if (s_tpms_task == NULL) {
        xTaskCreatePinnedToCore(tpms_task, "tpms_ble", TPMS_TASK_STACK, NULL, TPMS_TASK_PRIO, &s_tpms_task, 0);
    }

    tpms_request_scan();
    Wireless_EnsureBleScanActive();

    if (s_tpms_gattc_if != ESP_GATT_IF_NONE && s_tpms_connect_pending) {
        tpms_try_begin_connect();
    }
}

void tesla_tpms_stop(void)
{
    s_tpms_running = false;
    s_tpms_scan_requested = false;

    if (s_tpms_connected || s_tpms_connecting) {
        if (s_tpms_gattc_if != ESP_GATT_IF_NONE && s_tpms_conn_id != 0) {
            esp_ble_gattc_close(s_tpms_gattc_if, s_tpms_conn_id);
        }
    }
    tpms_reset_connection_state();
}

bool tesla_tpms_is_running(void)
{
    return s_tpms_running;
}

bool tesla_tpms_is_connected(void)
{
    return s_tpms_connected;
}

bool tesla_tpms_is_connecting(void)
{
    return s_tpms_connecting;
}

bool tesla_tpms_target_matches(const uint8_t bda[6])
{
    if (bda == NULL) {
        return false;
    }
    return memcmp(s_tpms_bda, bda, sizeof(esp_bd_addr_t)) == 0;
}

void tesla_tpms_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (!s_tpms_running || param == NULL) {
        return;
    }

    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            tpms_handle_scan_result(param);
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            if (s_tpms_connecting && s_tpms_gattc_if != ESP_GATT_IF_NONE) {
                esp_err_t err = esp_ble_gattc_open(s_tpms_gattc_if, s_tpms_bda, s_tpms_addr_type, true);
                if (err != ESP_OK) {
                    ESP_LOGW(TPMS_LOG_TAG, "[TPMS] gattc open failed: %s", esp_err_to_name(err));
                    s_tpms_connecting = false;
                    s_tpms_last_connect_fail_ms = (int64_t)(esp_timer_get_time() / 1000);
                    if (tpms_manager_is_scan_active()) {
                        tpms_resume_discovery_scan_if_needed();
                    } else {
                        tpms_restart_shared_scan();
                    }
                }
            } else if (s_tpms_scan_requested) {
                s_tpms_scan_requested = false;
            }
            break;
        default:
            break;
    }
}

bool tesla_tpms_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param)
{
    if (param == NULL) {
        return false;
    }

    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id != TPMS_APP_ID) {
            return false;
        }
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TPMS_LOG_TAG, "[TPMS] gattc register failed status=0x%x", param->reg.status);
            return true;
        }
        s_tpms_gattc_if = gattc_if;
        s_tpms_gattc_ready = true;
        s_tpms_gattc_wait_logged = false;
        tpms_log("[TPMS] gattc registered if=%d", (int)gattc_if);
        tpms_try_begin_connect();
        return true;
    }

    if (!s_tpms_running || gattc_if != s_tpms_gattc_if) {
        return false;
    }

    switch (event) {
        case ESP_GATTC_OPEN_EVT:
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGW(TPMS_LOG_TAG, "[TPMS] connect failed status=0x%x", param->open.status);
                s_tpms_connecting = false;
                s_tpms_last_connect_fail_ms = (int64_t)(esp_timer_get_time() / 1000);
                if (tpms_manager_is_scan_active()) {
                    tpms_resume_discovery_scan_if_needed();
                } else {
                    tpms_restart_shared_scan();
                }
                break;
            }
            s_tpms_last_connect_fail_ms = 0;
            s_tpms_connecting = false;
            s_tpms_connected = true;
            s_tpms_conn_id = param->open.conn_id;
            tpms_log("[TPMS] connected");
            tpms_on_connected(gattc_if);
            break;
        case ESP_GATTC_CFG_MTU_EVT:
            if (param->cfg_mtu.status == ESP_GATT_OK) {
                tpms_log("[TPMS] MTU=%u", (unsigned)param->cfg_mtu.mtu);
            } else {
                ESP_LOGW(TPMS_LOG_TAG, "[TPMS] MTU exchange failed status=0x%x", param->cfg_mtu.status);
            }
            if (!s_tpms_mtu_done) {
                s_tpms_mtu_done = true;
                tpms_start_service_discovery(gattc_if);
            }
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            if (s_tpms_disc_phase == TPMS_DISC_SERVICE &&
                tpms_uuid128_equals(&param->search_res.srvc_id.uuid, s_tpms_service_uuid128)) {
                s_tpms_service_start = param->search_res.start_handle;
                s_tpms_service_end = param->search_res.end_handle;
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (s_tpms_disc_phase == TPMS_DISC_SERVICE) {
                if (param->search_cmpl.status != ESP_GATT_OK) {
                    ESP_LOGW(TPMS_LOG_TAG, "[TPMS] service search failed status=0x%x",
                             param->search_cmpl.status);
                    esp_ble_gattc_close(gattc_if, s_tpms_conn_id);
                    break;
                }
                if (s_tpms_service_start != ESP_GATT_ILLEGAL_HANDLE) {
                    tpms_log("[TPMS] service found 00000211-b2d1-43f0-9b88-960cebf8b91e");
                    s_tpms_disc_phase = TPMS_DISC_CHARS;
                    tpms_discover_indicate_char(gattc_if);
                } else {
                    ESP_LOGW(TPMS_LOG_TAG, "[TPMS] service not found");
                    esp_ble_gattc_close(gattc_if, s_tpms_conn_id);
                }
            }
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.handle != s_tpms_indicate_handle) {
                break;
            }
            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGW(TPMS_LOG_TAG, "[TPMS] register_for_notify failed status=0x%x",
                         param->reg_for_notify.status);
                break;
            }
            tpms_log("[TPMS] register_for_notify ok for 00000213");
            if (tpms_write_cccd_indicate(gattc_if, s_tpms_indicate_handle,
                                         s_tpms_service_start, s_tpms_service_end) != ESP_GATT_OK &&
                s_tpms_cccd_retry_count < 2) {
                s_tpms_cccd_retry_count++;
                tpms_register_indicate_char(gattc_if);
            }
            break;
        case ESP_GATTC_WRITE_DESCR_EVT:
            if (param->write.handle != s_tpms_cccd_handle) {
                break;
            }
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGW(TPMS_LOG_TAG, "[TPMS] CCCD write failed status=0x%x", param->write.status);
                if (s_tpms_cccd_retry_count < 2) {
                    s_tpms_cccd_retry_count++;
                    (void)tpms_write_cccd_indicate(gattc_if, s_tpms_indicate_handle,
                                                   s_tpms_service_start, s_tpms_service_end);
                }
                break;
            }
            s_tpms_indications_active = true;
            tpms_log("[TPMS] indications enabled on 00000213-b2d1-43f0-9b88-960cebf8b91e");
            break;
        case ESP_GATTC_READ_CHAR_EVT:
            if (param->read.status == ESP_GATT_OK && param->read.handle == s_tpms_version_handle &&
                param->read.value != NULL && param->read.value_len > 0) {
                char hex[64];
                tpms_format_hex(hex, sizeof(hex), param->read.value, param->read.value_len);
                tpms_log("[TPMS] version raw: %s", hex);
            }
            break;
        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.handle == s_tpms_indicate_handle && param->notify.value != NULL) {
                if (param->notify.is_notify) {
                    ESP_LOGW(TPMS_LOG_TAG, "[TPMS] unexpected notify on 0213 (expected indicate)");
                } else if (!s_tpms_indications_active) {
                    ESP_LOGW(TPMS_LOG_TAG, "[TPMS] indication before CCCD active");
                }
                char hex[128];
                tpms_format_hex(hex, sizeof(hex), param->notify.value, param->notify.value_len);

                tpms_telemetry_t telem;
                tpms_parse_result_t result = tpms_parser_parse(
                    param->notify.value, param->notify.value_len, &telem);

                tpms_manager_on_gateway_telemetry(s_tpms_bda, result, &telem);

                switch (result) {
                case TPMS_PARSE_STABLE_OK:
                    tpms_log("[TPMS] STABLE pressure=%" PRIu32 " kPa temp=%" PRId32 " C",
                             telem.pressure_kpa, telem.temp_c);
                    break;
                case TPMS_PARSE_ACTIVE_OK:
                    tpms_log("[TPMS] ACTIVE pressure=%" PRIu32 " kPa temp=%" PRId32 " C",
                             telem.pressure_kpa, telem.temp_c);
                    break;
                case TPMS_PARSE_STATUS_REST:
                    tpms_log("[TPMS] STATUS/REST");
                    break;
                case TPMS_PARSE_STARTUP_INFO:
                    tpms_log("[TPMS] STARTUP/INFO");
                    break;
                case TPMS_PARSE_TELEMETRY_REJECTED:
                    tpms_log("[TPMS] TELEMETRY rejected");
                    break;
                case TPMS_PARSE_UNKNOWN:
                default:
                    tpms_log("[TPMS] UNKNOWN len=%u", (unsigned)param->notify.value_len);
                    break;
                }
            }
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            ESP_LOGW(TPMS_LOG_TAG, "[TPMS] disconnected reason=0x%x", param->disconnect.reason);
            tpms_reset_connection_state();
            tpms_restart_shared_scan();
            if (!tpms_manager_is_scan_active()) {
                tpms_manager_on_gatt_disconnect();
            }
            break;
        default:
            break;
    }

    return true;
}

#else /* ENABLE_TESLA_TPMS_DEBUG */

void tesla_tpms_init(void) {}
void tesla_tpms_set_target(const uint8_t bda[6], esp_ble_addr_type_t addr_type)
{
    (void)bda;
    (void)addr_type;
}
void tesla_tpms_request_connect(void) {}
void tesla_tpms_start(void) {}
void tesla_tpms_stop(void) {}
bool tesla_tpms_is_running(void) { return false; }
bool tesla_tpms_is_connected(void) { return false; }
bool tesla_tpms_is_connecting(void) { return false; }
bool tesla_tpms_target_matches(const uint8_t bda[6])
{
    (void)bda;
    return false;
}
void tesla_tpms_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    (void)event;
    (void)param;
}
bool tesla_tpms_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param)
{
    (void)event;
    (void)gattc_if;
    (void)param;
    return false;
}

#endif /* ENABLE_TESLA_TPMS_DEBUG */
