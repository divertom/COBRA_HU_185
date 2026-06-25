#include "tpms_manager.h"

#include "Storage_Manager.h"
#include "Wireless.h"
#include "tpms_ble_adv.h"
#include "tpms_parser.h"
#include "tesla_tpms_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "TPMS_MGR";
static const char *k_tpms_sensors_path = "/config/tpms_sensors.json";
static const char *k_device_config_path = "/config/device_config.json";

typedef struct {
    uint8_t bda[6];
    esp_ble_addr_type_t addr_type;
    char mac[18];
    char display_name[16];
    bool is_gateway;
    tpms_tire_position_t position;
    bool seen_since_boot;
    int64_t last_seen_ms;
    bool has_telemetry;
    tpms_telemetry_t telem;
    tpms_telemetry_state_t telemetry_state;
} tpms_sensor_slot_t;

static tpms_sensor_slot_t s_sensors[TPMS_MANAGER_MAX_SENSORS];
static int s_sensor_count = 0;
static bool s_scan_active = false;
static uint8_t s_primary_bda[6];
static bool s_primary_set = false;
static tpms_units_t s_units = { .temp_unit = "C", .pressure_unit = "kpa" };
static SemaphoreHandle_t s_lock = NULL;
static uint32_t s_gap_adv_seen = 0;
static uint32_t s_gap_adv_matched = 0;
static int64_t s_last_diag_ms = 0;
static TaskHandle_t s_gateway_work_task = NULL;
static TaskHandle_t s_rotation_task = NULL;

#define TPMS_GATEWAY_RETRY_MS 20000U
#define TPMS_GATEWAY_WORK_STACK 3072
#define TPMS_GATEWAY_WORK_PRIO  2
#define TPMS_ROTATION_STACK     3072
#define TPMS_ROTATION_PRIO      2
#define TPMS_SENSORS_JSON_MAX 4096U

/** Time on each tsTPMS module (connect + read + teardown). */
#define TPMS_ROTATE_SLOT_MS           15000U
/** Pause between modules: BLE scan for adv updates on other MACs. */
#define TPMS_ROTATE_GAP_MS            2000U
/** Abandon connect attempt if not connected by then. */
#define TPMS_ROTATE_CONNECT_TIMEOUT_MS 10000U

static bool s_rotation_active = false;
static int s_rotate_order[TPMS_MANAGER_MAX_SENSORS];
static int s_rotate_count = 0;
static int s_rotate_cursor = 0;
static int s_rotate_target_idx = -1;
static volatile bool s_rotate_got_read = false;
static int64_t s_rotate_slot_start_ms = 0;
static int64_t s_rotate_gap_until_ms = 0;

typedef enum {
    TPMS_ROT_GAP = 0,
    TPMS_ROT_SLOT,
} tpms_rotation_phase_t;

static tpms_rotation_phase_t s_rotate_phase = TPMS_ROT_GAP;

static int64_t tpms_now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

static void tpms_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void tpms_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

const char *tpms_position_to_string(tpms_tire_position_t pos)
{
    switch (pos) {
    case TPMS_TIRE_NO:
        return "NO";
    case TPMS_TIRE_LF:
        return "LF";
    case TPMS_TIRE_RF:
        return "RF";
    case TPMS_TIRE_LR:
        return "LR";
    case TPMS_TIRE_RR:
        return "RR";
    default:
        return "NO";
    }
}

bool tpms_position_from_string(const char *str, tpms_tire_position_t *out)
{
    if (str == NULL || out == NULL) {
        return false;
    }
    if (strcmp(str, "NO") == 0) {
        *out = TPMS_TIRE_NO;
        return true;
    }
    if (strcmp(str, "LF") == 0) {
        *out = TPMS_TIRE_LF;
        return true;
    }
    if (strcmp(str, "RF") == 0) {
        *out = TPMS_TIRE_RF;
        return true;
    }
    if (strcmp(str, "LR") == 0) {
        *out = TPMS_TIRE_LR;
        return true;
    }
    if (strcmp(str, "RR") == 0) {
        *out = TPMS_TIRE_RR;
        return true;
    }
    return false;
}

bool tpms_manager_format_mac(const uint8_t bda[6], char *out, size_t out_len)
{
    if (bda == NULL || out == NULL || out_len < 18) {
        return false;
    }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return true;
}

bool tpms_manager_parse_mac(const char *mac_str, uint8_t bda[6])
{
    if (mac_str == NULL || bda == NULL) {
        return false;
    }
    unsigned int v[6];
    int matched = sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (matched != 6) {
        matched = sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    }
    if (matched != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        bda[i] = (uint8_t)v[i];
    }
    return true;
}

static int tpms_find_slot_by_bda(const uint8_t bda[6])
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (memcmp(s_sensors[i].bda, bda, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static int tpms_find_slot_by_mac(const char *mac)
{
    uint8_t bda[6];
    if (!tpms_manager_parse_mac(mac, bda)) {
        return -1;
    }
    return tpms_find_slot_by_bda(bda);
}

static int tpms_find_slot_by_position(tpms_tire_position_t pos)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].position == pos) {
            return i;
        }
    }
    return -1;
}

