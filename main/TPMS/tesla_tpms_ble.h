#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

#ifndef ENABLE_TESLA_TPMS_DEBUG
#define ENABLE_TESLA_TPMS_DEBUG 0
#endif

void tesla_tpms_init(void);
void tesla_tpms_start(void);
void tesla_tpms_stop(void);
bool tesla_tpms_is_running(void);

/** Called from Wireless.c GAP callback (no-op when feature disabled). */
void tesla_tpms_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/** Called from Wireless.c GATTC callback; returns true if event was consumed. */
bool tesla_tpms_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param);
