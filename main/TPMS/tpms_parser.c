#include "tpms_parser.h"

#include <string.h>

/*
 * Tesla TPMS BLE indication payloads (reverse-engineered).
 * Startup and status/rest packets are logged but must never overwrite
 * the last valid pressure/temperature reading.
 */

static const int TPMS_TEMP_OFFSET_C = 27;
static const int TPMS_MIN_VALID_KPA = 50;
static const int TPMS_MAX_VALID_KPA = 450;
static const int TPMS_MIN_VALID_TEMP_C = -40;
static const int TPMS_MAX_VALID_TEMP_C = 125;

static const uint8_t TPMS_MARKER_STARTUP[] = {0xEA, 0x01};
static const uint8_t TPMS_MARKER_STATUS[] = {0x92, 0x02, 0x07};
static const uint8_t TPMS_MARKER_STABLE[] = {0xE2, 0x01, 0x04};
static const uint8_t TPMS_MARKER_ACTIVE[] = {0xE2, 0x01, 0x05};

static tpms_telemetry_t s_last_valid;
static bool s_last_valid_set = false;

static bool marker_at(const uint8_t *data, size_t len, size_t offset,
                      const uint8_t *marker, size_t marker_len)
{
    if (data == NULL || marker == NULL || offset + marker_len > len) {
        return false;
    }
    return memcmp(data + offset, marker, marker_len) == 0;
}

uint32_t decode_protobuf_varint(const uint8_t *data, size_t len, size_t *offset)
{
    if (data == NULL || offset == NULL || *offset >= len) {
        return 0;
    }

    size_t start = *offset;
    uint32_t result = 0;
    uint32_t shift = 0;
    size_t i = *offset;

    while (i < len) {
        uint8_t byte = data[i++];
        result |= ((uint32_t)(byte & 0x7Fu) << shift);
        if ((byte & 0x80u) == 0) {
            *offset = i;
            return result;
        }
        shift += 7;
        if (shift > 28) {
            *offset = start;
            return 0;
        }
    }

    *offset = start;
    return 0;
}

static bool telemetry_sane(const tpms_telemetry_t *t)
{
    return t->pressure_kpa >= (uint32_t)TPMS_MIN_VALID_KPA &&
           t->pressure_kpa <= (uint32_t)TPMS_MAX_VALID_KPA &&
           t->temp_c >= TPMS_MIN_VALID_TEMP_C &&
           t->temp_c <= TPMS_MAX_VALID_TEMP_C;
}

static tpms_telemetry_t make_telemetry(uint32_t pressure_kpa, uint32_t temp_raw)
{
    int32_t temp_c = (int32_t)temp_raw - TPMS_TEMP_OFFSET_C;
    tpms_telemetry_t t = {
        .pressure_kpa = pressure_kpa,
        .temp_raw = temp_raw,
        .temp_c = temp_c,
        .temp_f = ((float)temp_c * 9.0f / 5.0f) + 32.0f,
        .pressure_psi = (float)pressure_kpa * 0.1450377f,
    };
    return t;
}

static tpms_parse_result_t parse_telemetry(const uint8_t *data, size_t len, size_t marker_len,
                                           bool pressure_is_varint, tpms_telemetry_t *out)
{
    size_t idx = 2 + marker_len;

    if (idx >= len || data[idx] != 0x08) {
        return TPMS_PARSE_UNKNOWN;
    }
    idx++;

    uint32_t pressure_kpa = 0;
    if (pressure_is_varint) {
        size_t before = idx;
        pressure_kpa = decode_protobuf_varint(data, len, &idx);
        if (idx == before) {
            return TPMS_PARSE_UNKNOWN;
        }
    } else {
        if (idx >= len) {
            return TPMS_PARSE_UNKNOWN;
        }
        pressure_kpa = data[idx++];
    }

    if (idx >= len || data[idx] != 0x10) {
        return TPMS_PARSE_UNKNOWN;
    }
    idx++;

    if (idx >= len) {
        return TPMS_PARSE_UNKNOWN;
    }
    uint32_t temp_raw = data[idx++];

    tpms_telemetry_t candidate = make_telemetry(pressure_kpa, temp_raw);

    if (!telemetry_sane(&candidate)) {
        return TPMS_PARSE_TELEMETRY_REJECTED;
    }

    s_last_valid = candidate;
    s_last_valid_set = true;

    if (out != NULL) {
        *out = candidate;
    }

    return pressure_is_varint ? TPMS_PARSE_ACTIVE_OK : TPMS_PARSE_STABLE_OK;
}

const tpms_telemetry_t *tpms_parser_last_valid(void)
{
    return s_last_valid_set ? &s_last_valid : NULL;
}

tpms_parse_result_t tpms_parser_parse(const uint8_t *data, size_t len, tpms_telemetry_t *out)
{
    if (data == NULL || len < 5 || data[0] != 0x12) {
        return TPMS_PARSE_UNKNOWN;
    }

    uint8_t outer_len = data[1];
    if ((size_t)(2 + outer_len) > len) {
        return TPMS_PARSE_UNKNOWN;
    }

    if (marker_at(data, len, 2, TPMS_MARKER_STARTUP, sizeof(TPMS_MARKER_STARTUP))) {
        return TPMS_PARSE_STARTUP_INFO;
    }

    if (marker_at(data, len, 2, TPMS_MARKER_STATUS, sizeof(TPMS_MARKER_STATUS))) {
        return TPMS_PARSE_STATUS_REST;
    }

    if (marker_at(data, len, 2, TPMS_MARKER_STABLE, sizeof(TPMS_MARKER_STABLE))) {
        return parse_telemetry(data, len, sizeof(TPMS_MARKER_STABLE), false, out);
    }

    if (marker_at(data, len, 2, TPMS_MARKER_ACTIVE, sizeof(TPMS_MARKER_ACTIVE))) {
        return parse_telemetry(data, len, sizeof(TPMS_MARKER_ACTIVE), true, out);
    }

    return TPMS_PARSE_UNKNOWN;
}