static int tpms_find_primary_gateway_locked(void)
{
    if (s_primary_set) {
        int idx = tpms_find_slot_by_bda(s_primary_bda);
        if (idx >= 0 && s_sensors[idx].is_gateway) {
            return idx;
        }
    }

    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].is_gateway) {
            return i;
        }
    }
    return -1;
}

static void tpms_set_primary_bda_locked(const uint8_t bda[6])
{
    memcpy(s_primary_bda, bda, 6);
    s_primary_set = true;
}

static esp_err_t tpms_save_sensors_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < s_sensor_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "mac", s_sensors[i].mac);
        cJSON_AddNumberToObject(item, "addr_type", s_sensors[i].addr_type);
        cJSON_AddStringToObject(item, "position", tpms_position_to_string(s_sensors[i].position));
        if (s_sensors[i].is_gateway) {
            cJSON_AddStringToObject(item, "type", "gateway");
        }
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "sensors", arr);
    if (s_primary_set) {
        char primary_mac[18];
        tpms_manager_format_mac(s_primary_bda, primary_mac, sizeof(primary_mac));
        cJSON_AddStringToObject(root, "primary_gatt_mac", primary_mac);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t json_len = strlen(json);
    esp_err_t err = storage_write_file(k_tpms_sensors_path, json, json_len);
    cJSON_free(json);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved TPMS sensors (%d entries, %u bytes) to %s",
                 s_sensor_count, (unsigned)json_len, k_tpms_sensors_path);
    } else {
        ESP_LOGE(TAG, "Failed to save TPMS sensors to %s: %s",
                 k_tpms_sensors_path, esp_err_to_name(err));
    }
    return err;
}

static void tpms_load_sensors(void)
{
    if (!storage_file_exists(k_tpms_sensors_path)) {
        ESP_LOGI(TAG, "No TPMS sensors file on SPIFFS");
        return;
    }

    size_t file_size = 0;
    if (storage_get_file_size(k_tpms_sensors_path, &file_size) != ESP_OK || file_size == 0) {
        ESP_LOGW(TAG, "TPMS sensors file empty or unreadable");
        return;
    }
    if (file_size >= TPMS_SENSORS_JSON_MAX) {
        ESP_LOGW(TAG, "TPMS sensors file too large (%u bytes)", (unsigned)file_size);
        return;
    }

    char *buf = (char *)malloc(file_size + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "No memory to load TPMS sensors");
        return;
    }

    size_t bytes_read = 0;
    if (storage_read_file(k_tpms_sensors_path, buf, file_size, &bytes_read) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read TPMS sensors from SPIFFS");
        free(buf);
        return;
    }
    buf[bytes_read] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse TPMS sensors JSON");
        return;
    }

    cJSON *primary_item = cJSON_GetObjectItem(root, "primary_gatt_mac");
    if (cJSON_IsString(primary_item)) {
        uint8_t bda[6];
        if (tpms_manager_parse_mac(cJSON_GetStringValue(primary_item), bda)) {
            tpms_set_primary_bda_locked(bda);
        }
    }

    cJSON *arr = cJSON_GetObjectItem(root, "sensors");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "TPMS sensors JSON missing sensors array");
        cJSON_Delete(root);
        return;
    }

    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n && s_sensor_count < TPMS_MANAGER_MAX_SENSORS; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *mac_item = cJSON_GetObjectItem(item, "mac");
        cJSON *pos_item = cJSON_GetObjectItem(item, "position");
        if (!cJSON_IsString(mac_item)) {
            continue;
        }

        tpms_sensor_slot_t *slot = &s_sensors[s_sensor_count];
        const char *mac = cJSON_GetStringValue(mac_item);
        if (!tpms_manager_parse_mac(mac, slot->bda)) {
            continue;
        }
        tpms_manager_format_mac(slot->bda, slot->mac, sizeof(slot->mac));
        slot->addr_type = BLE_ADDR_TYPE_PUBLIC;
        cJSON *addr_type_item = cJSON_GetObjectItem(item, "addr_type");
        if (cJSON_IsNumber(addr_type_item)) {
            int at = (int)cJSON_GetNumberValue(addr_type_item);
            if (at >= 0 && at <= 1) {
                slot->addr_type = (esp_ble_addr_type_t)at;
            }
        }
        slot->position = TPMS_TIRE_NO;
        slot->is_gateway = false;
        slot->display_name[0] = '\0';
        if (cJSON_IsString(pos_item)) {
            tpms_position_from_string(cJSON_GetStringValue(pos_item), &slot->position);
        }
        cJSON *type_item = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(type_item) && strcmp(cJSON_GetStringValue(type_item), "gateway") == 0) {
            slot->is_gateway = true;
            strncpy(slot->display_name, "tsTPMS", sizeof(slot->display_name) - 1);
        }
        slot->seen_since_boot = false;
        slot->last_seen_ms = 0;
        slot->has_telemetry = false;
        slot->telemetry_state = TPMS_TELEMETRY_NONE;
        memset(&slot->telem, 0, sizeof(slot->telem));
        s_sensor_count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d TPMS sensors from SPIFFS", s_sensor_count);
}

