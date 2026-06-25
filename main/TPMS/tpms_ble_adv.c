#include "tpms_ble_adv.h"

#include "tpms_parser.h"

#include <string.h>

#include "esp_gap_ble_api.h"
#include "esp_log.h"

static const char *TAG = "TPMS_ADV";

static const uint8_t s_tpms_service_uuid128[ESP_UUID_LEN_128] = {
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00,
};

static const uint8_t s_tpms_marker_startup[] = {0xEA, 0x01};
static const uint8_t s_tpms_marker_status[] = {0x92, 0x02, 0x07};
static const uint8_t s_tpms_marker_stable[] = {0xE2, 0x01, 0x04};
static const uint8_t s_tpms_marker_active[] = {0xE2, 0x01, 0x05};

static const char s_tpms_gateway_name[] = "tsTPMS";

static bool tpms_name_is_gateway(const uint8_t *name, uint8_t name_len)
{
    if (name == NULL || name_len == 0) {
        return false;
    }
    size_t want = strlen(s_tpms_gateway_name);
    if ((size_t)name_len < want) {
        return false;
    }
    for (size_t i = 0; i < want; i++) {
        char a = (char)name[i];
        char b = s_tpms_gateway_name[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool tpms_slice_parseable(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 5) {
        return false;
    }
    tpms_telemetry_t telem;
    tpms_parse_result_t r = tpms_parser_parse(data, len, &telem);
    return r == TPMS_PARSE_STABLE_OK || r == TPMS_PARSE_ACTIVE_OK ||
           r == TPMS_PARSE_STARTUP_INFO || r == TPMS_PARSE_STATUS_REST;
}

static bool tpms_buf_has_marker(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 2) {
        return false;
    }
    for (size_t i = 0; i + 2 <= len; i++) {
        if (memcmp(data + i, s_tpms_marker_startup, 2) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i + 3 <= len; i++) {
        if (memcmp(data + i, s_tpms_marker_status, 3) == 0) {
            return true;
        }
        if (memcmp(data + i, s_tpms_marker_stable, 3) == 0) {
            return true;
        }
        if (memcmp(data + i, s_tpms_marker_active, 3) == 0) {
            return true;
        }
    }
    return false;
}

static bool tpms_adv_chunk_has_uuid128(const uint8_t *data, uint8_t len)
{
    if (data == NULL || len < 2) {
        return false;
    }

    uint8_t offset = 0;
    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > len) {
            break;
        }
        uint8_t ad_type = data[offset + 1];
        const uint8_t *payload = &data[offset + 2];
        uint8_t payload_len = (uint8_t)(field_len - 1);

        if (ad_type == ESP_BLE_AD_TYPE_128SRV_CMPL || ad_type == ESP_BLE_AD_TYPE_128SRV_PART) {
            for (uint8_t u = 0; u + ESP_UUID_LEN_128 <= payload_len; u += ESP_UUID_LEN_128) {
                if (memcmp(payload + u, s_tpms_service_uuid128, ESP_UUID_LEN_128) == 0) {
                    return true;
                }
            }
        }

        offset = (uint8_t)(offset + 1 + field_len);
    }
    return false;
}

