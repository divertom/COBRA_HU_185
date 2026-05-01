#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stdbool.h>

// Button types matching the working project
typedef enum {
    BLE_BUTTON_NONE = 0,
    BLE_BUTTON_UP,
    BLE_BUTTON_DOWN,
    BLE_BUTTON_LEFT,
    BLE_BUTTON_RIGHT,
    BLE_BUTTON_OK
} ble_button_t;

// Callback function type for button presses
typedef void (*ble_button_callback_t)(ble_button_t button);

/**
 * @brief Initialize BLE Media Fob connection (matches BluetoothManager::begin)
 * 
 * @param targetDeviceName Device name to search for (can be NULL if using MAC)
 * @param macAddress MAC address string (can be NULL if using device name)
 * @return true on success, false otherwise
 */
bool ble_media_fob_begin(const char* targetDeviceName, const char* macAddress);

/**
 * @brief Initialize BLE Media Fob connection (legacy - reads from config file)
 * 
 * Reads MAC address from config file and connects to the device
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_media_fob_init(void);

/**
 * @brief Set callback function for button presses
 * 
 * @param callback Function to call when a button is pressed
 */
void ble_media_fob_set_button_callback(ble_button_callback_t callback);

/**
 * @brief Check if BLE device is connected
 * 
 * @return true if connected, false otherwise
 */
bool ble_media_fob_is_connected(void);

/**
 * @brief Get the last received button press
 * 
 * @return Last button pressed (BLE_BUTTON_NONE if none)
 */
ble_button_t ble_media_fob_get_last_button(void);

/**
 * @brief Process BLE events (call this periodically)
 */
void ble_media_fob_update(void);

/**
 * @brief Deinitialize BLE Media Fob
 */
void ble_media_fob_deinit(void);

/**
 * @brief Print current BLE state for debugging
 */
void ble_media_fob_print_state(void);

/**
 * @brief Get connected device name (matches BluetoothManager::getConnectedDeviceName)
 * 
 * @return Connected device name string, or empty string if not connected
 */
const char* ble_media_fob_get_connected_device_name(void);

/**
 * @brief Get connected device MAC address (matches BluetoothManager::getConnectedDeviceMAC)
 * 
 * @param mac_str Buffer to store MAC address string (must be at least 18 bytes)
 * @return true if connected and MAC retrieved, false otherwise
 */
bool ble_media_fob_get_connected_device_mac(char* mac_str);

/**
 * @brief Convert button enum to string (matches BluetoothManager::buttonToString)
 * 
 * @param button Button enum value
 * @return String representation of button
 */
const char* ble_media_fob_button_to_string(ble_button_t button);