static void tpms_load_units(void)
{
    if (!storage_file_exists(k_device_config_path)) {
        return;
    }

    char buf[1024];
    size_t bytes_read = 0;
    if (storage_read_file(k_device_config_path, buf, sizeof(buf) - 1, &bytes_read) != ESP_OK) {
        return;
    }
    buf[bytes_read] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return;
    }

    cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (settings != NULL) {
        cJSON *temp = cJSON_GetObjectItem(settings, "temp_unit");
        if (cJSON_IsString(temp)) {
            const char *v = cJSON_GetStringValue(temp);
            if (strcmp(v, "F") == 0 || strcmp(v, "f") == 0) {
                strncpy(s_units.temp_unit, "F", sizeof(s_units.temp_unit) - 1);
            } else {
                strncpy(s_units.temp_unit, "C", sizeof(s_units.temp_unit) - 1);
            }
        }
        cJSON *press = cJSON_GetObjectItem(settings, "pressure_unit");
        if (cJSON_IsString(press)) {
            const char *v = cJSON_GetStringValue(press);
            if (strcmp(v, "psi") == 0 || strcmp(v, "PSI") == 0) {
                strncpy(s_units.pressure_unit, "psi", sizeof(s_units.pressure_unit) - 1);
            } else {
                strncpy(s_units.pressure_unit, "kpa", sizeof(s_units.pressure_unit) - 1);
            }
        }
    }

    cJSON_Delete(root);
}

static esp_err_t tpms_save_units(void)
{
    char buf[1024];
    size_t bytes_read = 0;
    cJSON *root = NULL;

    if (storage_file_exists(k_device_config_path) &&
        storage_read_file(k_device_config_path, buf, sizeof(buf) - 1, &bytes_read) == ESP_OK) {
        buf[bytes_read] = '\0';
        root = cJSON_Parse(buf);
    }

    if (root == NULL) {
        root = cJSON_CreateObject();
        if (root == NULL) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(root, "device_name", "COBRA_HU_185");
        cJSON_AddStringToObject(root, "version", "1.0.0");
    }

    cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (settings == NULL) {
        settings = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "settings", settings);
    }

    cJSON_DeleteItemFromObject(settings, "temp_unit");
    cJSON_DeleteItemFromObject(settings, "pressure_unit");
    cJSON_AddStringToObject(settings, "temp_unit", s_units.temp_unit);
    cJSON_AddStringToObject(settings, "pressure_unit", s_units.pressure_unit);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_write_file(k_device_config_path, json, strlen(json));
    cJSON_free(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device settings to SPIFFS");
    }
    return err;
}

static tpms_sensor_status_t tpms_compute_status(const tpms_sensor_slot_t *slot, int64_t now_ms)
{
    if (!slot->seen_since_boot) {
        return TPMS_SENSOR_STATUS_STALE;
    }
    int64_t age = now_ms - slot->last_seen_ms;
    if (age < 0) {
        age = 0;
    }
    if (age <= (int64_t)TPMS_STATUS_LIVE_MS) {
        return TPMS_SENSOR_STATUS_LIVE;
    }
    if (age <= (int64_t)TPMS_STATUS_RECENT_MS) {
        return TPMS_SENSOR_STATUS_RECENT;
    }
    return TPMS_SENSOR_STATUS_STALE;
}

static float tpms_pressure_display(const tpms_telemetry_t *t)
{
    if (strcmp(s_units.pressure_unit, "psi") == 0) {
        return t->pressure_psi;
    }
    return (float)t->pressure_kpa;
}

static float tpms_temp_display(const tpms_telemetry_t *t)
{
    if (strcmp(s_units.temp_unit, "F") == 0) {
        return t->temp_f;
    }
    return (float)t->temp_c;
}

static int tpms_add_sensor_locked(const uint8_t bda[6])
{
    if (s_sensor_count >= TPMS_MANAGER_MAX_SENSORS) {
        return -1;
    }
    int existing = tpms_find_slot_by_bda(bda);
    if (existing >= 0) {
        return existing;
    }

    tpms_sensor_slot_t *slot = &s_sensors[s_sensor_count];
    memcpy(slot->bda, bda, 6);
    tpms_manager_format_mac(bda, slot->mac, sizeof(slot->mac));
    slot->addr_type = BLE_ADDR_TYPE_PUBLIC;
    slot->position = TPMS_TIRE_NO;
    slot->is_gateway = false;
    slot->display_name[0] = '\0';
    slot->seen_since_boot = false;
    slot->last_seen_ms = 0;
    slot->has_telemetry = false;
    slot->telemetry_state = TPMS_TELEMETRY_NONE;
    memset(&slot->telem, 0, sizeof(slot->telem));

    int idx = s_sensor_count;
    s_sensor_count++;
    esp_err_t err = tpms_save_sensors_locked();
    if (err != ESP_OK) {
        s_sensor_count--;
        memset(slot, 0, sizeof(*slot));
        return -1;
    }
    ESP_LOGI(TAG, "Added sensor %s", slot->mac);
    return idx;
}

