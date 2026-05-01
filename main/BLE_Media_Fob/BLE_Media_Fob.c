#include "BLE_Media_Fob.h"
#include "Storage_Manager.h"
#include "cJSON.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "BLE_Media_Fob";

/**
 * @brief Case-insensitive string comparison
 */
static int strcasecmp_custom(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int diff = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (diff != 0) {
            return diff;
        }
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

// BLE GATT Client parameters
#define PROFILE_NUM 1
#define PROFILE_APP_IDX 0
#define INVALID_HANDLE 0
#define GATTC_TAG "BLE_Media_Fob"

// Service UUIDs
static esp_bt_uuid_t hid_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = 0x1812}
};

// Serial Service UUID: 0000ffe0-0000-1000-8000-00805f9b34fb
static esp_bt_uuid_t serial_service_uuid = {
    .len = ESP_UUID_LEN_128,
    .uuid = {.uuid128 = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                         0x00, 0x10, 0x00, 0x00, 0xe0, 0xff, 0x00, 0x00}}
};

// Characteristic UUIDs (for reference)
// Report Characteristic: 0x2A4D
// HID Control Point: 0x2A4C
// Serial Characteristic: 0000ffe1-0000-1000-8000-00805f9b34fb
// Report Reference Descriptor: 0x2908
// CCCD Descriptor: 0x2902

// Maximum number of pollable characteristics
#define MAX_POLLING_CHARS 10

// Timing constants
#define POLL_INTERVAL_MS 200  // Increased from 100ms to reduce GATT queue pressure
#define SCAN_INTERVAL_MS 5000
#define SCAN_DURATION_SEC 2

// Connection state - matches BluetoothManager specification
static struct {
    uint16_t conn_id;
    esp_bd_addr_t remote_bda;
    
    // Service handles
    uint16_t hid_service_handle;
    uint16_t hid_service_end_handle;
    uint16_t serial_service_handle;
    uint16_t serial_service_end_handle;
    
    // Characteristic handles
    uint16_t report_char_handle;  // Primary report characteristic (first one found)
    uint16_t serial_char_handle;
    uint16_t hid_control_point_handle;
    uint16_t cccd_handle;
    
    // All report characteristics (UUID 0x2A4D) - matching working code
    uint16_t report_char_handles[MAX_POLLING_CHARS];  // All report characteristics found
    uint8_t report_char_count;  // Number of report characteristics discovered
    
    // Polling characteristics (read-capable)
    uint16_t polling_char_handles[MAX_POLLING_CHARS];
    uint8_t polling_char_count;
    uint16_t primary_polling_char_handle; // Main polling characteristic
    
    // Characteristic discovery state
    bool char_discovery_in_progress;  // True when discovering characteristics
    
    // State flags
    bool connected;
    bool service_discovered;
    bool char_discovered;
    bool do_connect;        // Flag to initiate connection
    bool do_scan;           // Flag to start scanning
    bool scanning;
    bool pairing_complete;
    bool pairing_in_progress;
    bool connection_attempt_in_progress;  // True when connection attempt is active
    
    // Button state
    ble_button_t last_button;
    ble_button_callback_t button_callback;
    
    // Target device info
    uint8_t target_mac[6];
    bool mac_address_set;
    char target_device_name[64];
    bool device_name_set;
    char connected_device_name[64];
    
    // Timestamps (in ticks)
    uint32_t last_scan_time;
    uint32_t last_poll_time;
    uint32_t pairing_start_time;  // When pairing was initiated
    uint32_t connection_attempt_start_time;  // When connection attempt was initiated
    
    // Statistics
    uint32_t button_count;
    uint32_t notify_count;
    
    // Polling state
    bool read_in_progress;  // Flag to prevent overlapping reads
    uint8_t current_poll_index;  // Current index in polling list
} gattc_state = {
    .conn_id = 0xFFFF,
    .hid_service_handle = INVALID_HANDLE,
    .hid_service_end_handle = INVALID_HANDLE,
    .serial_service_handle = INVALID_HANDLE,
    .serial_service_end_handle = INVALID_HANDLE,
    .report_char_handle = INVALID_HANDLE,
    .serial_char_handle = INVALID_HANDLE,
    .hid_control_point_handle = INVALID_HANDLE,
    .cccd_handle = INVALID_HANDLE,
    .report_char_count = 0,
    .polling_char_count = 0,
    .primary_polling_char_handle = INVALID_HANDLE,
    .char_discovery_in_progress = false,
    .connected = false,
    .service_discovered = false,
    .char_discovered = false,
    .do_connect = false,
    .do_scan = true,
    .scanning = false,
    .pairing_complete = false,
    .pairing_in_progress = false,
    .connection_attempt_in_progress = false,
    .last_button = BLE_BUTTON_NONE,
    .button_callback = NULL,
    .mac_address_set = false,
    .target_device_name = {0},
    .device_name_set = false,
    .connected_device_name = {0},
    .last_scan_time = 0,
    .last_poll_time = 0,
    .pairing_start_time = 0,
    .connection_attempt_start_time = 0,
    .button_count = 0,
    .notify_count = 0,
    .read_in_progress = false,
    .current_poll_index = 0
};

static esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;
static uint32_t reconnect_delay = 0;

// Forward declarations
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                esp_gatt_if_t gattc_if_param,
                                esp_ble_gattc_cb_param_t *param);
static void add_to_polling_list(uint16_t handle);

/**
 * @brief Parse MAC address string to bytes
 * Format: "FF:FF:40:00:10:26" or "FF-FF-40-00-10:26"
 */
static bool parse_mac_address(const char *mac_str, uint8_t *mac_bytes) {
    if (!mac_str || strlen(mac_str) < 17) {
        return false;
    }
    
    int values[6];
    int count = sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    
    if (count != 6) {
        // Try with dashes
        count = sscanf(mac_str, "%02x-%02x-%02x-%02x-%02x-%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    }
    
    if (count == 6) {
        for (int i = 0; i < 6; i++) {
            mac_bytes[i] = (uint8_t)values[i];
        }
        return true;
    }
    
    return false;
}

/**
 * @brief Read BLE MAC address from config file
 */
static esp_err_t read_ble_mac_from_config(void) {
    ESP_LOGI(TAG, "[CONFIG] Reading config file: /config/device_config.json");
    
    // Check if config file exists first
    if (!storage_file_exists("/config/device_config.json")) {
        ESP_LOGW(TAG, "[CONFIG] Config file does not exist, creating default...");
        
        // Create default config file
        const char *default_config = 
            "{\n"
            "  \"device_name\": \"COBRA_HU_185\",\n"
            "  \"version\": \"1.0.0\",\n"
            "  \"settings\": {\n"
            "    \"ble_mac_address\": \"FF:FF:40:00:10:26\"\n"
            "  }\n"
            "}\n";
        
        esp_err_t write_ret = storage_write_file("/config/device_config.json", 
                                                   default_config, strlen(default_config));
        if (write_ret != ESP_OK) {
            ESP_LOGE(TAG, "[CONFIG] ✗ Failed to create default config file: %s", esp_err_to_name(write_ret));
            ESP_LOGE(TAG, "[CONFIG] Please ensure SPIFFS is properly initialized and has write access");
            return write_ret;
        }
        ESP_LOGI(TAG, "[CONFIG] ✓ Created default config file");
    }
    
    char config_buffer[1024];
    size_t bytes_read = 0;
    
    // Storage_Manager prepends /storage, so /config becomes /storage/config
    esp_err_t ret = storage_read_file("/config/device_config.json", 
                                       config_buffer, sizeof(config_buffer) - 1, 
                                       &bytes_read);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[CONFIG] ✗ Failed to read config file: %s", esp_err_to_name(ret));
        return ret;
    }
    
    config_buffer[bytes_read] = '\0';
    ESP_LOGI(TAG, "[CONFIG] ✓ Config file read successfully (%d bytes)", bytes_read);
    
    // Parse JSON
    cJSON *json = cJSON_Parse(config_buffer);
    if (!json) {
        ESP_LOGE(TAG, "[CONFIG] ✗ Failed to parse config JSON");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[CONFIG] ✓ JSON parsed successfully");
    
    // Look for ble_device_name or ble_mac_address in settings
    cJSON *settings = cJSON_GetObjectItem(json, "settings");
    if (!settings) {
        ESP_LOGE(TAG, "[CONFIG] ✗ settings object not found in config");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    // Try device name first (preferred)
    cJSON *ble_device_name = cJSON_GetObjectItem(settings, "ble_device_name");
    if (ble_device_name && cJSON_IsString(ble_device_name)) {
        const char *device_name = cJSON_GetStringValue(ble_device_name);
        strncpy(gattc_state.target_device_name, device_name, sizeof(gattc_state.target_device_name) - 1);
        gattc_state.target_device_name[sizeof(gattc_state.target_device_name) - 1] = '\0';
        gattc_state.device_name_set = true;
        ESP_LOGI(TAG, "[CONFIG] ✓✓✓ BLE device name from config: %s ✓✓✓", gattc_state.target_device_name);
        cJSON_Delete(json);
        return ESP_OK;
    }
    
    // Fallback to MAC address
    cJSON *ble_mac = cJSON_GetObjectItem(settings, "ble_mac_address");
    if (ble_mac && cJSON_IsString(ble_mac)) {
        const char *mac_str = cJSON_GetStringValue(ble_mac);
        ESP_LOGI(TAG, "[CONFIG] MAC address string from config: %s", mac_str);
        
        if (parse_mac_address(mac_str, gattc_state.target_mac)) {
            gattc_state.mac_address_set = true;
            ESP_LOGI(TAG, "[CONFIG] ✓✓✓ BLE MAC address parsed successfully: %02X:%02X:%02X:%02X:%02X:%02X ✓✓✓",
                     gattc_state.target_mac[0], gattc_state.target_mac[1],
                     gattc_state.target_mac[2], gattc_state.target_mac[3],
                     gattc_state.target_mac[4], gattc_state.target_mac[5]);
            cJSON_Delete(json);
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "[CONFIG] ✗ Invalid MAC address format: %s", mac_str);
        }
    } else {
        ESP_LOGW(TAG, "[CONFIG] ✗ Neither ble_device_name nor ble_mac_address found in settings");
    }
    
    cJSON_Delete(json);
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Extract device name from BLE advertisement data
 */
static bool extract_device_name(const uint8_t *adv_data, uint8_t adv_data_len, char *device_name, size_t max_name_len) {
    if (!adv_data || !device_name || max_name_len == 0) {
        return false;
    }
    
    uint8_t offset = 0;
    while (offset < adv_data_len - 1) {
        uint8_t length = adv_data[offset];
        if (length == 0 || offset + length >= adv_data_len) {
            break;
        }
        
        uint8_t type = adv_data[offset + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT) {
            uint8_t name_len = length - 1;
            if (name_len > max_name_len - 1) {
                name_len = max_name_len - 1;
            }
            memcpy(device_name, &adv_data[offset + 2], name_len);
            device_name[name_len] = '\0';
            return true;
        }
        
        offset += length + 1;
    }
    return false;
}

/**
 * @brief BLE GAP callback for scanning
 */
static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== ESP_GAP_BLE_SCAN_RESULT_EVT =====");
                ESP_LOGI(GATTC_TAG, "[BLE_LOG] bda=%02X:%02X:%02X:%02X:%02X:%02X",
                         param->scan_rst.bda[0], param->scan_rst.bda[1],
                         param->scan_rst.bda[2], param->scan_rst.bda[3],
                         param->scan_rst.bda[4], param->scan_rst.bda[5]);
                ESP_LOGI(GATTC_TAG, "[BLE_LOG] rssi=%d, adv_data_len=%d", 
                         param->scan_rst.rssi, param->scan_rst.adv_data_len);
                char device_name[64];
                if (extract_device_name(param->scan_rst.ble_adv, param->scan_rst.adv_data_len, 
                                       device_name, sizeof(device_name))) {
                    ESP_LOGI(GATTC_TAG, "[SCAN] Found device: %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X, RSSI: %d)",
                             device_name,
                             param->scan_rst.bda[0], param->scan_rst.bda[1],
                             param->scan_rst.bda[2], param->scan_rst.bda[3],
                             param->scan_rst.bda[4], param->scan_rst.bda[5],
                             param->scan_rst.rssi);
                    
                    // Check if this is our target device (case-insensitive, partial match)
                    if (gattc_state.device_name_set) {
                        // Try exact match first
                        bool match = (strcasecmp_custom(device_name, gattc_state.target_device_name) == 0);
                        // Also try partial match (device name contains target or vice versa)
                        if (!match) {
                            match = (strstr(device_name, gattc_state.target_device_name) != NULL) ||
                                    (strstr(gattc_state.target_device_name, device_name) != NULL);
                        }
                        
                        if (match) {
                            ESP_LOGI(GATTC_TAG, "[SCAN] ✓✓✓ Found target device: %s (looking for: %s) ✓✓✓", 
                                    device_name, gattc_state.target_device_name);
                            memcpy(gattc_state.target_mac, param->scan_rst.bda, 6);
                            gattc_state.mac_address_set = true;
                            gattc_state.scanning = false;
                            gattc_state.do_scan = false;
                            gattc_state.do_connect = true; // Set flag to connect
                            
                            // Stop scanning
                            esp_ble_gap_stop_scanning();
                            
                            ESP_LOGI(GATTC_TAG, "[SCAN] Device found, will connect in update() loop");
                        }
                    }
                }
            }
            break;
        }
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(GATTC_TAG, "[SCAN] Scan stopped");
            gattc_state.scanning = false;
            if (!gattc_state.mac_address_set && gattc_state.device_name_set) {
                ESP_LOGW(GATTC_TAG, "[SCAN] Target device '%s' not found in this scan, will retry in 15 seconds...", 
                        gattc_state.target_device_name);
                ESP_LOGI(GATTC_TAG, "[SCAN] Make sure the device is powered on and advertising");
                // Will retry scanning in update function
            }
            break;
        default:
            break;
    }
}