static bool tpms_adv_chunk_has_tpms_data(const uint8_t *data, uint8_t len)
{
    if (data == NULL || len < 2) {
        return false;
    }

    if (tpms_buf_has_marker(data, len)) {
        return true;
    }

    if (len >= 5 && data[0] == 0x12 && tpms_slice_parseable(data, len)) {
        return true;
    }

    uint8_t offset = 0;
    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > len) {
            break;
        }
        uint8_t ad_type = data[offset + 1];
        const uint8_t *payload = &data[offset + 2];
        uint8_t payload_len = (uint8_t)(field_len - 1);

        if (payload_len >= 5) {
            if (tpms_buf_has_marker(payload, payload_len)) {
                return true;
            }
            if (payload[0] == 0x12 && tpms_slice_parseable(payload, payload_len)) {
                return true;
            }
            /* Manufacturer-specific: 2-byte company ID then TPMS payload. */
            if (ad_type == ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE && payload_len > 2) {
                const uint8_t *body = payload + 2;
                uint8_t body_len = (uint8_t)(payload_len - 2);
                if (tpms_buf_has_marker(body, body_len)) {
                    return true;
                }
                if (body_len >= 5 && body[0] == 0x12 && tpms_slice_parseable(body, body_len)) {
                    return true;
                }
                /* Scan for 0x12-framed packet inside manufacturer blob. */
                for (uint8_t i = 0; i + 5 <= body_len; i++) {
                    if (body[i] != 0x12) {
                        continue;
                    }
                    uint8_t outer = body[i + 1];
                    size_t pkt_len = (size_t)(2 + outer);
                    if (i + pkt_len <= body_len && tpms_slice_parseable(body + i, pkt_len)) {
                        return true;
                    }
                }
            }
            if (ad_type == ESP_BLE_AD_TYPE_SERVICE_DATA && payload_len > 2) {
                const uint8_t *body = payload + 2;
                uint8_t body_len = (uint8_t)(payload_len - 2);
                if (body_len >= 5 && body[0] == 0x12 && tpms_slice_parseable(body, body_len)) {
                    return true;
                }
                if (tpms_buf_has_marker(body, body_len)) {
                    return true;
                }
            }
        }

        offset = (uint8_t)(offset + 1 + field_len);
    }
    return false;
}

static bool tpms_get_adv_name(const uint8_t *adv, uint8_t adv_len, uint8_t scan_rsp_len,
                              uint8_t **out_name, uint8_t *out_len)
{
    const uint8_t total = (uint8_t)(adv_len + scan_rsp_len);
    if (total == 0 || out_name == NULL || out_len == NULL) {
        return false;
    }
    *out_len = 0;
    *out_name = esp_ble_resolve_adv_data_by_type(adv, total, ESP_BLE_AD_TYPE_NAME_CMPL, out_len);
    if (*out_name == NULL || *out_len == 0) {
        *out_name = esp_ble_resolve_adv_data_by_type(adv, total, ESP_BLE_AD_TYPE_NAME_SHORT, out_len);
    }
    return *out_name != NULL && *out_len > 0;
}

static bool tpms_name_contains_tpms(const uint8_t *name, uint8_t name_len)
{
    if (name == NULL || name_len < 4) {
        return false;
    }
    for (uint8_t i = 0; i + 4 <= name_len; i++) {
        char a0 = (char)name[i];
        char a1 = (i + 1 < name_len) ? (char)name[i + 1] : 0;
        char a2 = (i + 2 < name_len) ? (char)name[i + 2] : 0;
        char a3 = (i + 3 < name_len) ? (char)name[i + 3] : 0;
        if (a0 >= 'A' && a0 <= 'Z') a0 = (char)(a0 - 'A' + 'a');
        if (a1 >= 'A' && a1 <= 'Z') a1 = (char)(a1 - 'A' + 'a');
        if (a2 >= 'A' && a2 <= 'Z') a2 = (char)(a2 - 'A' + 'a');
        if (a3 >= 'A' && a3 <= 'Z') a3 = (char)(a3 - 'A' + 'a');
        if (a0 == 't' && a1 == 'p' && a2 == 'm' && a3 == 's') {
            return true;
        }
    }
    return false;
}

static bool tpms_chunk_has_company_id(const uint8_t *data, uint8_t len, uint16_t company_id)
{
    if (data == NULL || len < 2) {
        return false;
    }

    uint8_t offset = 0;
    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > len) {
            break;
        }
        uint8_t ad_type = data[offset + 1];
        const uint8_t *payload = &data[offset + 2];
        uint8_t payload_len = (uint8_t)(field_len - 1);

        if (ad_type == ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE && payload_len >= 2) {
            uint16_t cid = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
            if (cid == company_id) {
                return true;
            }
        }

        offset = (uint8_t)(offset + 1 + field_len);
    }
    return false;
}