static void tpms_apply_gateway_telemetry_locked(int idx, tpms_parse_result_t result,
                                                const tpms_telemetry_t *telem)
{
    if (idx < 0 || telem == NULL) {
        return;
    }

    tpms_sensor_slot_t *slot = &s_sensors[idx];
    int64_t now = tpms_now_ms();
    slot->seen_since_boot = true;
    slot->last_seen_ms = now;

    switch (result) {
    case TPMS_PARSE_STABLE_OK:
        slot->has_telemetry = true;
        slot->telem = *telem;
        slot->telemetry_state = TPMS_TELEMETRY_STABLE;
        break;
    case TPMS_PARSE_ACTIVE_OK:
        slot->has_telemetry = true;
        slot->telem = *telem;
        slot->telemetry_state = TPMS_TELEMETRY_ACTIVE;
        break;
    default:
        break;
    }
}

static void tpms_bootstrap_storage(void)
{
    if (!storage_file_exists(k_device_config_path)) {
        const char *default_cfg =
            "{\"device_name\":\"COBRA_HU_185\",\"version\":\"1.0.0\","
            "\"service_portal\":{\"ap_ssid\":\"Cobra HU Service\"},"
            "\"settings\":{\"temp_unit\":\"C\",\"pressure_unit\":\"kpa\"}}";
        if (storage_write_file(k_device_config_path, default_cfg, strlen(default_cfg)) == ESP_OK) {
            ESP_LOGI(TAG, "Created default device config on SPIFFS");
        }
    }
}

static bool tpms_has_gateway_locked(void)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].is_gateway) {
            return true;
        }
    }
    return false;
}

static bool tpms_gateway_link_blocked_internal(bool portal_scan_active)
{
    (void)portal_scan_active;
    /* Only block while SmartRemote is mid-connect; one TPMS GATT client may coexist when connected. */
    if (Wireless_GetRemoteConnectionState() == BT_REMOTE_CONN_CONNECTING) {
        return true;
    }
    return false;
}

static bool tpms_gateway_link_blocked(void)
{
    return tpms_gateway_link_blocked_internal(tpms_manager_is_scan_active());
}

static void tpms_rotation_notify(void)
{
    if (s_rotation_task != NULL) {
        xTaskNotifyGive(s_rotation_task);
    }
}

static int tpms_rotation_tire_sort_key(tpms_tire_position_t pos)
{
    switch (pos) {
    case TPMS_TIRE_LF:
        return 0;
    case TPMS_TIRE_RF:
        return 1;
    case TPMS_TIRE_LR:
        return 2;
    case TPMS_TIRE_RR:
        return 3;
    default:
        return 4;
    }
}

static void tpms_rotation_build_queue_locked(void)
{
    s_rotate_count = 0;
    for (int i = 0; i < s_sensor_count && s_rotate_count < TPMS_MANAGER_MAX_SENSORS; i++) {
        if (s_sensors[i].is_gateway) {
            s_rotate_order[s_rotate_count++] = i;
        }
    }

    for (int i = 0; i < s_rotate_count; i++) {
        for (int j = i + 1; j < s_rotate_count; j++) {
            const int idx_i = s_rotate_order[i];
            const int idx_j = s_rotate_order[j];
            int key_i = tpms_rotation_tire_sort_key(s_sensors[idx_i].position);
            int key_j = tpms_rotation_tire_sort_key(s_sensors[idx_j].position);
            if (s_primary_set && memcmp(s_sensors[idx_i].bda, s_primary_bda, 6) == 0) {
                key_i = -1;
            }
            if (s_primary_set && memcmp(s_sensors[idx_j].bda, s_primary_bda, 6) == 0) {
                key_j = -1;
            }
            if (key_j < key_i) {
                const int tmp = s_rotate_order[i];
                s_rotate_order[i] = s_rotate_order[j];
                s_rotate_order[j] = tmp;
            }
        }
    }
}

static void tpms_rotation_begin_slot_locked(void)
{
    if (s_rotate_count == 0) {
        return;
    }

    s_rotate_target_idx = s_rotate_order[s_rotate_cursor];
    s_rotate_got_read = false;
    s_rotate_slot_start_ms = tpms_now_ms();
    s_rotate_phase = TPMS_ROT_SLOT;

    const tpms_sensor_slot_t *slot = &s_sensors[s_rotate_target_idx];
    ESP_LOGI(TAG, "GATT rotation slot %d/%d: %s",
             s_rotate_cursor + 1, s_rotate_count, slot->mac);

    tesla_tpms_init();
    tesla_tpms_start();
    tesla_tpms_set_target(slot->bda, slot->addr_type);
    tesla_tpms_request_connect();
}

static void tpms_rotation_end_slot(void)
{
    int slot_idx = -1;
    char mac[18] = {0};

    tpms_lock();
    slot_idx = s_rotate_target_idx;
    if (slot_idx >= 0) {
        strncpy(mac, s_sensors[slot_idx].mac, sizeof(mac) - 1);
    }
    if (s_rotate_count > 0) {
        s_rotate_cursor = (s_rotate_cursor + 1) % s_rotate_count;
    }
    s_rotate_target_idx = -1;
    s_rotate_phase = TPMS_ROT_GAP;
    s_rotate_gap_until_ms = tpms_now_ms() + (int64_t)TPMS_ROTATE_GAP_MS;
    tpms_unlock();

    tesla_tpms_stop();
    Wireless_EnsureBleScanActive();

    if (mac[0] != '\0') {
        ESP_LOGI(TAG, "GATT rotation done with %s, gap %u ms",
                 mac, (unsigned)TPMS_ROTATE_GAP_MS);
    }
    tpms_rotation_notify();
}