/**
 * @brief BLE GAP security callback for pairing/bonding
 */
static void gap_security_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SEC_REQ_EVT:
            ESP_LOGI(GATTC_TAG, "[SECURITY] Security request received");
            // Accept pairing request
            esp_ble_gap_security_rsp(gattc_state.remote_bda, true);
            gattc_state.pairing_in_progress = true;
            ESP_LOGI(GATTC_TAG, "[SECURITY] Pairing request accepted");
            break;
            
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            ESP_LOGI(GATTC_TAG, "[SECURITY] Authentication complete - success=%d, addr=%02X:%02X:%02X:%02X:%02X:%02X",
                    param->ble_security.auth_cmpl.success,
                    param->ble_security.auth_cmpl.bd_addr[0], param->ble_security.auth_cmpl.bd_addr[1],
                    param->ble_security.auth_cmpl.bd_addr[2], param->ble_security.auth_cmpl.bd_addr[3],
                    param->ble_security.auth_cmpl.bd_addr[4], param->ble_security.auth_cmpl.bd_addr[5]);
            if (param->ble_security.auth_cmpl.success) {
                gattc_state.pairing_complete = true;
                gattc_state.pairing_in_progress = false;
                gattc_state.pairing_start_time = 0;  // Clear timeout timer
                ESP_LOGI(GATTC_TAG, "[SECURITY] ✓✓✓ Pairing/Bonding successful! ✓✓✓");
                ESP_LOGI(GATTC_TAG, "[SECURITY] Bonding complete - device should stop blinking now");
                
                // Small delay to ensure bonding is fully processed
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // Service discovery should already be started in ESP_GATTC_OPEN_EVT
                // If it hasn't started yet, start it now (shouldn't happen, but safety check)
                if (gattc_state.connected && !gattc_state.service_discovered && gattc_if != ESP_GATT_IF_NONE) {
                    ESP_LOGW(GATTC_TAG, "[SEARCH] Pairing complete but service discovery not started - starting now");
                    esp_err_t ret = esp_ble_gattc_search_service(gattc_if, gattc_state.conn_id, &hid_service_uuid);
                    if (ret != ESP_OK) {
                        ESP_LOGE(GATTC_TAG, "[SEARCH] ✗ HID service search failed - error=0x%x (%s)", 
                                ret, esp_err_to_name(ret));
                        ret = esp_ble_gattc_search_service(gattc_if, gattc_state.conn_id, &serial_service_uuid);
                        if (ret != ESP_OK) {
                            ESP_LOGE(GATTC_TAG, "[SEARCH] ✗ Serial service search also failed: %s", esp_err_to_name(ret));
                        } else {
                            ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Serial service search initiated");
                        }
                    } else {
                        ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ HID service search initiated");
                    }
                } else {
                    ESP_LOGI(GATTC_TAG, "[SECURITY] Service discovery already in progress or complete (discovered=%d)",
                            gattc_state.service_discovered);
                }
                
                // If we have a report characteristic but haven't written CCCD yet, do it now
                if (gattc_state.report_char_handle != INVALID_HANDLE && gattc_state.connected) {
                    uint16_t cccd_handle = gattc_state.cccd_handle;
                    if (cccd_handle == INVALID_HANDLE) {
                        cccd_handle = gattc_state.report_char_handle + 1;
                        gattc_state.cccd_handle = cccd_handle;
                    }
                    
                    ESP_LOGI(GATTC_TAG, "[SECURITY] Now that bonding is complete, writing CCCD (handle=%d)...", cccd_handle);
                    uint8_t notify_data[2] = {0x01, 0x00};
                    esp_err_t ret = esp_ble_gattc_write_char_descr(gattc_if, gattc_state.conn_id,
                                                                   cccd_handle,
                                                                   sizeof(notify_data), notify_data,
                                                                   ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
                    if (ret == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[SECURITY] ✓ CCCD write request sent after bonding");
                    } else {
                        ESP_LOGW(GATTC_TAG, "[SECURITY] CCCD write failed after bonding: %s (will retry)", esp_err_to_name(ret));
                        // Try alternative handle
                        cccd_handle = gattc_state.report_char_handle + 2;
                        ret = esp_ble_gattc_write_char_descr(gattc_if, gattc_state.conn_id,
                                                             cccd_handle,
                                                             sizeof(notify_data), notify_data,
                                                             ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
                        if (ret == ESP_OK) {
                            ESP_LOGI(GATTC_TAG, "[SECURITY] ✓ CCCD write request sent to alternative handle %d", cccd_handle);
                            gattc_state.cccd_handle = cccd_handle;
                        }
                    }
                }
            } else {
                ESP_LOGE(GATTC_TAG, "[SECURITY] ✗ Pairing failed");
                gattc_state.pairing_in_progress = false;
            }
            break;
            
        case ESP_GAP_BLE_KEY_EVT:
            ESP_LOGI(GATTC_TAG, "[SECURITY] Key exchange event - key_type=%d",
                    param->ble_security.ble_key.key_type);
            // Key types: 1 = LTK (Long Term Key), 32 = IRK (Identity Resolving Key)
            // When we receive both keys, bonding is complete
            if (param->ble_security.ble_key.key_type == 1 ||  // LTK
                param->ble_security.ble_key.key_type == 32) { // IRK
                ESP_LOGI(GATTC_TAG, "[SECURITY] ✓ Bonding key received (key_type=%d)", param->ble_security.ble_key.key_type);
            }
            break;
            
        default:
            ESP_LOGD(GATTC_TAG, "[SECURITY] Unhandled security event: %d", event);
            break;
    }
}

/**
 * @brief Start BLE scan to find device by name
 */
static esp_err_t start_ble_scan(void) {
    if (!gattc_state.device_name_set) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(GATTC_TAG, "[SCAN] Starting BLE scan for device: %s", gattc_state.target_device_name);
    
    // Register GAP callback
    esp_err_t ret = esp_ble_gap_register_callback(gap_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "[SCAN] Failed to register GAP callback: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set scan parameters
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    
    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "[SCAN] Failed to set scan params: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Start scanning (30 seconds - longer to find device)
    ret = esp_ble_gap_start_scanning(30);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "[SCAN] Failed to start scanning: %s", esp_err_to_name(ret));
        return ret;
    }
    
    gattc_state.scanning = true;
    ESP_LOGI(GATTC_TAG, "[SCAN] ✓ Scan started (30 seconds)");
    return ESP_OK;
}

/**
 * @brief Parse button command from HID data
 */
static ble_button_t parse_button_command(uint8_t *data, size_t length) {
    if (length == 0) {
        ESP_LOGW(GATTC_TAG, "[BUTTON_PARSE] Empty data received");
        return BLE_BUTTON_NONE;
    }
    
    // Log raw data for debugging
    char data_str[64] = {0};
    for (size_t i = 0; i < length && i < 8; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", data[i]);
        strcat(data_str, hex);
    }
    ESP_LOGI(GATTC_TAG, "[BUTTON_PARSE] Raw HID data (len=%d): %s", length, data_str);
    
    uint8_t usage_code = 0;
    
    // Handle SmartRemote format: [usage_code][0x00]
    if (length >= 2 && data[1] == 0x00) {
        usage_code = data[0];
        if (usage_code == 0x00) {
            ESP_LOGD(GATTC_TAG, "[BUTTON_PARSE] Release event (usage_code=0x00)");
            return BLE_BUTTON_NONE; // Release event
        }
    } else if (length >= 1) {
        usage_code = data[0];
        if (usage_code == 0x00) {
            ESP_LOGD(GATTC_TAG, "[BUTTON_PARSE] No button (usage_code=0x00)");
            return BLE_BUTTON_NONE;
        }
    } else {
        ESP_LOGW(GATTC_TAG, "[BUTTON_PARSE] Invalid data length: %d", length);
        return BLE_BUTTON_NONE;
    }
    
    ESP_LOGI(GATTC_TAG, "[BUTTON_PARSE] Usage code: 0x%02X", usage_code);
    
    // HID Consumer Control Page (0x0C) usage codes
    ble_button_t button = BLE_BUTTON_NONE;
    switch (usage_code) {
        case 0xE9: // Volume Increment
            button = BLE_BUTTON_UP;
            break;
        case 0xEA: // Volume Decrement
            button = BLE_BUTTON_DOWN;
            break;
        case 0xB6: // Scan Previous Track
            button = BLE_BUTTON_LEFT;
            break;
        case 0xB5: // Scan Next Track
            button = BLE_BUTTON_RIGHT;
            break;
        case 0xCD: // Play/Pause
        case 0xB0: // Play
        case 0xB1: // Pause
            button = BLE_BUTTON_OK;
            break;
        default:
            ESP_LOGW(GATTC_TAG, "[BUTTON_PARSE] Unknown usage code: 0x%02X", usage_code);
            break;
    }
    
    if (button != BLE_BUTTON_NONE) {
        ESP_LOGI(GATTC_TAG, "[BUTTON_PARSE] Mapped to button: %d", button);
    }
    
    return button;
}

/**
 * @brief Add a characteristic handle to the polling list if not already present
 */
static void add_to_polling_list(uint16_t handle) {
    if (handle == INVALID_HANDLE) return;

    for (uint8_t i = 0; i < gattc_state.polling_char_count; i++) {
        if (gattc_state.polling_char_handles[i] == handle) {
            return; // Already in list
        }
    }

    if (gattc_state.polling_char_count < MAX_POLLING_CHARS) {
        gattc_state.polling_char_handles[gattc_state.polling_char_count++] = handle;
        ESP_LOGI(GATTC_TAG, "[POLL_LIST] Added handle %d to polling list. Count: %d", handle, gattc_state.polling_char_count);
        if (gattc_state.primary_polling_char_handle == INVALID_HANDLE) {
            gattc_state.primary_polling_char_handle = handle;
        }
    } else {
        ESP_LOGW(GATTC_TAG, "[POLL_LIST] Polling list full, cannot add handle %d", handle);
    }
}

/**
 * @brief GATT Client event handler
 */
