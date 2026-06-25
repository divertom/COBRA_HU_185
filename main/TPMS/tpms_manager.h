#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "tpms_parser.h"

#define TPMS_MANAGER_MAX_SENSORS 8

#define TPMS_STATUS_LIVE_MS     (120000U)
#define TPMS_STATUS_RECENT_MS   (18000000U)

typedef enum {
    TPMS_TIRE_NO = 0,
    TPMS_TIRE_LF,
    TPMS_TIRE_RF,
    TPMS_TIRE_LR,
    TPMS_TIRE_RR,
    TPMS_TIRE_COUNT
} tpms_tire_position_t;

typedef enum {
    TPMS_SENSOR_STATUS_STALE = 0,
    TPMS_SENSOR_STATUS_RECENT,
    TPMS_SENSOR_STATUS_LIVE,
} tpms_sensor_status_t;

typedef enum {
    TPMS_TELEMETRY_NONE = 0,
    TPMS_TELEMETRY_STABLE,
    TPMS_TELEMETRY_ACTIVE,
} tpms_telemetry_state_t;

typedef struct {
    char temp_unit[2];
    char pressure_unit[8];
} tpms_units_t;

typedef struct {
    char mac[18];
    char display_name[16];
    bool is_gateway;
    bool is_primary_gatt;
    /** True while round-robin GATT is connected to this MAC. */
    bool is_gatt_active;
    tpms_tire_position_t position;
    tpms_sensor_status_t status;
    tpms_telemetry_state_t telemetry_state;
    bool has_telemetry;
    float pressure;
    float temperature;
    tpms_units_t units;
} tpms_sensor_snapshot_t;

esp_err_t tpms_manager_init(void);

void tpms_manager_on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/** Feed GATT indication telemetry from the tsTPMS gateway into the matching sensor slot. */
void tpms_manager_on_gateway_telemetry(const uint8_t bda[6], tpms_parse_result_t result,
                                       const tpms_telemetry_t *telem);

void tpms_manager_ensure_gateway_link(void);

/** Queue gateway GATT work on the TPMS worker task (safe from timer/GATT callbacks). */
void tpms_manager_request_gateway_link(void);

/** Called when the tsTPMS GATT client disconnects (advances rotation or retries). */
void tpms_manager_on_gatt_disconnect(void);

/** Start round-robin GATT reads across known tsTPMS modules (after portal scan stops). */
void tpms_manager_start_rotation(void);
void tpms_manager_stop_rotation(void);

bool tpms_manager_is_scan_active(void);
/** True while round-robin GATT reads cycle through known tsTPMS modules (after scan stop). */
bool tpms_manager_is_rotation_active(void);
/** True while discovering sensors or any known sensor needs BLE scan updates. */
bool tpms_manager_ble_scan_required(void);
esp_err_t tpms_manager_set_scan_active(bool active);

esp_err_t tpms_manager_forget(const char *mac);
esp_err_t tpms_manager_set_position(const char *mac, tpms_tire_position_t position);
esp_err_t tpms_manager_set_primary_gatt(const char *mac);
esp_err_t tpms_manager_get_primary_gatt_mac(char *mac_out, size_t mac_out_len);

esp_err_t tpms_manager_get_units(tpms_units_t *out);
esp_err_t tpms_manager_set_units(const tpms_units_t *units);

int tpms_manager_sensor_count(void);
esp_err_t tpms_manager_get_snapshot(int index, tpms_sensor_snapshot_t *out);

const char *tpms_position_to_string(tpms_tire_position_t pos);
bool tpms_position_from_string(const char *str, tpms_tire_position_t *out);

bool tpms_manager_format_mac(const uint8_t bda[6], char *out, size_t out_len);
bool tpms_manager_parse_mac(const char *mac_str, uint8_t bda[6]);