static void tpms_rotation_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));

        if (!s_rotation_active || tpms_manager_is_scan_active()) {
            continue;
        }
        if (tpms_gateway_link_blocked_internal(false)) {
            continue;
        }

        tpms_lock();
        const int count = s_rotate_count;
        const tpms_rotation_phase_t phase = s_rotate_phase;
        const int64_t now = tpms_now_ms();
        const int64_t gap_until = s_rotate_gap_until_ms;
        const int64_t slot_start = s_rotate_slot_start_ms;
        const int target_idx = s_rotate_target_idx;
        const bool got_read = s_rotate_got_read;
        tpms_unlock();

        if (count == 0) {
            continue;
        }

        if (phase == TPMS_ROT_GAP) {
            if (now < gap_until) {
                continue;
            }
            tpms_lock();
            tpms_rotation_begin_slot_locked();
            tpms_unlock();
            continue;
        }

        if (phase != TPMS_ROT_SLOT || target_idx < 0) {
            continue;
        }

        const int64_t slot_age = now - slot_start;
        const bool connected = tesla_tpms_is_connected();
        const bool connecting = tesla_tpms_is_connecting();

        if (got_read) {
            tpms_rotation_end_slot();
            continue;
        }

        if (!connecting && !connected && slot_age >= 500) {
            tesla_tpms_request_connect();
        }

        if (connected && slot_age >= (int64_t)TPMS_ROTATE_SLOT_MS) {
            ESP_LOGW(TAG, "GATT rotation slot timeout (connected, no pressure yet)");
            tpms_rotation_end_slot();
            continue;
        }

        if (!connected && slot_age >= (int64_t)TPMS_ROTATE_CONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "GATT rotation slot timeout (connect failed)");
            tpms_rotation_end_slot();
            continue;
        }

        if (slot_age >= (int64_t)TPMS_ROTATE_SLOT_MS) {
            ESP_LOGW(TAG, "GATT rotation slot timeout");
            tpms_rotation_end_slot();
        }
    }
}

static void tpms_rotation_start_locked(void)
{
    tpms_rotation_build_queue_locked();
    if (s_rotate_count == 0) {
        ESP_LOGI(TAG, "GATT rotation: no tsTPMS modules to poll");
        s_rotation_active = false;
        return;
    }

    s_rotation_active = true;
    s_rotate_cursor = 0;
    s_rotate_target_idx = -1;
    s_rotate_got_read = false;
    s_rotate_phase = TPMS_ROT_GAP;
    s_rotate_gap_until_ms = 0;
    tesla_tpms_stop();
    ESP_LOGI(TAG, "GATT rotation started (%d tsTPMS modules, %u ms per slot)",
             s_rotate_count, (unsigned)TPMS_ROTATE_SLOT_MS);
    tpms_rotation_notify();
}

static void tpms_rotation_stop_locked(void)
{
    s_rotation_active = false;
    s_rotate_target_idx = -1;
    s_rotate_count = 0;
    s_rotate_phase = TPMS_ROT_GAP;
    tesla_tpms_stop();
}

static void tpms_start_rotation_task(void)
{
    if (s_rotation_task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        tpms_rotation_task, "tpms_rot", TPMS_ROTATION_STACK, NULL,
        TPMS_ROTATION_PRIO, &s_rotation_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TPMS rotation task");
        s_rotation_task = NULL;
    }
}

void tpms_manager_start_rotation(void)
{
    tpms_start_rotation_task();
    tpms_lock();
    tpms_rotation_start_locked();
    tpms_unlock();
}

void tpms_manager_stop_rotation(void)
{
    tpms_lock();
    tpms_rotation_stop_locked();
    tpms_unlock();
    tpms_rotation_notify();
}

bool tpms_manager_is_rotation_active(void)
{
    bool active;
    tpms_lock();
    active = s_rotation_active;
    tpms_unlock();
    return active;
}

static void tpms_gateway_work_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(TPMS_GATEWAY_RETRY_MS);

    for (;;) {
        (void)ulTaskNotifyTake(pdFALSE, period);

        if (tpms_manager_is_scan_active() || tpms_manager_is_rotation_active()) {
            continue;
        }
        if (tesla_tpms_is_connected()) {
            continue;
        }

        tpms_lock();
        const bool has_gateway = tpms_has_gateway_locked();
        tpms_unlock();
        if (!has_gateway) {
            continue;
        }

        tpms_manager_start_rotation();
    }
}

static void tpms_start_gateway_work_task(void)
{
    if (s_gateway_work_task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        tpms_gateway_work_task, "tpms_gw", TPMS_GATEWAY_WORK_STACK, NULL,
        TPMS_GATEWAY_WORK_PRIO, &s_gateway_work_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TPMS gateway work task");
        s_gateway_work_task = NULL;
    }
}

