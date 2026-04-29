#pragma once

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