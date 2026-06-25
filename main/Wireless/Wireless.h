#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "nvs_flash.h" 
#include "esp_log.h"

#include <stdio.h>
#include <string.h>  // For memcpy
#include "esp_system.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_gattc_api.h"

typedef enum {
    BT_REMOTE_EVENT_UNKNOWN = 0,
    BT_REMOTE_EVENT_VOL_UP,
    BT_REMOTE_EVENT_VOL_DOWN,
    BT_REMOTE_EVENT_NEXT_TRACK,
    BT_REMOTE_EVENT_PREV_TRACK,
    BT_REMOTE_EVENT_PLAY_PAUSE
} bt_remote_event_t;

typedef void (*bt_remote_event_handler_t)(bt_remote_event_t event);

typedef enum {
    BT_REMOTE_CONN_DISCONNECTED = 0,
    BT_REMOTE_CONN_CONNECTING,
    BT_REMOTE_CONN_CONNECTED
} bt_remote_conn_state_t;

extern uint16_t BLE_NUM;
extern uint16_t WIFI_NUM;
extern bool Scan_finish;

void Wireless_Init(void);
void WIFI_Init(void *arg);
uint16_t WIFI_Scan(void);
void BLE_Init(void *arg);
uint16_t BLE_Scan(void);
void Wireless_LogRemoteEvent(uint16_t usage, bt_remote_event_t event);
void Wireless_DecodeHidReport(const uint8_t *report_data, uint16_t report_len);
void Wireless_RegisterRemoteEventHandler(bt_remote_event_handler_t handler);

/** Smart Remote BLE link (GATT central in Wireless.c); safe to call from UI thread for display-only. */
bt_remote_conn_state_t Wireless_GetRemoteConnectionState(void);
void Wireless_GetRemoteDisplayName(char *out, size_t out_len);
/** Formats BLE address when connecting/connected or target chosen; sets "---" otherwise. Returns true once a BLE address has been copied to out. */
bool Wireless_FormatRemoteMac(char *out, size_t out_len);
/** Percent 0–100; returns false when unknown / no BLE Battery Service level. */
bool Wireless_GetRemoteBatteryPercent(uint8_t *out_percent);

/** Start BLE scan if not already running. Safe for TPMS to call. */
void Wireless_EnsureBleScanActive(void);

/** Stop and restart BLE scan (use when starting TPMS discovery). */
void Wireless_RestartBleScanForTpms(void);

/** True while the SmartRemote GAP scan is running (shared radio). */
bool Wireless_IsBleScanActive(void);

/** Service portal SoftAP SSID from device_config service_portal.ap_ssid (legacy: wifi.ssid). */
void Wireless_GetApSsid(char *out, size_t out_len);