void tpms_manager_request_gateway_link(void)
{
    tpms_start_gateway_work_task();
    if (s_gateway_work_task != NULL) {
        xTaskNotifyGive(s_gateway_work_task);
    }
}

void tpms_manager_on_gatt_disconnect(void)
{
    if (tpms_manager_is_scan_active()) {
        return;
    }

    bool end_slot = false;
    tpms_lock();
    if (s_rotation_active && s_rotate_phase == TPMS_ROT_SLOT) {
        end_slot = true;
    }
    tpms_unlock();

    if (end_slot) {
        tpms_rotation_end_slot();
        return;
    }

    if (tpms_manager_is_rotation_active()) {
        tpms_rotation_notify();
        return;
    }

    tpms_manager_request_gateway_link();
}

static void tpms_ensure_gateway_link_locked(void)
{
    if (s_scan_active) {
        return;
    }
    if (tpms_gateway_link_blocked_internal(s_scan_active)) {
        ESP_LOGI(TAG, "Gateway GATT deferred (SmartRemote connecting)");
        return;
    }
    if (tesla_tpms_is_connecting()) {
        return;
    }

    int idx = tpms_find_primary_gateway_locked();
    if (idx < 0) {
        return;
    }

    if (tesla_tpms_is_connected() && tesla_tpms_target_matches(s_sensors[idx].bda)) {
        return;
    }

    tesla_tpms_init();
    tesla_tpms_start();
    tesla_tpms_set_target(s_sensors[idx].bda, s_sensors[idx].addr_type);
    tesla_tpms_request_connect();
    ESP_LOGI(TAG, "Gateway GATT link requested for %s", s_sensors[idx].mac);
}

void tpms_manager_ensure_gateway_link(void)
{
    tpms_lock();
    tpms_ensure_gateway_link_locked();
    tpms_unlock();
}

void tpms_manager_on_gateway_telemetry(const uint8_t bda[6], tpms_parse_result_t result,
                                         const tpms_telemetry_t *telem)
{
    if (bda == NULL) {
        return;
    }

    tpms_lock();
    int idx = tpms_find_slot_by_bda(bda);
    if (idx < 0) {
        tpms_unlock();
        return;
    }

    if (result == TPMS_PARSE_STABLE_OK || result == TPMS_PARSE_ACTIVE_OK) {
        tpms_apply_gateway_telemetry_locked(idx, result, telem);
        if (s_rotation_active && idx == s_rotate_target_idx) {
            s_rotate_got_read = true;
            tpms_rotation_notify();
        }
    } else if (result == TPMS_PARSE_STATUS_REST || result == TPMS_PARSE_STARTUP_INFO) {
        s_sensors[idx].seen_since_boot = true;
        s_sensors[idx].last_seen_ms = tpms_now_ms();
    }
    tpms_unlock();
}

static void tpms_update_sensor_from_adv(int idx, const uint8_t *payload, size_t payload_len)
{
    tpms_sensor_slot_t *slot = &s_sensors[idx];
    int64_t now = tpms_now_ms();

    slot->seen_since_boot = true;
    slot->last_seen_ms = now;

    tpms_telemetry_t telem;
    tpms_parse_result_t result = tpms_parser_parse(payload, payload_len, &telem);

    switch (result) {
    case TPMS_PARSE_STABLE_OK:
        slot->has_telemetry = true;
        slot->telem = telem;
        slot->telemetry_state = TPMS_TELEMETRY_STABLE;
        break;
    case TPMS_PARSE_ACTIVE_OK:
        slot->has_telemetry = true;
        slot->telem = telem;
        slot->telemetry_state = TPMS_TELEMETRY_ACTIVE;
        break;
    case TPMS_PARSE_STATUS_REST:
    case TPMS_PARSE_STARTUP_INFO:
        break;
    default:
        break;
    }
}

esp_err_t tpms_manager_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_sensor_count = 0;
    s_scan_active = false;
    s_primary_set = false;
    memset(s_primary_bda, 0, sizeof(s_primary_bda));

    tpms_bootstrap_storage();

    tpms_lock();
    tpms_load_units();
    tpms_load_sensors();
    const int loaded = s_sensor_count;
    const bool has_gateway = tpms_has_gateway_locked();
    const bool has_primary = s_primary_set;
    tpms_unlock();

    if (has_gateway) {
        tpms_manager_start_rotation();
    }

    if (loaded > 0) {
        Wireless_EnsureBleScanActive();
    }

    if (loaded > 0) {
        storage_log_all_files();
    }

    ESP_LOGI(TAG, "TPMS manager ready (%d sensors persisted, gateway=%d, primary=%d)",
             loaded, (int)has_gateway, (int)has_primary);
    return ESP_OK;
}

