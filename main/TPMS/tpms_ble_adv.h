#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gap_ble_api.h"

/** True if advertisement is the tsTPMS BLE gateway (name, 16-bit UUID 0x1122, or SiLabs mfg). */
bool tpms_ble_adv_is_gateway(const esp_ble_gap_cb_param_t *param);

/** True if advertisement looks like a Tesla TPMS wheel sensor (not the gateway). */
bool tpms_ble_adv_is_wheel_sensor(const esp_ble_gap_cb_param_t *param);

/** Gateway or wheel sensor — used during portal discovery scan. */
bool tpms_ble_adv_is_discovery_candidate(const esp_ble_gap_cb_param_t *param);

/** Try to parse TPMS telemetry from combined adv + scan response; returns true if parsed. */
bool tpms_ble_adv_try_parse_telemetry(const uint8_t *adv, uint8_t adv_len,
                                      const uint8_t *scan_rsp, uint8_t scan_rsp_len,
                                      uint8_t *out_payload, size_t out_cap, size_t *out_len);