static void gattc_event_handler(esp_gattc_cb_event_t event,
                                esp_gatt_if_t gattc_if_param,
                                esp_ble_gattc_cb_param_t *param) {
    switch (event) {
        case ESP_GATTC_REG_EVT: {
            ESP_LOGI(GATTC_TAG, "[REG] GATT Client registered - status=%d, app_id=%d, gattc_if=%d", 
                     param->reg.status, param->reg.app_id, gattc_if_param);
            if (param->reg.status == ESP_GATT_OK) {
                gattc_if = gattc_if_param;
                ESP_LOGI(GATTC_TAG, "[REG] GATT interface assigned: %d", gattc_if);
                
                // Connect to target device if MAC address is set, or start scanning if device name is set
                if (gattc_state.mac_address_set) {
                    ESP_LOGI(GATTC_TAG, "[CONNECT] Initiating connection to device: %02X:%02X:%02X:%02X:%02X:%02X",
                             gattc_state.target_mac[0], gattc_state.target_mac[1],
                             gattc_state.target_mac[2], gattc_state.target_mac[3],
                             gattc_state.target_mac[4], gattc_state.target_mac[5]);
                    esp_err_t ret = esp_ble_gattc_open(gattc_if, gattc_state.target_mac, BLE_ADDR_TYPE_PUBLIC, true);
                    if (ret) {
                        ESP_LOGE(GATTC_TAG, "[CONNECT] esp_ble_gattc_open failed, error code = 0x%x (%s)", 
                                ret, esp_err_to_name(ret));
                    } else {
                        ESP_LOGI(GATTC_TAG, "[CONNECT] Connection request sent successfully");
                    }
                } else if (gattc_state.device_name_set) {
                    ESP_LOGI(GATTC_TAG, "[SCAN] Device name set, starting scan for: %s", gattc_state.target_device_name);
                    start_ble_scan();
                } else {
                    ESP_LOGW(GATTC_TAG, "[CONNECT] Neither MAC address nor device name set, cannot connect");
                }
            } else {
                ESP_LOGE(GATTC_TAG, "[REG] Registration failed - app_id=%04x, status=%d (%s)",
                         param->reg.app_id, param->reg.status, esp_err_to_name(param->reg.status));
            }
            break;
        }
        
        case ESP_GATTC_CONNECT_EVT: {
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== ESP_GATTC_CONNECT_EVT =====");
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] conn_id=%d, gattc_if=%d", param->connect.conn_id, gattc_if_param);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] remote_bda=%02X:%02X:%02X:%02X:%02X:%02X",
                     param->connect.remote_bda[0], param->connect.remote_bda[1],
                     param->connect.remote_bda[2], param->connect.remote_bda[3],
                     param->connect.remote_bda[4], param->connect.remote_bda[5]);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] link_role=%d, conn_params: interval=%d, latency=%d, timeout=%d",
                     param->connect.link_role, 
                     param->connect.conn_params.interval,
                     param->connect.conn_params.latency,
                     param->connect.conn_params.timeout);
            ESP_LOGI(GATTC_TAG, "[CONNECT] Connection event - conn_id=%d, gattc_if=%d", 
                     param->connect.conn_id, gattc_if_param);
            ESP_LOGI(GATTC_TAG, "[CONNECT] Remote device: %02X:%02X:%02X:%02X:%02X:%02X",
                     param->connect.remote_bda[0], param->connect.remote_bda[1],
                     param->connect.remote_bda[2], param->connect.remote_bda[3],
                     param->connect.remote_bda[4], param->connect.remote_bda[5]);
            
            // Store conn_id (may be 0 initially, will be updated in OPEN_EVT)
            if (param->connect.conn_id != 0xFFFF) {
                gattc_state.conn_id = param->connect.conn_id;
                // Connection is being established - keep connection_attempt_in_progress flag set
                // It will be cleared in OPEN_EVT (success or failure)
            }
            memcpy(gattc_state.remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(GATTC_TAG, "[CONNECT] Connection ID stored: %d", gattc_state.conn_id);
            break;
        }
        
        case ESP_GATTC_OPEN_EVT: {
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== ESP_GATTC_OPEN_EVT =====");
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] conn_id=%d, status=%d (0x%02x)", 
                     param->open.conn_id, param->open.status, param->open.status);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] remote_bda=%02X:%02X:%02X:%02X:%02X:%02X",
                     param->open.remote_bda[0], param->open.remote_bda[1],
                     param->open.remote_bda[2], param->open.remote_bda[3],
                     param->open.remote_bda[4], param->open.remote_bda[5]);
            ESP_LOGI(GATTC_TAG, "[OPEN] Connection opened - conn_id=%d, status=%d", 
                     param->open.conn_id, param->open.status);
            ESP_LOGI(GATTC_TAG, "[OPEN] Remote device: %02X:%02X:%02X:%02X:%02X:%02X",
                     param->open.remote_bda[0], param->open.remote_bda[1],
                     param->open.remote_bda[2], param->open.remote_bda[3],
                     param->open.remote_bda[4], param->open.remote_bda[5]);
            
            // Clear connection attempt tracking (whether success or failure)
            gattc_state.connection_attempt_in_progress = false;
            gattc_state.connection_attempt_start_time = 0;
            
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGE(GATTC_TAG, "[BLE_LOG] Connection FAILED - status=%d (0x%02x) = %s", 
                        param->open.status, param->open.status, esp_err_to_name(param->open.status));
                ESP_LOGE(GATTC_TAG, "[OPEN] Connection failed - status=%d (%s)", 
                        param->open.status, esp_err_to_name(param->open.status));
                // Schedule rescan on connection failure
                gattc_state.do_scan = true;
                reconnect_delay = xTaskGetTickCount() + pdMS_TO_TICKS(5000); // 5 second delay before retry
                break;
            }
            
            ESP_LOGI(GATTC_TAG, "[OPEN] Connection successful! Starting service discovery...");
            gattc_state.connected = true;
            gattc_state.conn_id = param->open.conn_id;
            memcpy(gattc_state.remote_bda, param->open.remote_bda, sizeof(esp_bd_addr_t));
            gattc_state.pairing_complete = false;
            gattc_state.pairing_in_progress = false;
            
            // Store connected device name
            if (gattc_state.device_name_set) {
                strncpy(gattc_state.connected_device_name, gattc_state.target_device_name, 
                       sizeof(gattc_state.connected_device_name) - 1);
                gattc_state.connected_device_name[sizeof(gattc_state.connected_device_name) - 1] = '\0';
            }
            
            // CRITICAL: Match working code - start service discovery after connection
            // Pairing will happen automatically during GATT operations if needed
            // Don't call esp_ble_set_encryption immediately - it causes connection timeouts
            // Small delay to ensure connection is fully established
            vTaskDelay(pdMS_TO_TICKS(200));
            
            ESP_LOGI(GATTC_TAG, "[SEARCH] ===== Starting service discovery (matching working code) =====");
            
            // Use the stored conn_id (from CONNECT_EVT) or the one from OPEN_EVT
            // conn_id of 0 is valid in ESP-IDF (only 0xFFFF is invalid)
            uint16_t conn_id_to_use = param->open.conn_id;
            if (conn_id_to_use == 0xFFFF && gattc_state.conn_id != 0xFFFF) {
                conn_id_to_use = gattc_state.conn_id;
                ESP_LOGI(GATTC_TAG, "[SEARCH] Using stored conn_id=%d (from CONNECT_EVT)", conn_id_to_use);
            }
            
            ESP_LOGI(GATTC_TAG, "[SEARCH] gattc_if=%d, conn_id=%d", gattc_if, conn_id_to_use);
            ESP_LOGI(GATTC_TAG, "[SEARCH] Pairing will complete during service discovery if device requires it");
            
            // Verify conn_id is valid before starting service discovery
            // Note: conn_id of 0 is VALID in ESP-IDF, only 0xFFFF is invalid
            if (conn_id_to_use == 0xFFFF) {
                ESP_LOGE(GATTC_TAG, "[SEARCH] ✗ Invalid conn_id=0xFFFF, cannot start service discovery");
                ESP_LOGI(GATTC_TAG, "[SEARCH] Will retry service discovery in update loop when conn_id is available");
                break;
            }
            
            // Log that we're using conn_id (even if it's 0, which is valid)
            ESP_LOGI(GATTC_TAG, "[SEARCH] Using conn_id=%d (0 is valid in ESP-IDF, only 0xFFFF is invalid)", conn_id_to_use);
            
            // Update stored conn_id if it was 0xFFFF
            if (gattc_state.conn_id == 0xFFFF) {
                gattc_state.conn_id = conn_id_to_use;
            }
            
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] Calling esp_ble_gattc_search_service:");
            ESP_LOGI(GATTC_TAG, "[BLE_LOG]   gattc_if=%d", gattc_if);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG]   conn_id=%d", conn_id_to_use);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG]   service_uuid=0x%04X (HID Service)", hid_service_uuid.uuid.uuid16);
            esp_err_t ret = esp_ble_gattc_search_service(gattc_if, conn_id_to_use, &hid_service_uuid);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] esp_ble_gattc_search_service returned: %d (0x%x) = %s", 
                    ret, ret, esp_err_to_name(ret));
            if (ret != ESP_OK) {
                ESP_LOGE(GATTC_TAG, "[SEARCH] ✗ HID service search failed - error=0x%x (%s)", 
                        ret, esp_err_to_name(ret));
                // If HID service search fails, try serial service
                ESP_LOGI(GATTC_TAG, "[SEARCH] Trying serial service (UUID: 0000ffe0-0000-1000-8000-00805f9b34fb)...");
                ESP_LOGI(GATTC_TAG, "[BLE_LOG] Calling esp_ble_gattc_search_service (serial):");
                ESP_LOGI(GATTC_TAG, "[BLE_LOG]   gattc_if=%d, conn_id=%d, service_uuid=0xFFE0", gattc_if, conn_id_to_use);
                ret = esp_ble_gattc_search_service(gattc_if, conn_id_to_use, &serial_service_uuid);
                ESP_LOGI(GATTC_TAG, "[BLE_LOG] esp_ble_gattc_search_service (serial) returned: %d (0x%x) = %s", 
                        ret, ret, esp_err_to_name(ret));
                if (ret != ESP_OK) {
                    ESP_LOGE(GATTC_TAG, "[SEARCH] Serial service search also failed: %s", esp_err_to_name(ret));
                } else {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] Serial service search initiated");
                }
            } else {
                ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ HID service search initiated successfully");
            }
            break;
        }
        
        case ESP_GATTC_DISCONNECT_EVT: {
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== ESP_GATTC_DISCONNECT_EVT =====");
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] conn_id=%d", param->disconnect.conn_id);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] reason=0x%04x (%d)", param->disconnect.reason, param->disconnect.reason);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] remote_bda=%02X:%02X:%02X:%02X:%02X:%02X",
                     param->disconnect.remote_bda[0], param->disconnect.remote_bda[1],
                     param->disconnect.remote_bda[2], param->disconnect.remote_bda[3],
                     param->disconnect.remote_bda[4], param->disconnect.remote_bda[5]);
            const char* reason_str = "Unknown";
            // Use uint16_t to handle reason codes up to 0x100
            uint16_t reason = (uint16_t)param->disconnect.reason;
            switch (reason) {
                case 0x08: reason_str = "Connection Timeout"; break;
                case 0x13: reason_str = "Remote User Terminated"; break;
                case 0x16: reason_str = "Connection Terminated by Local Host"; break;
                case 0x3B: reason_str = "Connection Failed to be Established"; break;
                case 0x3E: reason_str = "Connection Failed (0x3E)"; break;
                case 0x100: reason_str = "Connection Failed (0x100) - Likely device not advertising"; break;
                default: reason_str = "Other"; break;
            }
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] reason_str=%s", reason_str);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] Current state: connected=%d, service_discovered=%d, char_discovered=%d, pairing_complete=%d",
                     gattc_state.connected, gattc_state.service_discovered, gattc_state.char_discovered, gattc_state.pairing_complete);
            ESP_LOGW(GATTC_TAG, "[DISCONNECT] Connection lost - conn_id=%d, reason=0x%02x (%s)", 
                     param->disconnect.conn_id, param->disconnect.reason, reason_str);
            ESP_LOGI(GATTC_TAG, "[DISCONNECT] Remote device: %02X:%02X:%02X:%02X:%02X:%02X",
                     param->disconnect.remote_bda[0], param->disconnect.remote_bda[1],
                     param->disconnect.remote_bda[2], param->disconnect.remote_bda[3],
                     param->disconnect.remote_bda[4], param->disconnect.remote_bda[5]);
            ESP_LOGI(GATTC_TAG, "[DISCONNECT] State before disconnect: connected=%d, service_discovered=%d, char_discovered=%d, pairing_complete=%d",
                     gattc_state.connected, gattc_state.service_discovered, gattc_state.char_discovered, gattc_state.pairing_complete);
            
            if (param->disconnect.reason == 0x08) {
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] Connection timeout - device may have disconnected due to incomplete bonding");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] Make sure bonding completes before attempting GATT operations");
            } else if (param->disconnect.reason == 0x3E) {
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] Connection failed (0x3E) - may be caused by:");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] - Button callback blocking or crashing");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] - GATT queue overflow");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] - Device-side disconnection");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] - Ensure button callback is fast and non-blocking");
            } else if (param->disconnect.reason == 0x100) {
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] Connection failed (0x100) - device may have stopped advertising");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] Device may need to be reset or may be in pairing mode");
                ESP_LOGW(GATTC_TAG, "[DISCONNECT] Will immediately rescan to find device again");
            }
            
            gattc_state.connected = false;
            gattc_state.service_discovered = false;
            gattc_state.char_discovered = false;
            gattc_state.pairing_complete = false;
            gattc_state.pairing_in_progress = false;
            gattc_state.pairing_start_time = 0;  // Reset pairing timer
            gattc_state.connection_attempt_in_progress = false;  // Clear connection attempt flag
            gattc_state.connection_attempt_start_time = 0;  // Reset connection attempt timer
            gattc_state.conn_id = 0xFFFF;  // Reset conn_id to indicate no active connection
            gattc_state.hid_service_handle = INVALID_HANDLE;
            gattc_state.hid_service_end_handle = INVALID_HANDLE;
            gattc_state.serial_service_handle = INVALID_HANDLE;
            gattc_state.serial_service_end_handle = INVALID_HANDLE;
            gattc_state.report_char_handle = INVALID_HANDLE;
            gattc_state.serial_char_handle = INVALID_HANDLE;
            gattc_state.cccd_handle = INVALID_HANDLE;
            gattc_state.hid_control_point_handle = INVALID_HANDLE;
            gattc_state.primary_polling_char_handle = INVALID_HANDLE;
            gattc_state.polling_char_count = 0;
            gattc_state.report_char_count = 0;
            memset(gattc_state.polling_char_handles, 0, sizeof(gattc_state.polling_char_handles));
            memset(gattc_state.report_char_handles, 0, sizeof(gattc_state.report_char_handles));
            gattc_state.char_discovery_in_progress = false;
            memset(gattc_state.connected_device_name, 0, sizeof(gattc_state.connected_device_name));
            gattc_state.do_scan = true; // Schedule re-scan after disconnect
            
            // For reason 0x100 (device not advertising), immediately rescan instead of waiting
            if (param->disconnect.reason == 0x100) {
                reconnect_delay = 0; // No delay - rescan immediately
                ESP_LOGI(GATTC_TAG, "[DISCONNECT] Will immediately rescan to find device");
            } else {
                // Reset reconnect delay - wait longer to ensure connection is fully closed
                // Use longer delay for certain error codes to prevent spamming the device
                uint32_t delay_ms = 10000; // Default 10 seconds
                if (param->disconnect.reason == 0x3E) {
                    delay_ms = 20000; // 20 seconds for 0x3E to allow device to recover
                    ESP_LOGW(GATTC_TAG, "[DISCONNECT] Using extended delay (%lu ms) due to error code 0x%02x", delay_ms, param->disconnect.reason);
                }
                reconnect_delay = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
                ESP_LOGI(GATTC_TAG, "[DISCONNECT] Will attempt reconnection in %lu seconds", delay_ms / 1000);
            }
            break;
        }
        
        case ESP_GATTC_SEARCH_RES_EVT: {
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== ESP_GATTC_SEARCH_RES_EVT =====");
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] conn_id=%d", param->search_res.conn_id);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] UUID len=%d", param->search_res.srvc_id.uuid.len);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] start_handle=%d, end_handle=%d", 
                     param->search_res.start_handle, param->search_res.end_handle);
            ESP_LOGI(GATTC_TAG, "[SEARCH] ===== Service found event received =====");
            ESP_LOGI(GATTC_TAG, "[SEARCH] UUID len=%d", param->search_res.srvc_id.uuid.len);
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16) {
                uint16_t uuid16 = param->search_res.srvc_id.uuid.uuid.uuid16;
                ESP_LOGI(GATTC_TAG, "[SEARCH] Service UUID: 0x%04X, start_handle=%d, end_handle=%d",
                         uuid16, param->search_res.start_handle, param->search_res.end_handle);
                
                // Log all services found for debugging
                if (uuid16 == 0x1800) {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Generic Access service (0x1800) found");
                } else if (uuid16 == 0x1801) {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Generic Attribute service (0x1801) found");
                } else if (uuid16 == 0x1812) {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓✓✓ HID service (0x1812) found! ✓✓✓");
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓✓✓ TEXT INPUT PROFILE IDENTIFIED! ✓✓✓");
                    ESP_LOGI(GATTC_TAG, "[SEARCH] Service handle range: %d to %d",
                             param->search_res.start_handle, param->search_res.end_handle);
                    gattc_state.hid_service_handle = param->search_res.start_handle;
                    gattc_state.hid_service_end_handle = param->search_res.end_handle;
                } else if (uuid16 == 0x180F) {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Battery service (0x180F) found");
                } else if (uuid16 == 0x180A) {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Device Information service (0x180A) found");
                } else {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] Other service found: 0x%04X (not HID)", uuid16);
                }
            } else if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128) {
                // Check for serial service UUID: 0000ffe0-0000-1000-8000-00805f9b34fb
                const uint8_t* uuid128 = param->search_res.srvc_id.uuid.uuid.uuid128;
                if (uuid128[0] == 0xfb && uuid128[1] == 0x34 && uuid128[2] == 0x9b && uuid128[3] == 0x5f &&
                    uuid128[4] == 0x80 && uuid128[5] == 0x00 && uuid128[6] == 0x00 && uuid128[7] == 0x80 &&
                    uuid128[8] == 0x00 && uuid128[9] == 0x10 && uuid128[10] == 0x00 && uuid128[11] == 0x00 &&
                    uuid128[12] == 0xe0 && uuid128[13] == 0xff && uuid128[14] == 0x00 && uuid128[15] == 0x00) {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Serial service (0000ffe0) found!");
                    gattc_state.serial_service_handle = param->search_res.start_handle;
                    gattc_state.serial_service_end_handle = param->search_res.end_handle;
                }
            } else {
                ESP_LOGD(GATTC_TAG, "[SEARCH] Service with non-16-bit UUID found");
            }
            break;
        }
        
        case ESP_GATTC_SEARCH_CMPL_EVT: {
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== ESP_GATTC_SEARCH_CMPL_EVT =====");
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] conn_id=%d", param->search_cmpl.conn_id);
            ESP_LOGI(GATTC_TAG, "[BLE_LOG] status=%d (0x%x) = %s", 
                     param->search_cmpl.status, param->search_cmpl.status, esp_err_to_name(param->search_cmpl.status));
            ESP_LOGI(GATTC_TAG, "[SEARCH] ===== Service search complete event =====");
            ESP_LOGI(GATTC_TAG, "[SEARCH] Status=%d (%s)", 
                     param->search_cmpl.status, esp_err_to_name(param->search_cmpl.status));
            ESP_LOGI(GATTC_TAG, "[SEARCH] Current hid_service_handle=%d (INVALID_HANDLE=%d)", 
                     gattc_state.hid_service_handle, INVALID_HANDLE);
            
            if (gattc_state.hid_service_handle != INVALID_HANDLE) {
                gattc_state.service_discovered = true;
                ESP_LOGI(GATTC_TAG, "[SEARCH] ✓✓✓ HID service discovered successfully! ✓✓✓");
                ESP_LOGI(GATTC_TAG, "[SEARCH] ✓✓✓ TEXT INPUT PROFILE CONFIRMED! ✓✓✓");
                ESP_LOGI(GATTC_TAG, "[SEARCH] Handle range: %d to %d", 
                        gattc_state.hid_service_handle, gattc_state.hid_service_end_handle);
                
                // CRITICAL: Use assumed handles and register for notifications
                // ESP-IDF's characteristic discovery API is different from Arduino BLE library
                // We'll use assumed handles and let notifications discover the actual handles
                ESP_LOGI(GATTC_TAG, "[SEARCH] Using assumed handles for HID characteristics...");
                gattc_state.char_discovery_in_progress = false;
                gattc_state.report_char_count = 0;
                memset(gattc_state.report_char_handles, 0, sizeof(gattc_state.report_char_handles));
                
                // Assume report characteristic is at service_handle + 1 (common HID layout)
                gattc_state.report_char_handle = gattc_state.hid_service_handle + 1;
                gattc_state.hid_control_point_handle = gattc_state.hid_service_handle + 3;
                ESP_LOGI(GATTC_TAG, "[SEARCH] Assuming Report characteristic at handle %d, Control Point at handle %d",
                        gattc_state.report_char_handle, gattc_state.hid_control_point_handle);
                
                // Register for notifications on assumed report characteristic
                // The actual handle will be discovered when REG_FOR_NOTIFY_EVT is received
                ESP_LOGI(GATTC_TAG, "[SEARCH] Registering for notifications on assumed handle %d...",
                         gattc_state.report_char_handle);
                esp_err_t ret = esp_ble_gattc_register_for_notify(gattc_if, gattc_state.remote_bda,
                                                                   gattc_state.report_char_handle);
                if (ret != ESP_OK) {
                    ESP_LOGW(GATTC_TAG, "[SEARCH] Notification registration failed: %s (will retry later)", 
                            esp_err_to_name(ret));
                } else {
                    ESP_LOGI(GATTC_TAG, "[SEARCH] ✓ Notification registration request sent");
                }
            } else {
                ESP_LOGW(GATTC_TAG, "[SEARCH] ✗✗✗ HID service (0x1812) NOT found during search ✗✗✗");
                ESP_LOGI(GATTC_TAG, "[SEARCH] Search status: %d (%s)", 
                        param->search_cmpl.status, esp_err_to_name(param->search_cmpl.status));
                ESP_LOGI(GATTC_TAG, "[SEARCH] hid_service_handle: %d (expected != %d)", 
                        gattc_state.hid_service_handle, INVALID_HANDLE);
                ESP_LOGW(GATTC_TAG, "[SEARCH] Device may not expose HID service, or search failed");
                ESP_LOGW(GATTC_TAG, "[SEARCH] Will try to discover all services or use serial service");
            }
            break;
        }
        
        case ESP_GATTC_READ_CHAR_EVT: {
            gattc_state.read_in_progress = false; // Reset flag when read completes
            ESP_LOGI(GATTC_TAG, "[READ] Characteristic read event - handle=%d, status=%d, value_len=%d", 
                     param->read.handle, param->read.status, param->read.value_len);
            if (param->read.status != ESP_GATT_OK) {
                ESP_LOGE(GATTC_TAG, "[READ] ✗ Read failed: %s", esp_err_to_name(param->read.status));
            } else {
                ESP_LOGI(GATTC_TAG, "[READ] ✓ Read successful, value_len=%d", param->read.value_len);
                
                // Log the actual data bytes for debugging
                if (param->read.value_len > 0 && param->read.value) {
                    char data_str[64] = {0};
                    int offset = 0;
                    for (int i = 0; i < param->read.value_len && i < 8 && offset < 60; i++) {
                        offset += snprintf(data_str + offset, 64 - offset, "%02X ", param->read.value[i]);
                    }
                    ESP_LOGI(GATTC_TAG, "[READ] Data bytes: %s", data_str);
                    
                    // Check if all bytes are zero (idle state)
                    bool all_zeros = true;
                    for (int i = 0; i < param->read.value_len; i++) {
                        if (param->read.value[i] != 0) {
                            all_zeros = false;
                            break;
                        }
                    }
                    
                    if (all_zeros) {
                        ESP_LOGD(GATTC_TAG, "[READ] All zeros (idle state)");
                    } else {
                        ESP_LOGI(GATTC_TAG, "[READ] Non-zero data detected!");
                        // Parse button command from polled data
                        ble_button_t button = parse_button_command(param->read.value, param->read.value_len);
                        if (button != BLE_BUTTON_NONE) {
                            gattc_state.button_count++;
                            gattc_state.last_button = button;
                            
                            const char* button_names[] = {"NONE", "UP", "DOWN", "LEFT", "RIGHT", "OK"};
                            const char* button_name = (button < 6) ? button_names[button] : "UNKNOWN";
                            
                            ESP_LOGI(GATTC_TAG, "[POLL] ✓✓✓ BUTTON PRESSED #%lu (from polling): %s (code=%d) ✓✓✓",
                                    gattc_state.button_count, button_name, button);
                            
                            if (gattc_state.button_callback) {
                                gattc_state.button_callback(button);
                            }
                        } else {
                            ESP_LOGW(GATTC_TAG, "[READ] Non-zero data but parse_button_command returned NONE");
                        }
                    }
                } else {
                    ESP_LOGW(GATTC_TAG, "[READ] No data received (value_len=%d, value=%p)", 
                            param->read.value_len, param->read.value);
                }
            }
            break;
        }
        
        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            ESP_LOGI(GATTC_TAG, "[PAIRING] Notification registration event - status=%d, handle=%d",
                     param->reg_for_notify.status, param->reg_for_notify.handle);
            ESP_LOGI(GATTC_TAG, "[PAIRING] Remote device: %02X:%02X:%02X:%02X:%02X:%02X",
                     gattc_state.remote_bda[0], gattc_state.remote_bda[1],
                     gattc_state.remote_bda[2], gattc_state.remote_bda[3],
                     gattc_state.remote_bda[4], gattc_state.remote_bda[5]);
            
            if (param->reg_for_notify.status == ESP_GATT_OK) {
                ESP_LOGI(GATTC_TAG, "[PAIRING] ✓ Notification registration successful!");
                ESP_LOGI(GATTC_TAG, "[PAIRING] Report characteristic handle: %d", param->reg_for_notify.handle);
                
                // Store as primary if not set
                if (gattc_state.report_char_handle == INVALID_HANDLE) {
                    gattc_state.report_char_handle = param->reg_for_notify.handle;
                }
                
                // Add to report characteristics list if not already present (matching working code)
                bool already_in_list = false;
                for (uint8_t i = 0; i < gattc_state.report_char_count; i++) {
                    if (gattc_state.report_char_handles[i] == param->reg_for_notify.handle) {
                        already_in_list = true;
                        break;
                    }
                }
                if (!already_in_list && gattc_state.report_char_count < MAX_POLLING_CHARS) {
                    gattc_state.report_char_handles[gattc_state.report_char_count++] = param->reg_for_notify.handle;
                    ESP_LOGI(GATTC_TAG, "[PAIRING] Added to report characteristics list (total=%d)", gattc_state.report_char_count);
                }
                
                // Store CCCD handle for later use
                gattc_state.cccd_handle = param->reg_for_notify.handle + 1;
                
                // Wait for pairing to complete before writing CCCD
                // This is critical - many HID devices require bonding to complete first
                if (!gattc_state.pairing_complete) {
                    ESP_LOGI(GATTC_TAG, "[PAIRING] Waiting for pairing/bonding to complete before writing CCCD...");
                    ESP_LOGI(GATTC_TAG, "[PAIRING] Device may still be blinking - this is normal until bonding completes");
                    // Add to polling list as fallback
                    add_to_polling_list(param->reg_for_notify.handle);
                    break;
                }
                
                // Pairing is complete - now write CCCD
                uint16_t cccd_handle = param->reg_for_notify.handle + 1;
                uint8_t notify_data[2] = {0x01, 0x00}; // Enable notifications (little-endian: 0x0001)
                
                ESP_LOGI(GATTC_TAG, "[PAIRING] Pairing complete! Enabling notifications on CCCD (handle=%d)...", cccd_handle);
                
                // Use MITM authentication since pairing is complete
                esp_err_t ret = esp_ble_gattc_write_char_descr(gattc_if, gattc_state.conn_id,
                                                               cccd_handle,
                                                               sizeof(notify_data), notify_data,
                                                               ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
                if (ret != ESP_OK) {
                    ESP_LOGW(GATTC_TAG, "[PAIRING] CCCD write failed on handle %d, trying handle %d - error=0x%x (%s)", 
                            cccd_handle, cccd_handle + 1, ret, esp_err_to_name(ret));
                    // Try next handle
                    cccd_handle = param->reg_for_notify.handle + 2;
                    ret = esp_ble_gattc_write_char_descr(gattc_if, gattc_state.conn_id,
                                                         cccd_handle,
                                                         sizeof(notify_data), notify_data,
                                                         ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
                }
                
                if (ret == ESP_OK) {
                    ESP_LOGI(GATTC_TAG, "[PAIRING] ✓ CCCD write request sent to handle %d", cccd_handle);
                    gattc_state.cccd_handle = cccd_handle;
                } else {
                    ESP_LOGE(GATTC_TAG, "[PAIRING] ✗ CCCD write failed - error=0x%x (%s)", 
                            ret, esp_err_to_name(ret));
                    ESP_LOGI(GATTC_TAG, "[PAIRING] Will wait for WRITE_DESCR_EVT to confirm or try polling");
                }
                
                // Add to polling list as fallback
                add_to_polling_list(param->reg_for_notify.handle);
            } else {
                ESP_LOGE(GATTC_TAG, "[PAIRING] ✗ Notification registration failed - status=%d (%s)",
                        param->reg_for_notify.status, esp_err_to_name(param->reg_for_notify.status));
            }
            break;
        }
        
        case ESP_GATTC_READ_DESCR_EVT: {
            ESP_LOGI(GATTC_TAG, "[READ] Descriptor read event - handle=%d, status=%d, value_len=%d", 
                     param->read.handle, param->read.status, param->read.value_len);
            if (param->read.status == ESP_GATT_OK) {
                ESP_LOGI(GATTC_TAG, "[READ] ✓ Descriptor read successful, value_len=%d", param->read.value_len);
                
                // Check if this is Report Reference descriptor (0x2908) - typically at report_char_handle + 2
                // Matching working code: read Report Reference and write report ID back
                if (param->read.value_len >= 1 && param->read.value && 
                    param->read.handle >= gattc_state.report_char_handle && 
                    param->read.handle <= gattc_state.report_char_handle + 3) {
                    uint8_t report_id = param->read.value[0];
                    ESP_LOGI(GATTC_TAG, "[HID_INIT] Report Reference descriptor read - Report ID: %d", report_id);
                    
                    // Write the report ID back to the characteristic (matching working code)
                    if (gattc_state.report_char_handle != INVALID_HANDLE) {
                        ESP_LOGI(GATTC_TAG, "[HID_INIT] Writing report ID %d to characteristic to enable notifications...", report_id);
                        uint8_t report_id_bytes[] = {report_id};
                        esp_err_t ret = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                                                  gattc_state.report_char_handle,
                                                                  sizeof(report_id_bytes), report_id_bytes,
                                                                  ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
                        if (ret == ESP_OK) {
                            ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ Report ID %d write request sent", report_id);
                        } else {
                            ESP_LOGD(GATTC_TAG, "[HID_INIT] Report ID write failed: %s", esp_err_to_name(ret));
                        }
                    }
                }
            } else {
                ESP_LOGD(GATTC_TAG, "[READ] Descriptor read failed: %s", esp_err_to_name(param->read.status));
            }
            break;
        }
        
        case ESP_GATTC_WRITE_CHAR_EVT: {
            ESP_LOGI(GATTC_TAG, "[WRITE] Characteristic write event - handle=%d, status=%d", 
                     param->write.handle, param->write.status);
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGE(GATTC_TAG, "[WRITE] ✗ Write failed: %s", esp_err_to_name(param->write.status));
            } else {
                ESP_LOGI(GATTC_TAG, "[WRITE] ✓ Write successful");
            }
            break;
        }
        
        case ESP_GATTC_WRITE_DESCR_EVT: {
            ESP_LOGI(GATTC_TAG, "[WRITE] Descriptor write event - handle=%d, status=%d (0x%02x)", 
                     param->write.handle, param->write.status, param->write.status);
            ESP_LOGI(GATTC_TAG, "[WRITE] Expected CCCD handle: %d, report_char_handle: %d", 
                     gattc_state.cccd_handle, gattc_state.report_char_handle);
            
            // Check if this is our CCCD write (handle should match or be close)
            // CCCD is typically at report_char_handle + 1
            bool is_cccd = (param->write.handle == gattc_state.cccd_handle) ||
                          (param->write.handle == gattc_state.report_char_handle + 1) ||
                          (gattc_state.report_char_handle != INVALID_HANDLE && 
                           param->write.handle >= gattc_state.report_char_handle && 
                           param->write.handle <= gattc_state.report_char_handle + 2);
            
            if (is_cccd) {
                ESP_LOGI(GATTC_TAG, "[WRITE] This is our CCCD write (handle=%d)", param->write.handle);
                
                // Update CCCD handle if it was different
                if (param->write.handle != gattc_state.cccd_handle) {
                    ESP_LOGI(GATTC_TAG, "[WRITE] CCCD handle corrected: %d -> %d", 
                            gattc_state.cccd_handle, param->write.handle);
                    gattc_state.cccd_handle = param->write.handle;
                }
                
                if (param->write.status == ESP_GATT_OK) {
                    ESP_LOGI(GATTC_TAG, "[WRITE] ✓✓✓ CCCD write successful - Notifications ENABLED! ✓✓✓");
                    gattc_state.pairing_complete = true; // Mark pairing as complete after successful CCCD write
                    
                    // Now initialize HID device (following reference implementation order)
                    ESP_LOGI(GATTC_TAG, "[HID_INIT] Initializing HID device...");
                    
                    // 1. Write to HID Control Point (0x2A4C) to wake up device (matching working code)
                    // HID Control Point: Suspend = 0, Exit Suspend = 1
                    if (gattc_state.hid_control_point_handle != INVALID_HANDLE) {
                        ESP_LOGI(GATTC_TAG, "[HID_INIT] Writing to HID Control Point (0x2A4C) at handle %d to wake device...",
                                gattc_state.hid_control_point_handle);
                        uint8_t wake_up[] = {0x01}; // Exit Suspend
                        esp_err_t ret_cp = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                                                gattc_state.hid_control_point_handle,
                                                                sizeof(wake_up), wake_up,
                                                                ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                        if (ret_cp == ESP_OK) {
                            ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ HID Control Point write request sent");
                        } else {
                            ESP_LOGW(GATTC_TAG, "[HID_INIT] HID Control Point write failed: %s", 
                                    esp_err_to_name(ret_cp));
                        }
                        vTaskDelay(pdMS_TO_TICKS(100)); // Give device time to process (matching working code)
                    } else {
                        ESP_LOGW(GATTC_TAG, "[HID_INIT] HID Control Point not found, skipping wake-up");
                    }
                    
                    // 2. Try to read Report Reference descriptor (0x2908) and write report ID back (matching working code)
                    // Report Reference descriptor is typically at report_char_handle + 1 (after CCCD)
                    // But we need to find it - try reading descriptor at report_char_handle + 2
                    ESP_LOGI(GATTC_TAG, "[HID_INIT] Attempting to read Report Reference descriptor (0x2908)...");
                    uint16_t report_ref_handle = gattc_state.report_char_handle + 2; // After CCCD
                    esp_err_t ret_desc = esp_ble_gattc_read_char_descr(gattc_if, gattc_state.conn_id, report_ref_handle, ESP_GATT_AUTH_REQ_NONE);
                    if (ret_desc == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ Report Reference descriptor read request sent (handle=%d)", report_ref_handle);
                    } else {
                        ESP_LOGD(GATTC_TAG, "[HID_INIT] Report Reference descriptor read failed: %s (will continue anyway)", 
                                esp_err_to_name(ret_desc));
                    }
                    
                    // 3. Try writing report IDs 2, 3, 4 to initialize (matching working code)
                    ESP_LOGI(GATTC_TAG, "[HID_INIT] Writing report IDs 2, 3, 4 to initialize device...");
                    for (uint8_t report_id = 2; report_id <= 4; report_id++) {
                        uint8_t report_id_bytes[] = {report_id};
                        esp_err_t ret_write = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                                      gattc_state.report_char_handle,
                                                      sizeof(report_id_bytes), report_id_bytes,
                                                      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
                        if (ret_write == ESP_OK) {
                            ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ Report ID %d write request sent", report_id);
                        } else {
                            ESP_LOGD(GATTC_TAG, "[HID_INIT] Report ID %d write failed: %s", 
                                    report_id, esp_err_to_name(ret_write));
                        }
                        vTaskDelay(pdMS_TO_TICKS(50)); // Small delay between writes (matching working code)
                    }
                    
                    // Mark as discovered and ready
                    gattc_state.char_discovered = true;
                    ESP_LOGI(GATTC_TAG, "[PAIRING] ✓✓✓ Pairing complete! Ready to receive button presses. ✓✓✓");
                    ESP_LOGI(GATTC_TAG, "[PAIRING] Notifications should be enabled - waiting for button presses via notifications");
                    
                    // Re-register for notifications after HID initialization (matching working code)
                    ESP_LOGI(GATTC_TAG, "[PAIRING] Re-registering for notifications after HID init...");
                    esp_err_t ret_notify = esp_ble_gattc_register_for_notify(gattc_if, gattc_state.remote_bda, gattc_state.report_char_handle);
                    if (ret_notify == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[PAIRING] ✓ Re-registration request sent");
                    } else {
                        ESP_LOGW(GATTC_TAG, "[PAIRING] Re-registration failed: %s", esp_err_to_name(ret_notify));
                    }
                } else {
                    ESP_LOGE(GATTC_TAG, "[WRITE] ✗ CCCD write failed - handle=%d, status=%d (0x%02x) (%s)", 
                            param->write.handle, param->write.status, param->write.status, esp_err_to_name(param->write.status));
                    
                    // Status 13 = ESP_GATT_WRITE_NOT_PERMITTED - device requires pairing/bonding
                    // Status 133 (0x85) = ESP_ERR_INVALID_RESPONSE or similar
                    if (param->write.status == 13) {
                        ESP_LOGW(GATTC_TAG, "[WRITE] Status 13 = Write not permitted - device requires pairing");
                        ESP_LOGW(GATTC_TAG, "[WRITE] However, many HID devices enable notifications automatically");
                        ESP_LOGW(GATTC_TAG, "[WRITE] Will continue monitoring for notifications");
                        
                        // Even though CCCD write failed, try HID initialization
                        // Some devices work without explicit CCCD write
                        ESP_LOGI(GATTC_TAG, "[HID_INIT] Attempting HID initialization despite CCCD write failure...");
                        
                        // Try HID Control Point write
                        if (gattc_state.hid_control_point_handle != INVALID_HANDLE) {
                            uint8_t wake_up[] = {0x01};
                            esp_err_t ret_cp2 = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                                                    gattc_state.hid_control_point_handle,
                                                                    sizeof(wake_up), wake_up,
                                                                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                            if (ret_cp2 == ESP_OK) {
                                ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ HID Control Point write request sent");
                            }
                        }
                        
                        // Try report ID writes
                        for (uint8_t report_id = 2; report_id <= 4; report_id++) {
                            uint8_t report_id_bytes[] = {report_id};
                            esp_err_t ret_rid = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                                          gattc_state.report_char_handle,
                                                          sizeof(report_id_bytes), report_id_bytes,
                                                          ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
                            if (ret_rid == ESP_OK) {
                                ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ Report ID %d write request sent", report_id);
                            }
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        
                        ESP_LOGI(GATTC_TAG, "[PAIRING] Monitoring for notifications - device may work without CCCD write");
                        
                        // Even if CCCD write failed, try HID initialization and add to polling
                        if (gattc_state.report_char_handle != INVALID_HANDLE && 
                            gattc_state.polling_char_count < MAX_POLLING_CHARS) {
                            bool already_in_list = false;
                            for (uint8_t i = 0; i < gattc_state.polling_char_count; i++) {
                                if (gattc_state.polling_char_handles[i] == gattc_state.report_char_handle) {
                                    already_in_list = true;
                                    break;
                                }
                            }
                            if (!already_in_list) {
                                gattc_state.polling_char_handles[gattc_state.polling_char_count] = gattc_state.report_char_handle;
                                gattc_state.polling_char_count++;
                                if (gattc_state.primary_polling_char_handle == INVALID_HANDLE) {
                                    gattc_state.primary_polling_char_handle = gattc_state.report_char_handle;
                                }
                                ESP_LOGI(GATTC_TAG, "[HID_INIT] Added report characteristic to polling list (fallback)");
                            }
                        }
                    } else if (param->write.status == 133 || param->write.status == 0x85) {
                        ESP_LOGW(GATTC_TAG, "[WRITE] Status 133 = Invalid response - CCCD handle may be wrong");
                        ESP_LOGW(GATTC_TAG, "[WRITE] Expected handle: %d, got: %d", 
                                gattc_state.cccd_handle, param->write.handle);
                        ESP_LOGW(GATTC_TAG, "[WRITE] Some HID devices enable notifications automatically");
                    }
                }
            } else {
                // This might be a different descriptor write - log it but don't process
                ESP_LOGI(GATTC_TAG, "[WRITE] Descriptor write on handle %d (not recognized as CCCD) - status=%d", 
                        param->write.handle, param->write.status);
                if (param->write.status != ESP_GATT_OK) {
                    ESP_LOGW(GATTC_TAG, "[WRITE] Descriptor write on handle %d failed: %s", 
                            param->write.handle, esp_err_to_name(param->write.status));
                } else {
                    ESP_LOGI(GATTC_TAG, "[WRITE] Descriptor write successful on handle %d (might be CCCD at different handle)", 
                            param->write.handle);
                    // Check if this could be CCCD at a different handle (handle 17 is expected for char 16)
                    if (param->write.handle >= 15 && param->write.handle <= 20 && 
                        gattc_state.report_char_handle == 16) {
                        ESP_LOGI(GATTC_TAG, "[WRITE] This might be CCCD - treating as success!");
                        // Treat as CCCD success
                        gattc_state.cccd_handle = param->write.handle;
                        gattc_state.pairing_complete = true;
                        if (!gattc_state.char_discovered) {
                            gattc_state.char_discovered = true;
                            ESP_LOGI(GATTC_TAG, "[WRITE] ✓✓✓ CCCD write successful (detected)! Notifications should be enabled. ✓✓✓");
                        }
                    }
                }
            }
            
            // If we haven't received CCCD confirmation yet but pairing is complete,
            // try to initialize HID anyway (some devices don't send WRITE_DESCR_EVT)
            if (!gattc_state.char_discovered && gattc_state.pairing_complete && 
                gattc_state.report_char_handle != INVALID_HANDLE) {
                ESP_LOGI(GATTC_TAG, "[HID_INIT] Pairing complete but no CCCD confirmation - initializing HID anyway...");
                // Trigger HID initialization
                if (gattc_state.hid_control_point_handle != INVALID_HANDLE) {
                    uint8_t wake_up[] = {0x01};
                    esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                            gattc_state.hid_control_point_handle,
                                            sizeof(wake_up), wake_up,
                                            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                }
                // Add to polling
                if (gattc_state.polling_char_count < MAX_POLLING_CHARS) {
                    gattc_state.polling_char_handles[gattc_state.polling_char_count] = gattc_state.report_char_handle;
                    gattc_state.polling_char_count++;
                    if (gattc_state.primary_polling_char_handle == INVALID_HANDLE) {
                        gattc_state.primary_polling_char_handle = gattc_state.report_char_handle;
                    }
                }
                gattc_state.char_discovered = true;
            }
            break;
        }
        
        case ESP_GATTC_NOTIFY_EVT: {
            gattc_state.notify_count++;
            ESP_LOGI(GATTC_TAG, "[NOTIFY] ✓✓✓ Notification received #%lu ✓✓✓", gattc_state.notify_count);
            ESP_LOGI(GATTC_TAG, "[NOTIFY] Handle=%d, value_len=%d, is_notify=%d, conn_id=%d",
                     param->notify.handle, param->notify.value_len, param->notify.is_notify, param->notify.conn_id);
            
            // Safety check: ensure we're still connected
            if (!gattc_state.connected) {
                ESP_LOGW(GATTC_TAG, "[NOTIFY] Received notification but not connected - ignoring");
                break;
            }
            
            // Safety check: validate notification data pointer
            if (param->notify.value == NULL) {
                ESP_LOGE(GATTC_TAG, "[NOTIFY] Notification with NULL value pointer - ignoring");
                break;
            }
            
            // Check if this notification is from one of our report characteristics (matching working code)
            bool is_report_char = false;
            for (uint8_t i = 0; i < gattc_state.report_char_count; i++) {
                if (param->notify.handle == gattc_state.report_char_handles[i]) {
                    is_report_char = true;
                    ESP_LOGI(GATTC_TAG, "[NOTIFY] ✓ Notification from Report Characteristic[%d] (handle=%d)", i, param->notify.handle);
                    break;
                }
            }
            // Also check primary report characteristic (for backward compatibility)
            if (!is_report_char && param->notify.handle == gattc_state.report_char_handle) {
                is_report_char = true;
                ESP_LOGI(GATTC_TAG, "[NOTIFY] ✓ Notification from primary Report Characteristic (handle=%d)", param->notify.handle);
            }
            
            if (!is_report_char) {
                ESP_LOGD(GATTC_TAG, "[NOTIFY] Notification from other characteristic (handle=%d) - ignoring", param->notify.handle);
                break;
            }
            
            // Log raw data (matching working trace format: 2 bytes for button press like [B5 00])
            // Limit logging to prevent buffer overflow
            size_t log_len = param->notify.value_len > 16 ? 16 : param->notify.value_len;
            ESP_LOG_BUFFER_HEXDUMP(GATTC_TAG, param->notify.value, log_len, ESP_LOG_INFO);
            
            // Parse button command - working trace shows 2-byte format: [B5 00] for button press, [00 00] for release
            if (param->notify.value_len > 0) {
                ble_button_t button = parse_button_command(param->notify.value, param->notify.value_len);
                
                if (button != BLE_BUTTON_NONE) {
                    gattc_state.button_count++;
                    gattc_state.last_button = button;
                    
                    const char* button_name = ble_media_fob_button_to_string(button);
                    
                    ESP_LOGI(GATTC_TAG, "[BUTTON] ✓✓✓ BUTTON PRESSED #%lu: %s (code=%d) ✓✓✓", 
                            gattc_state.button_count, button_name, button);
                    
                    // Call callback if registered
                    // CRITICAL: Callback runs in BT task context - must be fast and non-blocking
                    // If callback crashes or blocks, it can cause disconnections
                    if (gattc_state.button_callback) {
                        ESP_LOGI(GATTC_TAG, "[BUTTON] Calling button callback...");
                        // Wrap callback in try-catch equivalent (ESP-IDF doesn't have exceptions)
                        // Just ensure callback doesn't crash - if it does, log and continue
                        gattc_state.button_callback(button);
                        ESP_LOGD(GATTC_TAG, "[BUTTON] Button callback completed");
                    } else {
                        ESP_LOGW(GATTC_TAG, "[BUTTON] No callback registered");
                    }
                } else {
                    ESP_LOGD(GATTC_TAG, "[BUTTON] Notification received but no button detected (release event: [00 00]?)");
                }
            } else {
                ESP_LOGW(GATTC_TAG, "[NOTIFY] Notification with no data (value_len=%d)", param->notify.value_len);
            }
            break;
        }
        
        case ESP_GATTC_CFG_MTU_EVT: {
            if (param->cfg_mtu.status == ESP_GATT_OK) {
                ESP_LOGI(GATTC_TAG, "[MTU] MTU configured: %d", param->cfg_mtu.mtu);
            } else {
                ESP_LOGW(GATTC_TAG, "[MTU] MTU configuration failed: %d", param->cfg_mtu.status);
            }
            break;
        }
        
        case ESP_GATTC_EXEC_EVT: {
            ESP_LOGI(GATTC_TAG, "[EXEC] Execute write event - status=%d", param->exec_cmpl.status);
            break;
        }
        
        default:
            // Log all unhandled events to help debug
            if (event == ESP_GATTC_READ_CHAR_EVT || event == ESP_GATTC_READ_DESCR_EVT) {
                ESP_LOGW(GATTC_TAG, "[EVENT] Unhandled READ event: %d - this should not happen!", event);
            } else {
                ESP_LOGD(GATTC_TAG, "[EVENT] Unhandled GATT event: %d", event);
            }
            break;
    }
}