void tpms_manager_on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT || param == NULL) {
        return;
    }
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        return;
    }

    bool scan_active = false;
    tpms_lock();
    scan_active = s_scan_active;
    tpms_unlock();

    s_gap_adv_seen++;

    int idx = -1;
    tpms_lock();
    idx = tpms_find_slot_by_bda(param->scan_rst.bda);
    tpms_unlock();

    bool is_gateway_adv = tpms_ble_adv_is_gateway(param);
    bool is_candidate = tpms_ble_adv_is_discovery_candidate(param);

    if (!is_candidate && idx < 0) {
        int64_t now = tpms_now_ms();
        if (scan_active && now - s_last_diag_ms >= 10000) {
            s_last_diag_ms = now;
            ESP_LOGI(TAG, "TPMS scan: %u adv seen, %u matched (still listening)",
                     (unsigned)s_gap_adv_seen, (unsigned)s_gap_adv_matched);
        }
        return;
    }

    if (idx < 0 && !scan_active) {
        return;
    }

    s_gap_adv_matched++;

    const uint8_t *adv = (const uint8_t *)param->scan_rst.ble_adv;
    uint8_t payload[64];
    size_t payload_len = 0;
    bool has_payload = tpms_ble_adv_try_parse_telemetry(
        adv, param->scan_rst.adv_data_len,
        adv + param->scan_rst.adv_data_len, param->scan_rst.scan_rsp_len,
        payload, sizeof(payload), &payload_len);

    bool need_gateway_link = false;

    tpms_lock();

    if (idx < 0 && scan_active) {
        idx = tpms_add_sensor_locked(param->scan_rst.bda);
        if (idx >= 0) {
            s_sensors[idx].addr_type = param->scan_rst.ble_addr_type;
            if (is_gateway_adv) {
                s_sensors[idx].is_gateway = true;
                strncpy(s_sensors[idx].display_name, "tsTPMS", sizeof(s_sensors[idx].display_name) - 1);
                if (!s_primary_set) {
                    tpms_set_primary_bda_locked(s_sensors[idx].bda);
                }
                (void)tpms_save_sensors_locked();
                ESP_LOGI(TAG, "Discovered gateway tsTPMS %s rssi=%d (GATT deferred until scan stops)",
                         s_sensors[idx].mac, param->scan_rst.rssi);
            } else {
                ESP_LOGI(TAG, "Discovered sensor %s rssi=%d",
                         s_sensors[idx].mac, param->scan_rst.rssi);
            }
        }
    } else if (idx >= 0 && is_gateway_adv && !s_sensors[idx].is_gateway) {
        s_sensors[idx].is_gateway = true;
        strncpy(s_sensors[idx].display_name, "tsTPMS", sizeof(s_sensors[idx].display_name) - 1);
        if (!s_primary_set) {
            tpms_set_primary_bda_locked(s_sensors[idx].bda);
        }
        (void)tpms_save_sensors_locked();
        if (!scan_active) {
            need_gateway_link = true;
        } else {
            ESP_LOGI(TAG, "Marked gateway tsTPMS %s (GATT deferred until scan stops)",
                     s_sensors[idx].mac);
        }
    }

    if (idx >= 0) {
        if (has_payload) {
            tpms_update_sensor_from_adv(idx, payload, payload_len);
        } else {
            s_sensors[idx].seen_since_boot = true;
            s_sensors[idx].last_seen_ms = tpms_now_ms();
        }
    }

    if (need_gateway_link && !s_rotation_active) {
        tpms_rotation_start_locked();
    }

    tpms_unlock();
}

bool tpms_manager_is_scan_active(void)
{
    bool active;
    tpms_lock();
    active = s_scan_active;
    tpms_unlock();
    return active;
}

bool tpms_manager_ble_scan_required(void)
{
    bool need;
    tpms_lock();
    need = s_scan_active || s_sensor_count > 0;
    tpms_unlock();
    return need;
}

esp_err_t tpms_manager_set_scan_active(bool active)
{
    tpms_lock();
    s_scan_active = active;
    tpms_unlock();

    if (active) {
        tpms_lock();
        tpms_rotation_stop_locked();
        tpms_unlock();

        s_gap_adv_seen = 0;
        s_gap_adv_matched = 0;
        s_last_diag_ms = tpms_now_ms();
        tesla_tpms_stop();
        Wireless_RestartBleScanForTpms();
        ESP_LOGI(TAG, "TPMS discovery scan started");
    } else {
        ESP_LOGI(TAG, "TPMS discovery scan stopped");
        tesla_tpms_stop();
        tpms_manager_start_rotation();
    }
    return ESP_OK;
}

