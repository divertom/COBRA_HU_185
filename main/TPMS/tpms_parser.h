#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TPMS_PARSE_STATUS_REST = 0,
    TPMS_PARSE_STARTUP_INFO,
    TPMS_PARSE_UNKNOWN,
    TPMS_PARSE_STABLE_OK,
    TPMS_PARSE_ACTIVE_OK,
    TPMS_PARSE_TELEMETRY_REJECTED,
} tpms_parse_result_t;

typedef struct {
    uint32_t pressure_kpa;
    uint32_t temp_raw;
    int32_t temp_c;
    float temp_f;
    float pressure_psi;
} tpms_telemetry_t;

uint32_t decode_protobuf_varint(const uint8_t *data, size_t len, size_t *offset);

tpms_parse_result_t tpms_parser_parse(const uint8_t *data, size_t len, tpms_telemetry_t *out);
const tpms_telemetry_t *tpms_parser_last_valid(void);