esp_err_t ble_media_fob_init(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[INIT] Starting BLE Media Fob initialization");
    ESP_LOGI(TAG, "========================================");
    
    // Ensure BLE controller is initialized
    esp_bt_controller_status_t bt_status = esp_bt_controller_get_status();
    ESP_LOGI(TAG, "[INIT] BLE controller status: %d", bt_status);
    
    if (bt_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGI(TAG, "[INIT] BLE controller not initialized, initializing...");
        ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_bt_controller_init(&bt_cfg);
        if (ret) {
            ESP_LOGE(TAG, "[INIT] ✗ Controller init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "[INIT] ✓ Controller initialized");
        
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret) {
            ESP_LOGE(TAG, "[INIT] ✗ Controller enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "[INIT] ✓ Controller enabled");
        
        ret = esp_bluedroid_init();
        if (ret) {
            ESP_LOGE(TAG, "[INIT] ✗ Bluedroid init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "[INIT] ✓ Bluedroid initialized");
        
        ret = esp_bluedroid_enable();
        if (ret) {
            ESP_LOGE(TAG, "[INIT] ✗ Bluedroid enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "[INIT] ✓ Bluedroid enabled");
    } else if (bt_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGI(TAG, "[INIT] ✓ BLE controller already enabled");
    } else {
        ESP_LOGW(TAG, "[INIT] ⚠ BLE controller status: %d, may not be ready", bt_status);
    }
    
    esp_err_t ret;
    
    // Read MAC address from config
    ESP_LOGI(TAG, "[INIT] Reading BLE MAC address from config file...");
    ret = read_ble_mac_from_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Could not read BLE MAC from config (error: %s)", esp_err_to_name(ret));
        ESP_LOGE(TAG, "[INIT] BLE Media Fob will not connect without MAC address");
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ MAC address loaded from config");
    
    // Set up security parameters for bonding (MUST be done before connection)
    ESP_LOGI(TAG, "[INIT] Setting up BLE security parameters for pairing/bonding...");
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    
    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Failed to set auth req: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ Authentication requirement set: SC_MITM_BOND");
    
    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Failed to set IO cap: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ IO capability set: NONE");
    
    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Failed to set init key: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ Init key mask set");
    
    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Failed to set rsp key: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ Response key mask set");
    
    ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Failed to set key size: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ Max key size set: %d", key_size);
    
    // Register security callback (MUST be done before connection)
    ESP_LOGI(TAG, "[INIT] Registering security callback...");
    ret = esp_ble_gap_register_callback(gap_security_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] ✗ Failed to register security callback: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ Security callback registered");
    
    // Register GATT client callback
    ESP_LOGI(TAG, "[INIT] Registering GATT client callback...");
    ret = esp_ble_gattc_register_callback(gattc_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "[INIT] ✗ GATT callback registration failed: 0x%x (%s)", ret, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ GATT callback registered");
    
    // Register GATT client app
    ESP_LOGI(TAG, "[INIT] Registering GATT client app (app_idx=%d)...", PROFILE_APP_IDX);
    ret = esp_ble_gattc_app_register(PROFILE_APP_IDX);
    if (ret) {
        ESP_LOGE(TAG, "[INIT] ✗ GATT app registration failed: 0x%x (%s)", ret, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[INIT] ✓ GATT app registered");
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[INIT] ✓✓✓ BLE Media Fob initialized successfully ✓✓✓");
    ESP_LOGI(TAG, "[INIT] Waiting for GATT registration event...");
    ESP_LOGI(TAG, "========================================");
    return ESP_OK;
}

void ble_media_fob_set_button_callback(ble_button_callback_t callback) {
    gattc_state.button_callback = callback;
}

bool ble_media_fob_is_connected(void) {
    return gattc_state.connected;
}

ble_button_t ble_media_fob_get_last_button(void) {
    return gattc_state.last_button;
}

/**
 * @brief Poll a characteristic for data (internal helper)
 */
static void poll_characteristic(uint16_t char_handle) {
    if (char_handle == INVALID_HANDLE) {
        ESP_LOGW(GATTC_TAG, "[POLL] Cannot poll - invalid handle");
        return;
    }
    if (gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(GATTC_TAG, "[POLL] Cannot poll - GATT interface not set");
        return;
    }
    if (gattc_state.conn_id == 0xFFFF) {
        ESP_LOGW(GATTC_TAG, "[POLL] Cannot poll - connection ID invalid");
        return;
    }
    if (!gattc_state.connected) {
        ESP_LOGW(GATTC_TAG, "[POLL] Cannot poll - not connected");
        return;
    }
    if (gattc_state.read_in_progress) {
        ESP_LOGD(GATTC_TAG, "[POLL] Cannot poll - read already in progress");
        return;
    }
    
    ESP_LOGI(GATTC_TAG, "[POLL] Sending read request for handle %d (gattc_if=%d, conn_id=%d)", 
            char_handle, gattc_if, gattc_state.conn_id);
    gattc_state.read_in_progress = true;
    
    // Try with authentication if paired
    esp_gatt_auth_req_t auth_req = gattc_state.pairing_complete ? ESP_GATT_AUTH_REQ_MITM : ESP_GATT_AUTH_REQ_NONE;
    esp_err_t ret = esp_ble_gattc_read_char(gattc_if, gattc_state.conn_id, char_handle, auth_req);
    if (ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "[POLL] ✗ Read request failed for handle %d: %s (0x%x, auth_req=%d)", 
                char_handle, esp_err_to_name(ret), ret, auth_req);
        gattc_state.read_in_progress = false; // Reset on error
    } else {
        ESP_LOGI(GATTC_TAG, "[POLL] ✓ Read request sent successfully for handle %d (auth_req=%d)", 
                char_handle, auth_req);
    }
    // If successful, read_in_progress will be reset in ESP_GATTC_READ_CHAR_EVT
}

void ble_media_fob_update(void) {
    uint32_t now = xTaskGetTickCount();
    
    // 0. Check for connection attempt timeout (before handling new connection attempts)
    // If connection attempt has been in progress for more than 5 seconds without OPEN event, cancel it
    if (gattc_state.connection_attempt_in_progress && !gattc_state.connected) {
        const uint32_t CONNECTION_TIMEOUT_MS = 5000; // 5 seconds timeout
        if (gattc_state.connection_attempt_start_time > 0 && 
            (now - gattc_state.connection_attempt_start_time) > pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS)) {
            ESP_LOGW(GATTC_TAG, "[UPDATE] Connection attempt timeout after %lu ms - device may not be connectable", 
                    CONNECTION_TIMEOUT_MS);
            ESP_LOGW(GATTC_TAG, "[UPDATE] Cancelling connection attempt and rescanning...");
            gattc_state.connection_attempt_in_progress = false;
            gattc_state.connection_attempt_start_time = 0;
            gattc_state.do_scan = true; // Rescan to find device again
            reconnect_delay = 0; // No delay before rescan
        }
    }
    
    // 1. Handle doConnect flag - initiate connection (matches spec section 5.2)
    // CRITICAL: Respect reconnect_delay to prevent spamming connection attempts
    // CRITICAL: Also check connection_attempt_in_progress and conn_id to prevent "wrong state" errors
    if (gattc_state.do_connect && !gattc_state.connected && !gattc_state.connection_attempt_in_progress && 
        gattc_state.conn_id == 0xFFFF && gattc_if != ESP_GATT_IF_NONE) {
        // Check if we should wait before attempting connection
        if (reconnect_delay > 0 && now < reconnect_delay) {
            // Still waiting for reconnect delay
            ESP_LOGD(GATTC_TAG, "[UPDATE] Waiting for reconnect delay (%lu ms remaining)", 
                    (reconnect_delay - now) * portTICK_PERIOD_MS);
            // Don't clear do_connect flag - keep it set so we retry after delay
        } else {
            // Clear reconnect delay if we've waited long enough
            if (reconnect_delay > 0 && now >= reconnect_delay) {
                reconnect_delay = 0;
                ESP_LOGI(GATTC_TAG, "[UPDATE] Reconnect delay expired, attempting connection");
            }
            
            gattc_state.do_connect = false;
            ESP_LOGI(GATTC_TAG, "[UPDATE] Initiating connection...");
            
            if (gattc_state.mac_address_set) {
                // Double-check: Only attempt connection if not already in progress (safety check)
                if (gattc_state.connection_attempt_in_progress) {
                    ESP_LOGW(GATTC_TAG, "[UPDATE] Connection attempt already in progress, skipping (safety check)");
                } else if (gattc_state.conn_id != 0xFFFF) {
                    ESP_LOGW(GATTC_TAG, "[UPDATE] Connection already exists (conn_id=%d), skipping", gattc_state.conn_id);
                } else {
                    ESP_LOGI(GATTC_TAG, "[BLE_LOG] ===== Calling esp_ble_gattc_open =====");
                    ESP_LOGI(GATTC_TAG, "[BLE_LOG] gattc_if=%d", gattc_if);
                    ESP_LOGI(GATTC_TAG, "[BLE_LOG] remote_bda=%02X:%02X:%02X:%02X:%02X:%02X",
                             gattc_state.target_mac[0], gattc_state.target_mac[1],
                             gattc_state.target_mac[2], gattc_state.target_mac[3],
                             gattc_state.target_mac[4], gattc_state.target_mac[5]);
                    ESP_LOGI(GATTC_TAG, "[BLE_LOG] remote_addr_type=%d (BLE_ADDR_TYPE_PUBLIC)", BLE_ADDR_TYPE_PUBLIC);
                    ESP_LOGI(GATTC_TAG, "[BLE_LOG] is_direct=%d (true=direct connection)", true);
                    
                    // Mark connection attempt as in progress
                    gattc_state.connection_attempt_in_progress = true;
                    gattc_state.connection_attempt_start_time = now;
                    
                    esp_err_t ret = esp_ble_gattc_open(gattc_if, gattc_state.target_mac, BLE_ADDR_TYPE_PUBLIC, true);
                    ESP_LOGI(GATTC_TAG, "[BLE_LOG] esp_ble_gattc_open returned: %d (0x%x) = %s", 
                            ret, ret, esp_err_to_name(ret));
                    if (ret != ESP_OK) {
                        ESP_LOGE(GATTC_TAG, "[UPDATE] Connection failed: %s", esp_err_to_name(ret));
                        gattc_state.connection_attempt_in_progress = false;
                        gattc_state.connection_attempt_start_time = 0;
                        gattc_state.do_scan = true;
                        gattc_state.connected = false;
                        // Set reconnect delay to prevent immediate retry
                        reconnect_delay = now + pdMS_TO_TICKS(5000);
                    } else {
                        ESP_LOGI(GATTC_TAG, "[UPDATE] Connection request sent (timeout: 5 seconds)");
                    }
                }
            } else {
                ESP_LOGW(GATTC_TAG, "[UPDATE] Cannot connect - no MAC address available");
                gattc_state.do_scan = true;
            }
        }
    }
    
    // 2. Connection health check (matches spec section 5.3)
    // ESP-IDF doesn't have a direct isConnected() call, but we check conn_id
    // Actual connection status is maintained by event handlers
    
    // 3. Periodic scanning when not connected (matches spec section 5.4)
    if (gattc_state.do_scan && !gattc_state.connected && !gattc_state.scanning) {
        if (gattc_state.last_scan_time == 0 || 
            (now - gattc_state.last_scan_time) > pdMS_TO_TICKS(SCAN_INTERVAL_MS)) {
            ESP_LOGI(GATTC_TAG, "[UPDATE] Scanning for %s...", 
                     gattc_state.device_name_set ? gattc_state.target_device_name : "device");
            
            if (gattc_state.device_name_set) {
                start_ble_scan();
            } else if (gattc_state.mac_address_set) {
                // If we have MAC but not connected, try direct connection
                gattc_state.do_connect = true;
            }
            
            gattc_state.last_scan_time = now;
        }
    }
    
    // 4. Polling characteristics for data (matches spec section 5.5)
    if (gattc_state.connected && gattc_if != ESP_GATT_IF_NONE && 
        gattc_state.conn_id != 0xFFFF) {
        
        // Service discovery is started immediately in ESP_GATTC_OPEN_EVT (matching working code)
        // If it failed due to invalid conn_id, retry here when conn_id becomes available
        if (gattc_state.connected && !gattc_state.service_discovered && 
            gattc_state.conn_id != 0xFFFF && gattc_if != ESP_GATT_IF_NONE) {
            // Retry service discovery if it wasn't started in OPEN_EVT
            static uint32_t last_retry_time = 0;
            if (last_retry_time == 0 || (now - last_retry_time) > pdMS_TO_TICKS(1000)) {
                ESP_LOGI(GATTC_TAG, "[UPDATE] Retrying service discovery (conn_id=%d)", gattc_state.conn_id);
                esp_err_t ret = esp_ble_gattc_search_service(gattc_if, gattc_state.conn_id, &hid_service_uuid);
                if (ret != ESP_OK) {
                    ESP_LOGE(GATTC_TAG, "[UPDATE] Service discovery retry failed: %s", esp_err_to_name(ret));
                    ret = esp_ble_gattc_search_service(gattc_if, gattc_state.conn_id, &serial_service_uuid);
                    if (ret != ESP_OK) {
                        ESP_LOGE(GATTC_TAG, "[UPDATE] Serial service search also failed: %s", esp_err_to_name(ret));
                    } else {
                        ESP_LOGI(GATTC_TAG, "[UPDATE] Serial service search initiated");
                    }
                } else {
                    ESP_LOGI(GATTC_TAG, "[UPDATE] HID service search initiated");
                }
                last_retry_time = now;
            }
        }
        
        // Timeout: If pairing is complete but we haven't initialized HID after 2 seconds,
        // initialize it anyway (some devices don't send WRITE_DESCR_EVT)
        static uint32_t pairing_complete_time = 0;
        if (gattc_state.pairing_complete && gattc_state.report_char_handle != INVALID_HANDLE && 
            !gattc_state.char_discovered) {
            if (pairing_complete_time == 0) {
                pairing_complete_time = now;
                ESP_LOGI(GATTC_TAG, "[UPDATE] Waiting for CCCD confirmation, will timeout in 2 seconds...");
            } else if ((now - pairing_complete_time) > pdMS_TO_TICKS(2000)) {
                ESP_LOGI(GATTC_TAG, "[UPDATE] Timeout: Initializing HID without CCCD confirmation...");
                // Add to polling if not already there
                if (gattc_state.polling_char_count < MAX_POLLING_CHARS) {
                    bool already_in_list = false;
                    for (uint8_t i = 0; i < gattc_state.polling_char_count; i++) {
                        if (gattc_state.polling_char_handles[i] == gattc_state.report_char_handle) {
                            already_in_list = true;
                            break;
                        }
                    }
                    if (!already_in_list) {
                        gattc_state.polling_char_handles[gattc_state.polling_char_count] = gattc_state.report_char_handle;
                        gattc_state.polling_char_count++;
                        if (gattc_state.primary_polling_char_handle == INVALID_HANDLE) {
                            gattc_state.primary_polling_char_handle = gattc_state.report_char_handle;
                        }
                        ESP_LOGI(GATTC_TAG, "[UPDATE] ✓ Added report characteristic to polling list (count=%d)", 
                                gattc_state.polling_char_count);
                    }
                }
                // Initialize HID device (matching working code order)
                // 1. Write to HID Control Point to wake device
                if (gattc_state.hid_control_point_handle != INVALID_HANDLE) {
                    ESP_LOGI(GATTC_TAG, "[HID_INIT] Writing to HID Control Point (handle=%d) to wake device...",
                            gattc_state.hid_control_point_handle);
                    uint8_t wake_up[] = {0x01};
                    esp_err_t ret_cp3 = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                            gattc_state.hid_control_point_handle,
                                            sizeof(wake_up), wake_up,
                                            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                    if (ret_cp3 == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ HID Control Point write request sent");
                    } else {
                        ESP_LOGW(GATTC_TAG, "[HID_INIT] HID Control Point write failed: %s", esp_err_to_name(ret_cp3));
                    }
                    vTaskDelay(pdMS_TO_TICKS(100)); // Give device time to process (matching working code)
                } else {
                    ESP_LOGW(GATTC_TAG, "[HID_INIT] HID Control Point handle not available");
                }
                
                // 2. Try to read Report Reference descriptor (0x2908)
                ESP_LOGI(GATTC_TAG, "[HID_INIT] Attempting to read Report Reference descriptor...");
                uint16_t report_ref_handle = gattc_state.report_char_handle + 2; // After CCCD
                esp_err_t ret_desc2 = esp_ble_gattc_read_char_descr(gattc_if, gattc_state.conn_id, report_ref_handle, ESP_GATT_AUTH_REQ_NONE);
                if (ret_desc2 == ESP_OK) {
                    ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ Report Reference descriptor read request sent");
                } else {
                    ESP_LOGD(GATTC_TAG, "[HID_INIT] Report Reference descriptor read failed: %s (will continue)", 
                            esp_err_to_name(ret_desc2));
                }
                
                // 3. Try writing report IDs 2, 3, 4
                ESP_LOGI(GATTC_TAG, "[HID_INIT] Writing report IDs 2, 3, 4 to initialize device...");
                for (uint8_t report_id = 2; report_id <= 4; report_id++) {
                    uint8_t report_id_bytes[] = {report_id};
                    esp_err_t ret_rid2 = esp_ble_gattc_write_char(gattc_if, gattc_state.conn_id,
                                                          gattc_state.report_char_handle,
                                                          sizeof(report_id_bytes), report_id_bytes,
                                                          ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
                    if (ret_rid2 == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[HID_INIT] ✓ Report ID %d write request sent", report_id);
                    } else {
                        ESP_LOGD(GATTC_TAG, "[HID_INIT] Report ID %d write failed: %s", report_id, esp_err_to_name(ret_rid2));
                    }
                    vTaskDelay(pdMS_TO_TICKS(50)); // Small delay between writes (matching working code)
                }
                
                gattc_state.char_discovered = true;
                pairing_complete_time = 0; // Reset
                ESP_LOGI(GATTC_TAG, "[UPDATE] ✓✓✓ HID initialization complete! Ready to receive button presses. ✓✓✓");
                
                // Re-register for notifications after HID initialization (matching working code - line 308)
                // This is critical - the working code does this AFTER all initialization
                if (gattc_state.report_char_handle != INVALID_HANDLE) {
                    ESP_LOGI(GATTC_TAG, "[UPDATE] Re-registering for notifications after HID init (matching working code)...");
                    esp_err_t ret_reg = esp_ble_gattc_register_for_notify(gattc_if, gattc_state.remote_bda, gattc_state.report_char_handle);
                    if (ret_reg == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[UPDATE] ✓ Re-registration request sent");
                    } else {
                        ESP_LOGW(GATTC_TAG, "[UPDATE] Re-registration failed: %s", esp_err_to_name(ret_reg));
                    }
                    
                    // Also try to re-write CCCD after a short delay
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ESP_LOGI(GATTC_TAG, "[UPDATE] Attempting to re-write CCCD after HID init...");
                    uint16_t cccd_handle = gattc_state.report_char_handle + 1;
                    uint8_t notify_data[2] = {0x01, 0x00}; // Enable notifications
                    esp_err_t ret_cccd = esp_ble_gattc_write_char_descr(gattc_if, gattc_state.conn_id,
                                                         cccd_handle,
                                                         sizeof(notify_data), notify_data,
                                                         ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                    if (ret_cccd == ESP_OK) {
                        ESP_LOGI(GATTC_TAG, "[UPDATE] ✓ CCCD re-write request sent (handle=%d)", cccd_handle);
                    } else {
                        ESP_LOGW(GATTC_TAG, "[UPDATE] CCCD re-write failed: %s", esp_err_to_name(ret_cccd));
                    }
                }
            }
        } else if (gattc_state.char_discovered) {
            pairing_complete_time = 0; // Reset if already discovered
        }
        
        // Poll one characteristic at a time to avoid queue overflow
        // Rotate through polling list with longer interval
        if (gattc_state.char_discovered && gattc_state.polling_char_count > 0) {
            if (gattc_state.last_poll_time == 0 || 
                (now - gattc_state.last_poll_time) > pdMS_TO_TICKS(POLL_INTERVAL_MS)) {
                
                // Only poll if no read is in progress
                if (!gattc_state.read_in_progress) {
                    // Poll one characteristic at a time, rotating through the list
                    uint16_t char_handle = gattc_state.polling_char_handles[gattc_state.current_poll_index];
                    if (char_handle != INVALID_HANDLE) {
                        ESP_LOGI(GATTC_TAG, "[POLL] Polling characteristic handle %d (index %d/%d)",
                                char_handle, gattc_state.current_poll_index, gattc_state.polling_char_count);
                        poll_characteristic(char_handle);
                    } else {
                        ESP_LOGW(GATTC_TAG, "[POLL] Invalid handle at index %d", gattc_state.current_poll_index);
                    }
                    
                    // Move to next characteristic in list (round-robin)
                    gattc_state.current_poll_index++;
                    if (gattc_state.current_poll_index >= gattc_state.polling_char_count) {
                        gattc_state.current_poll_index = 0;
                    }
                    
                    gattc_state.last_poll_time = now;
                } else {
                    ESP_LOGD(GATTC_TAG, "[POLL] Read in progress, skipping poll");
                }
            }
        }
    }
    
    // Periodic status log (every 10 seconds)
    static uint32_t last_status_log = 0;
    if (now - last_status_log > pdMS_TO_TICKS(10000)) {
        last_status_log = now;
        ESP_LOGI(GATTC_TAG, "[STATUS] Connected=%d, Service=%d, Char=%d, Buttons=%lu, Notifies=%lu, Polling=%d",
                 gattc_state.connected, gattc_state.service_discovered, gattc_state.char_discovered,
                 gattc_state.button_count, gattc_state.notify_count, gattc_state.polling_char_count);
    }
}

void ble_media_fob_deinit(void) {
    ESP_LOGI(TAG, "[DEINIT] Deinitializing BLE Media Fob...");
    if (gattc_state.connected) {
        ESP_LOGI(TAG, "[DEINIT] Closing connection...");
        esp_ble_gattc_close(gattc_if, gattc_state.conn_id);
    }
    ESP_LOGI(TAG, "[DEINIT] Unregistering GATT app...");
    esp_ble_gattc_app_unregister(gattc_if);
    memset(&gattc_state, 0, sizeof(gattc_state));
    ESP_LOGI(TAG, "[DEINIT] ✓ BLE Media Fob deinitialized");
}

void ble_media_fob_print_state(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[STATE] BLE Media Fob Current State:");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[STATE] MAC Address Set: %s", gattc_state.mac_address_set ? "YES" : "NO");
    if (gattc_state.mac_address_set) {
        ESP_LOGI(TAG, "[STATE] Target MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 gattc_state.target_mac[0], gattc_state.target_mac[1],
                 gattc_state.target_mac[2], gattc_state.target_mac[3],
                 gattc_state.target_mac[4], gattc_state.target_mac[5]);
    }
    ESP_LOGI(TAG, "[STATE] GATT Interface: %d", gattc_if);
    ESP_LOGI(TAG, "[STATE] Connected: %s", gattc_state.connected ? "YES" : "NO");
    ESP_LOGI(TAG, "[STATE] Connection ID: %d", gattc_state.conn_id);
    if (gattc_state.connected) {
        ESP_LOGI(TAG, "[STATE] Remote MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 gattc_state.remote_bda[0], gattc_state.remote_bda[1],
                 gattc_state.remote_bda[2], gattc_state.remote_bda[3],
                 gattc_state.remote_bda[4], gattc_state.remote_bda[5]);
    }
    ESP_LOGI(TAG, "[STATE] Service Discovered: %s", gattc_state.service_discovered ? "YES" : "NO");
    ESP_LOGI(TAG, "[STATE] HID Service Handle: %d", gattc_state.hid_service_handle);
    ESP_LOGI(TAG, "[STATE] Characteristic Discovered: %s", gattc_state.char_discovered ? "YES" : "NO");
    ESP_LOGI(TAG, "[STATE] Report Char Handle (primary): %d", gattc_state.report_char_handle);
    ESP_LOGI(TAG, "[STATE] Report Characteristics Count: %d", gattc_state.report_char_count);
    if (gattc_state.report_char_count > 0) {
        ESP_LOGI(TAG, "[STATE] Report Characteristics: ");
        for (uint8_t i = 0; i < gattc_state.report_char_count; i++) {
            ESP_LOGI(TAG, "[STATE]   [%d] Handle: %d", i, gattc_state.report_char_handles[i]);
        }
    }
    ESP_LOGI(TAG, "[STATE] CCCD Handle: %d", gattc_state.cccd_handle);
    ESP_LOGI(TAG, "[STATE] Button Callback: %s", gattc_state.button_callback ? "SET" : "NULL");
    ESP_LOGI(TAG, "[STATE] Last Button: %d", gattc_state.last_button);
    ESP_LOGI(TAG, "[STATE] Total Buttons Received: %lu", gattc_state.button_count);
    ESP_LOGI(TAG, "[STATE] Total Notifications: %lu", gattc_state.notify_count);
    ESP_LOGI(TAG, "========================================");
}

/**
 * @brief Initialize BLE with target device name and/or MAC (matches BluetoothManager::begin)
 */
bool ble_media_fob_begin(const char* targetDeviceName, const char* macAddress) {
    ESP_LOGI(TAG, "[BEGIN] Initializing BLE Media Fob with target: name=%s, mac=%s",
             targetDeviceName ? targetDeviceName : "NULL",
             macAddress ? macAddress : "NULL");
    
    // Initialize BLE stack if not already done
    esp_bt_controller_status_t bt_status = esp_bt_controller_get_status();
    if (bt_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGI(TAG, "[BEGIN] Initializing BLE controller...");
        ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_bt_controller_init(&bt_cfg);
        if (ret) {
            ESP_LOGE(TAG, "[BEGIN] Controller init failed: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret) {
            ESP_LOGE(TAG, "[BEGIN] Controller enable failed: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_bluedroid_init();
        if (ret) {
            ESP_LOGE(TAG, "[BEGIN] Bluedroid init failed: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_bluedroid_enable();
        if (ret) {
            ESP_LOGE(TAG, "[BEGIN] Bluedroid enable failed: %s", esp_err_to_name(ret));
            return false;
        }
    }
    
    // Store target device name
    if (targetDeviceName && strlen(targetDeviceName) > 0) {
        strncpy(gattc_state.target_device_name, targetDeviceName, sizeof(gattc_state.target_device_name) - 1);
        gattc_state.target_device_name[sizeof(gattc_state.target_device_name) - 1] = '\0';
        gattc_state.device_name_set = true;
        ESP_LOGI(TAG, "[BEGIN] Target device name: %s", gattc_state.target_device_name);
    } else {
        gattc_state.device_name_set = false;
    }
    
    // Store MAC address if provided
    if (macAddress && strlen(macAddress) > 0) {
        if (parse_mac_address(macAddress, gattc_state.target_mac)) {
            gattc_state.mac_address_set = true;
            ESP_LOGI(TAG, "[BEGIN] Target MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     gattc_state.target_mac[0], gattc_state.target_mac[1],
                     gattc_state.target_mac[2], gattc_state.target_mac[3],
                     gattc_state.target_mac[4], gattc_state.target_mac[5]);
        } else {
            ESP_LOGE(TAG, "[BEGIN] Invalid MAC address format: %s", macAddress);
            return false;
        }
    } else {
        gattc_state.mac_address_set = false;
    }
    
    if (!gattc_state.device_name_set && !gattc_state.mac_address_set) {
        ESP_LOGE(TAG, "[BEGIN] Either device name or MAC address must be provided");
        return false;
    }
    
    // Register GATT client callback
    esp_err_t ret = esp_ble_gattc_register_callback(gattc_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "[BEGIN] GATT callback registration failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Register GATT client app
    ret = esp_ble_gattc_app_register(PROFILE_APP_IDX);
    if (ret) {
        ESP_LOGE(TAG, "[BEGIN] GATT app registration failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Set scan parameters (matching spec: interval=1349, window=449, active scan)
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x0549,  // ~1349 (0x0549 = 1353, close enough)
        .scan_window = 0x01C1,    // ~449 (0x01C1 = 449)
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[BEGIN] Failed to set scan params: %s", esp_err_to_name(ret));
    }
    
    // Start initial scan
    gattc_state.do_scan = true;
    gattc_state.last_scan_time = 0;
    
    ESP_LOGI(TAG, "[BEGIN] ✓ BLE Media Fob initialized, scanning will start...");
    return true;
}

/**
 * @brief Get connected device name
 */
const char* ble_media_fob_get_connected_device_name(void) {
    if (gattc_state.connected && strlen(gattc_state.connected_device_name) > 0) {
        return gattc_state.connected_device_name;
    }
    return "";
}

/**
 * @brief Get connected device MAC address
 */
bool ble_media_fob_get_connected_device_mac(char* mac_str) {
    if (!mac_str || !gattc_state.connected) {
        return false;
    }
    snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             gattc_state.remote_bda[0], gattc_state.remote_bda[1],
             gattc_state.remote_bda[2], gattc_state.remote_bda[3],
             gattc_state.remote_bda[4], gattc_state.remote_bda[5]);
    return true;
}

/**
 * @brief Convert button enum to string
 */
const char* ble_media_fob_button_to_string(ble_button_t button) {
    const char* button_names[] = {"NONE", "UP", "DOWN", "LEFT", "RIGHT", "OK"};
    if (button < 6) {
        return button_names[button];
    }
    return "UNKNOWN";
}