esp_err_t tpms_manager_forget(const char *mac)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tpms_lock();
    if (s_scan_active) {
        tpms_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    int idx = tpms_find_slot_by_mac(mac);
    if (idx < 0) {
        tpms_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    bool was_gateway = s_sensors[idx].is_gateway;
    const bool was_primary = s_primary_set && memcmp(s_primary_bda, s_sensors[idx].bda, 6) == 0;

    tpms_sensor_slot_t backup[TPMS_MANAGER_MAX_SENSORS];
    const int backup_count = s_sensor_count;
    memcpy(backup, s_sensors, sizeof(backup));

    for (int i = idx; i < s_sensor_count - 1; i++) {
        s_sensors[i] = s_sensors[i + 1];
    }
    s_sensor_count--;

    if (was_primary) {
        s_primary_set = false;
        memset(s_primary_bda, 0, sizeof(s_primary_bda));
        for (int i = 0; i < s_sensor_count; i++) {
            if (s_sensors[i].is_gateway) {
                tpms_set_primary_bda_locked(s_sensors[i].bda);
                break;
            }
        }
    }

    esp_err_t err = tpms_save_sensors_locked();
    if (err != ESP_OK) {
        s_sensor_count = backup_count;
        memcpy(s_sensors, backup, sizeof(backup));
        if (was_primary) {
            tpms_set_primary_bda_locked(backup[idx].bda);
        }
    }
    tpms_unlock();

    if (was_gateway) {
        tesla_tpms_stop();
    }

    ESP_LOGI(TAG, "Forgot sensor %s", mac);
    return err;
}

esp_err_t tpms_manager_set_position(const char *mac, tpms_tire_position_t position)
{
    if (mac == NULL || position >= TPMS_TIRE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    tpms_lock();
    int idx = tpms_find_slot_by_mac(mac);
    if (idx < 0) {
        tpms_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    tpms_tire_position_t old_pos = s_sensors[idx].position;
    tpms_tire_position_t old_other_pos = TPMS_TIRE_NO;
    int other = -1;

    if (position != TPMS_TIRE_NO) {
        other = tpms_find_slot_by_position(position);
        if (other >= 0 && other != idx) {
            old_other_pos = s_sensors[other].position;
            s_sensors[other].position = TPMS_TIRE_NO;
        }
    }

    s_sensors[idx].position = position;
    esp_err_t err = tpms_save_sensors_locked();
    if (err != ESP_OK) {
        s_sensors[idx].position = old_pos;
        if (other >= 0 && other != idx) {
            s_sensors[other].position = old_other_pos;
        }
    }
    tpms_unlock();
    return err;
}

esp_err_t tpms_manager_set_primary_gatt(const char *mac)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tpms_lock();
    if (s_scan_active) {
        tpms_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    int idx = tpms_find_slot_by_mac(mac);
    if (idx < 0) {
        tpms_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_sensors[idx].is_gateway) {
        tpms_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    tpms_set_primary_bda_locked(s_sensors[idx].bda);
    esp_err_t err = tpms_save_sensors_locked();
    tpms_unlock();

    if (err == ESP_OK) {
        tesla_tpms_stop();
        tpms_manager_start_rotation();
    }
    return err;
}

esp_err_t tpms_manager_get_primary_gatt_mac(char *mac_out, size_t mac_out_len)
{
    if (mac_out == NULL || mac_out_len < 18) {
        return ESP_ERR_INVALID_ARG;
    }

    tpms_lock();
    if (!s_primary_set) {
        tpms_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    tpms_manager_format_mac(s_primary_bda, mac_out, mac_out_len);
    tpms_unlock();
    return ESP_OK;
}

esp_err_t tpms_manager_get_units(tpms_units_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    tpms_lock();
    *out = s_units;
    tpms_unlock();
    return ESP_OK;
}

esp_err_t tpms_manager_set_units(const tpms_units_t *units)
{
    if (units == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tpms_lock();
    strncpy(s_units.temp_unit, units->temp_unit, sizeof(s_units.temp_unit) - 1);
    s_units.temp_unit[sizeof(s_units.temp_unit) - 1] = '\0';
    strncpy(s_units.pressure_unit, units->pressure_unit, sizeof(s_units.pressure_unit) - 1);
    s_units.pressure_unit[sizeof(s_units.pressure_unit) - 1] = '\0';
    esp_err_t err = tpms_save_units();
    tpms_unlock();
    return err;
}

int tpms_manager_sensor_count(void)
{
    int count;
    tpms_lock();
    count = s_sensor_count;
    tpms_unlock();
    return count;
}

esp_err_t tpms_manager_get_snapshot(int index, tpms_sensor_snapshot_t *out)
{
    if (out == NULL || index < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    tpms_lock();
    if (index >= s_sensor_count) {
        tpms_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    const tpms_sensor_slot_t *slot = &s_sensors[index];
    int64_t now = tpms_now_ms();

    memset(out, 0, sizeof(*out));
    strncpy(out->mac, slot->mac, sizeof(out->mac) - 1);
    strncpy(out->display_name, slot->display_name, sizeof(out->display_name) - 1);
    out->is_gateway = slot->is_gateway;
    if (slot->is_gateway) {
        int primary_idx = tpms_find_primary_gateway_locked();
        out->is_primary_gatt = (primary_idx == index);
    } else {
        out->is_primary_gatt = false;
    }
    out->is_gatt_active = s_rotation_active && (s_rotate_target_idx == index);
    out->position = slot->position;
    out->status = tpms_compute_status(slot, now);
    out->telemetry_state = slot->has_telemetry ? slot->telemetry_state : TPMS_TELEMETRY_NONE;
    out->has_telemetry = slot->has_telemetry;
    if (slot->has_telemetry) {
        out->pressure = tpms_pressure_display(&slot->telem);
        out->temperature = tpms_temp_display(&slot->telem);
    }
    out->units = s_units;

    tpms_unlock();
    return ESP_OK;
}