static bool tpms_adv_has_company_id(const uint8_t *adv, uint8_t adv_len, uint8_t scan_len, uint16_t company_id)
{
    if (adv_len > 0 && tpms_chunk_has_company_id(adv, adv_len, company_id)) {
        return true;
    }
    if (scan_len > 0 && tpms_chunk_has_company_id(adv + adv_len, scan_len, company_id)) {
        return true;
    }
    return false;
}

/** tsTPMS gateway advertises 16-bit service UUID 0x1122 (bytes 11 22 in AD). */
#define TPMS_GATEWAY_UUID16 0x1122u
/** Silicon Labs company ID seen on tsTPMS gateway advertisements. */
#define TPMS_GATEWAY_COMPANY_ID 0x022Bu

static bool tpms_chunk_has_uuid16(const uint8_t *data, uint8_t len, uint16_t uuid16)
{
    if (data == NULL || len < 2) {
        return false;
    }

    uint8_t offset = 0;
    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > len) {
            break;
        }
        uint8_t ad_type = data[offset + 1];
        const uint8_t *payload = &data[offset + 2];
        uint8_t payload_len = (uint8_t)(field_len - 1);

        if ((ad_type == ESP_BLE_AD_TYPE_16SRV_CMPL || ad_type == ESP_BLE_AD_TYPE_16SRV_PART) &&
            payload_len >= 2) {
            for (uint8_t u = 0; u + 2 <= payload_len; u += 2) {
                uint16_t u16 = (uint16_t)(payload[u] | ((uint16_t)payload[u + 1] << 8));
                if (u16 == uuid16) {
                    return true;
                }
            }
        }

        offset = (uint8_t)(offset + 1 + field_len);
    }
    return false;
}

static bool tpms_adv_has_uuid16(const uint8_t *adv, uint8_t adv_len, uint8_t scan_len, uint16_t uuid16)
{
    if (adv_len > 0 && tpms_chunk_has_uuid16(adv, adv_len, uuid16)) {
        return true;
    }
    if (scan_len > 0 && tpms_chunk_has_uuid16(adv + adv_len, scan_len, uuid16)) {
        return true;
    }
    return false;
}

bool tpms_ble_adv_is_gateway(const esp_ble_gap_cb_param_t *param)
{
    if (param == NULL) {
        return false;
    }

    const uint8_t adv_len = param->scan_rst.adv_data_len;
    const uint8_t scan_len = param->scan_rst.scan_rsp_len;
    if (adv_len == 0 && scan_len == 0) {
        return false;
    }

    const uint8_t *adv = (const uint8_t *)param->scan_rst.ble_adv;
    uint8_t name_len = 0;
    uint8_t *name = NULL;
    if (tpms_get_adv_name(adv, adv_len, scan_len, &name, &name_len) &&
        tpms_name_is_gateway(name, name_len)) {
        return true;
    }

    if (tpms_adv_has_uuid16(adv, adv_len, scan_len, TPMS_GATEWAY_UUID16)) {
        return true;
    }

    if (tpms_adv_has_company_id(adv, adv_len, scan_len, TPMS_GATEWAY_COMPANY_ID) &&
        tpms_get_adv_name(adv, adv_len, scan_len, &name, &name_len) &&
        tpms_name_is_gateway(name, name_len)) {
        return true;
    }

    return false;
}

static bool tpms_ble_adv_is_wheel_only(const esp_ble_gap_cb_param_t *param)
{
    if (param == NULL || tpms_ble_adv_is_gateway(param)) {
        return false;
    }

    const uint8_t adv_len = param->scan_rst.adv_data_len;
    const uint8_t scan_len = param->scan_rst.scan_rsp_len;
    const uint8_t *adv = (const uint8_t *)param->scan_rst.ble_adv;

    uint8_t name_len = 0;
    uint8_t *name = NULL;
    if (tpms_get_adv_name(adv, adv_len, scan_len, &name, &name_len) &&
        tpms_name_contains_tpms(name, name_len)) {
        return true;
    }

    if (adv_len > 0 && tpms_adv_chunk_has_uuid128(adv, adv_len)) {
        return true;
    }
    if (scan_len > 0 && tpms_adv_chunk_has_uuid128(adv + adv_len, scan_len)) {
        return true;
    }
    if (adv_len > 0 && tpms_adv_chunk_has_tpms_data(adv, adv_len)) {
        return true;
    }
    if (scan_len > 0 && tpms_adv_chunk_has_tpms_data(adv + adv_len, scan_len)) {
        return true;
    }
    if (tpms_adv_has_company_id(adv, adv_len, scan_len, 0x04C3)) {
        return true;
    }

    return false;
}

bool tpms_ble_adv_is_wheel_sensor(const esp_ble_gap_cb_param_t *param)
{
    return tpms_ble_adv_is_wheel_only(param);
}

bool tpms_ble_adv_is_discovery_candidate(const esp_ble_gap_cb_param_t *param)
{
    return tpms_ble_adv_is_gateway(param) || tpms_ble_adv_is_wheel_only(param);
}

static bool tpms_try_copy_payload(const uint8_t *data, size_t len,
                                  uint8_t *out_payload, size_t out_cap, size_t *out_len)
{
    if (data == NULL || len < 5 || out_payload == NULL || out_cap == 0) {
        return false;
    }

    if (data[0] == 0x12 && tpms_slice_parseable(data, len)) {
        size_t copy_len = len < out_cap ? len : out_cap;
        memcpy(out_payload, data, copy_len);
        if (out_len != NULL) {
            *out_len = copy_len;
        }
        return true;
    }

    for (size_t i = 0; i + 5 <= len; i++) {
        if (data[i] != 0x12) {
            continue;
        }
        uint8_t outer = data[i + 1];
        size_t pkt_len = (size_t)(2 + outer);
        if (i + pkt_len > len) {
            continue;
        }
        if (tpms_slice_parseable(data + i, pkt_len)) {
            size_t copy_len = pkt_len < out_cap ? pkt_len : out_cap;
            memcpy(out_payload, data + i, copy_len);
            if (out_len != NULL) {
                *out_len = copy_len;
            }
            return true;
        }
    }
    return false;
}

static bool tpms_try_chunk_payload(const uint8_t *data, uint8_t len,
                                   uint8_t *out_payload, size_t out_cap, size_t *out_len)
{
    if (data == NULL || len < 2) {
        return false;
    }

    if (tpms_try_copy_payload(data, len, out_payload, out_cap, out_len)) {
        return true;
    }

    uint8_t offset = 0;
    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > len) {
            break;
        }
        uint8_t ad_type = data[offset + 1];
        const uint8_t *payload = &data[offset + 2];
        uint8_t payload_len = (uint8_t)(field_len - 1);

        if (payload_len >= 5) {
            if (tpms_try_copy_payload(payload, payload_len, out_payload, out_cap, out_len)) {
                return true;
            }
            if (ad_type == ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE && payload_len > 2) {
                if (tpms_try_copy_payload(payload + 2, payload_len - 2, out_payload, out_cap, out_len)) {
                    return true;
                }
            }
            if (ad_type == ESP_BLE_AD_TYPE_SERVICE_DATA && payload_len > 2) {
                if (tpms_try_copy_payload(payload + 2, payload_len - 2, out_payload, out_cap, out_len)) {
                    return true;
                }
            }
        }

        offset = (uint8_t)(offset + 1 + field_len);
    }
    return false;
}

bool tpms_ble_adv_try_parse_telemetry(const uint8_t *adv, uint8_t adv_len,
                                      const uint8_t *scan_rsp, uint8_t scan_rsp_len,
                                      uint8_t *out_payload, size_t out_cap, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }

    if (adv_len > 0 && tpms_try_chunk_payload(adv, adv_len, out_payload, out_cap, out_len)) {
        return true;
    }
    if (scan_rsp_len > 0 &&
        tpms_try_chunk_payload(scan_rsp, scan_rsp_len, out_payload, out_cap, out_len)) {
        return true;
    }
    return false;
}
