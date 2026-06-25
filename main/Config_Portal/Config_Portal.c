#include "Config_Portal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "Datetime_Set.h"
#include "PCF85063.h"
#include "UI_Navigation.h"
#include "tpms_manager.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "Config_Portal";

static const char *portal_http_method_str(httpd_method_t method);
static void log_http_request(httpd_req_t *req);

/* EMBED_TXTFILES symbols use the file basename (see IDF target_add_binary_data). */
extern const char service_html_start[] asm("_binary_service_html_start");
extern const char service_html_end[] asm("_binary_service_html_end");
extern const char service_css_start[] asm("_binary_service_css_start");
extern const char service_css_end[] asm("_binary_service_css_end");
extern const char service_js_start[] asm("_binary_service_js_start");
extern const char service_js_end[] asm("_binary_service_js_end");

typedef struct {
    const char *uri;
    const char *start;
    const char *end;
    const char *content_type;
} portal_static_file_t;

static const portal_static_file_t s_static_files[] = {
    { .uri = "/service.html", .start = service_html_start, .end = service_html_end,
      .content_type = "text/html; charset=utf-8" },
    { .uri = "/service.css", .start = service_css_start, .end = service_css_end,
      .content_type = "text/css; charset=utf-8" },
    { .uri = "/service.js", .start = service_js_start, .end = service_js_end,
      .content_type = "application/javascript; charset=utf-8" },
};

#define PORTAL_JSON_BUF_MAX 384

typedef struct {
    const char *slug;
    ux_page_id_t page_id;
} portal_page_slug_t;

static const portal_page_slug_t s_page_slugs[] = {
    { "boot-logo", UX_PAGE_BOOT_LOGO },
    { "clock", UX_PAGE_CLOCK },
    { "speed", UX_PAGE_SPEED },
    { "acceleration", UX_PAGE_ACCELERATION },
    { "weather", UX_PAGE_WEATHER },
    { "status", UX_PAGE_STATUS },
};

static esp_err_t portal_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool portal_normalize_mac(const char *mac_in, char *mac_out, size_t mac_out_len)
{
    if (mac_in == NULL || mac_out == NULL || mac_out_len < 18) {
        return false;
    }

    uint8_t bda[6];
    if (!tpms_manager_parse_mac(mac_in, bda)) {
        return false;
    }
    return tpms_manager_format_mac(bda, mac_out, mac_out_len);
}

static esp_err_t portal_recv_body(httpd_req_t *req, char *buf, size_t buf_size, size_t *out_len)
{
    if (req->content_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int received = httpd_req_recv(req, buf + total, buf_size - total - 1U);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)received;
    }
    buf[total] = '\0';
    if (out_len != NULL) {
        *out_len = total;
    }
    return ESP_OK;
}

static esp_err_t portal_api_rtc_get_handler(httpd_req_t *req)
{
    log_http_request(req);
    char body[160];
    (void)snprintf(body, sizeof(body),
                   "{\"ok\":true,\"year\":%u,\"month\":%u,\"day\":%u,"
                   "\"hour\":%u,\"minute\":%u,\"second\":%u}",
                   (unsigned)datetime.year, (unsigned)datetime.month, (unsigned)datetime.day,
                   (unsigned)datetime.hour, (unsigned)datetime.minute, (unsigned)datetime.second);
    return portal_send_json(req, body);
}

static int portal_json_int(cJSON *obj, const char *key, int *out)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item)) {
        return -1;
    }
    *out = item->valueint;
    return 0;
}

static esp_err_t portal_api_rtc_set_handler(httpd_req_t *req)
{
    log_http_request(req);

    char buf[PORTAL_JSON_BUF_MAX];
    esp_err_t err = portal_recv_body(req, buf, sizeof(buf), NULL);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (portal_json_int(json, "year", &year) != 0 || portal_json_int(json, "month", &month) != 0 ||
        portal_json_int(json, "day", &day) != 0 || portal_json_int(json, "hour", &hour) != 0 ||
        portal_json_int(json, "minute", &minute) != 0 || portal_json_int(json, "second", &second) != 0) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"missing fields\"}");
    }
    cJSON_Delete(json);

    err = datetime_set((uint16_t)year, (uint8_t)month, (uint8_t)day,
                       (uint8_t)hour, (uint8_t)minute, (uint8_t)second);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC set rejected: %s", esp_err_to_name(err));
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid date/time\"}");
    }

    ESP_LOGI(TAG, "RTC set from portal: %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return portal_send_json(req, "{\"ok\":true}");
}

static const portal_page_slug_t *portal_slug_lookup(const char *slug)
{
    for (size_t i = 0; i < sizeof(s_page_slugs) / sizeof(s_page_slugs[0]); i++) {
        if (strcmp(s_page_slugs[i].slug, slug) == 0) {
            return &s_page_slugs[i];
        }
    }
    return NULL;
}

static const char *portal_slug_for_page(ux_page_id_t page_id)
{
    for (size_t i = 0; i < sizeof(s_page_slugs) / sizeof(s_page_slugs[0]); i++) {
        if (s_page_slugs[i].page_id == page_id) {
            return s_page_slugs[i].slug;
        }
    }
    return NULL;
}

static esp_err_t portal_api_pages_order_get_handler(httpd_req_t *req)
{
    log_http_request(req);

    uint8_t order[UX_NAVIGABLE_COUNT];
    if (ux_navigation_get_page_order(order, UX_NAVIGABLE_COUNT) != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"unavailable\"}");
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }

    for (size_t i = 0; i < UX_NAVIGABLE_COUNT; i++) {
        ux_page_id_t page_id = (ux_page_id_t)order[i];
        const char *slug = portal_slug_for_page(page_id);
        if (slug == NULL) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(arr);
            return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
        }
        cJSON_AddStringToObject(item, "id", slug);
        cJSON_AddStringToObject(item, "name", ux_navigation_page_name(page_id));
        cJSON_AddItemToArray(arr, item);
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(arr);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "pages", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }

    esp_err_t ret = portal_send_json(req, json);
    cJSON_free(json);
    return ret;
}

static esp_err_t portal_api_pages_order_post_handler(httpd_req_t *req)
{
    log_http_request(req);

    char buf[PORTAL_JSON_BUF_MAX];
    esp_err_t err = portal_recv_body(req, buf, sizeof(buf), NULL);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *json = cJSON_Parse(buf);
    if (!cJSON_IsArray(json)) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"expected array\"}");
    }

    const int count = cJSON_GetArraySize(json);
    if (count != UX_NAVIGABLE_COUNT) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"wrong count\"}");
    }

    uint8_t order[UX_NAVIGABLE_COUNT];
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(json, i);
        if (!cJSON_IsString(item)) {
            cJSON_Delete(json);
            return portal_send_json(req, "{\"ok\":false,\"error\":\"expected string ids\"}");
        }
        const portal_page_slug_t *slug = portal_slug_lookup(cJSON_GetStringValue(item));
        if (slug == NULL) {
            cJSON_Delete(json);
            return portal_send_json(req, "{\"ok\":false,\"error\":\"unknown page id\"}");
        }
        order[i] = (uint8_t)slug->page_id;
    }
    cJSON_Delete(json);

    err = ux_navigation_request_set_page_order(order, UX_NAVIGABLE_COUNT);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid order\"}");
    }

    ESP_LOGI(TAG, "Page order update queued from portal");
    return portal_send_json(req, "{\"ok\":true}");
}

static const char *portal_tpms_status_str(tpms_sensor_status_t status)
{
    switch (status) {
    case TPMS_SENSOR_STATUS_LIVE:
        return "live";
    case TPMS_SENSOR_STATUS_RECENT:
        return "recent";
    default:
        return "stale";
    }
}

static const char *portal_tpms_telemetry_str(tpms_telemetry_state_t state)
{
    switch (state) {
    case TPMS_TELEMETRY_STABLE:
        return "stable";
    case TPMS_TELEMETRY_ACTIVE:
        return "active";
    default:
        return "none";
    }
}

static esp_err_t portal_api_tpms_get_handler(httpd_req_t *req)
{
    log_http_request(req);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "scan_active", tpms_manager_is_scan_active());
    cJSON_AddBoolToObject(root, "rotation_active", tpms_manager_is_rotation_active());

    char primary_mac[18];
    if (tpms_manager_get_primary_gatt_mac(primary_mac, sizeof(primary_mac)) == ESP_OK) {
        cJSON_AddStringToObject(root, "primary_gatt_mac", primary_mac);
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        cJSON_Delete(root);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }

    const int count = tpms_manager_sensor_count();
    for (int i = 0; i < count; i++) {
        tpms_sensor_snapshot_t snap;
        if (tpms_manager_get_snapshot(i, &snap) != ESP_OK) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }

        cJSON_AddStringToObject(item, "mac", snap.mac);
        if (snap.display_name[0] != '\0') {
            cJSON_AddStringToObject(item, "display_name", snap.display_name);
        }
        if (snap.is_gateway) {
            cJSON_AddBoolToObject(item, "is_gateway", true);
        }
        if (snap.is_primary_gatt) {
            cJSON_AddBoolToObject(item, "is_primary_gatt", true);
        }
        if (snap.is_gatt_active) {
            cJSON_AddBoolToObject(item, "is_gatt_active", true);
        }
        cJSON_AddStringToObject(item, "position", tpms_position_to_string(snap.position));
        cJSON_AddStringToObject(item, "status", portal_tpms_status_str(snap.status));
        cJSON_AddStringToObject(item, "telemetry_state", portal_tpms_telemetry_str(snap.telemetry_state));
        cJSON_AddStringToObject(item, "pressure_unit", snap.units.pressure_unit);
        cJSON_AddStringToObject(item, "temp_unit", snap.units.temp_unit);

        if (snap.has_telemetry) {
            cJSON_AddNumberToObject(item, "pressure", (double)snap.pressure);
            cJSON_AddNumberToObject(item, "temperature", (double)snap.temperature);
        }

        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "sensors", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }

    esp_err_t ret = portal_send_json(req, json);
    cJSON_free(json);
    return ret;
}

static esp_err_t portal_api_tpms_scan_handler(httpd_req_t *req)
{
    log_http_request(req);

    char buf[PORTAL_JSON_BUF_MAX];
    esp_err_t err = portal_recv_body(req, buf, sizeof(buf), NULL);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    cJSON *active_item = cJSON_GetObjectItem(json, "active");
    if (!cJSON_IsBool(active_item)) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"missing active\"}");
    }

    bool active = cJSON_IsTrue(active_item);
    cJSON_Delete(json);

    err = tpms_manager_set_scan_active(active);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"scan failed\"}");
    }

    return portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t portal_tpms_forget_by_request(httpd_req_t *req)
{
    char mac_norm[18];
    bool have_mac = false;

    if (req->content_len > 0) {
        char buf[PORTAL_JSON_BUF_MAX];
        if (portal_recv_body(req, buf, sizeof(buf), NULL) == ESP_OK) {
            cJSON *json = cJSON_Parse(buf);
            if (json != NULL) {
                cJSON *mac_item = cJSON_GetObjectItem(json, "mac");
                if (cJSON_IsString(mac_item)) {
                    have_mac = portal_normalize_mac(cJSON_GetStringValue(mac_item),
                                                    mac_norm, sizeof(mac_norm));
                }
                cJSON_Delete(json);
            }
        }
    }

    if (!have_mac) {
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 80) {
            char query[96];
            if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
                char mac_param[32];
                if (httpd_query_key_value(query, "mac", mac_param, sizeof(mac_param)) == ESP_OK) {
                    have_mac = portal_normalize_mac(mac_param, mac_norm, sizeof(mac_norm));
                }
            }
        }
    }

    if (!have_mac) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"missing mac\"}");
    }

    esp_err_t err = tpms_manager_forget(mac_norm);
    if (err == ESP_ERR_INVALID_STATE) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"stop scan first\"}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"not found\"}");
    }
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"forget failed\"}");
    }

    return portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t portal_api_tpms_forget_handler(httpd_req_t *req)
{
    log_http_request(req);
    return portal_tpms_forget_by_request(req);
}

static esp_err_t portal_api_tpms_position_handler(httpd_req_t *req)
{
    log_http_request(req);

    char buf[PORTAL_JSON_BUF_MAX];
    esp_err_t err = portal_recv_body(req, buf, sizeof(buf), NULL);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    cJSON *mac_item = cJSON_GetObjectItem(json, "mac");
    cJSON *pos_item = cJSON_GetObjectItem(json, "position");
    if (!cJSON_IsString(mac_item) || !cJSON_IsString(pos_item)) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"missing fields\"}");
    }

    char mac_norm[18];
    const char *pos_str = cJSON_GetStringValue(pos_item);
    if (!portal_normalize_mac(cJSON_GetStringValue(mac_item), mac_norm, sizeof(mac_norm))) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid mac\"}");
    }
    tpms_tire_position_t pos;
    if (!tpms_position_from_string(pos_str, &pos)) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid position\"}");
    }
    cJSON_Delete(json);

    err = tpms_manager_set_position(mac_norm, pos);
    if (err == ESP_ERR_NOT_FOUND) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"not found\"}");
    }
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"set failed\"}");
    }

    return portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t portal_api_tpms_primary_handler(httpd_req_t *req)
{
    log_http_request(req);

    char buf[PORTAL_JSON_BUF_MAX];
    esp_err_t err = portal_recv_body(req, buf, sizeof(buf), NULL);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    cJSON *mac_item = cJSON_GetObjectItem(json, "mac");
    if (!cJSON_IsString(mac_item)) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"missing mac\"}");
    }

    char mac_norm[18];
    if (!portal_normalize_mac(cJSON_GetStringValue(mac_item), mac_norm, sizeof(mac_norm))) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid mac\"}");
    }
    cJSON_Delete(json);

    err = tpms_manager_set_primary_gatt(mac_norm);
    if (err == ESP_ERR_INVALID_STATE) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"stop scan first\"}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"not found\"}");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"not a tsTPMS module\"}");
    }
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }

    return portal_send_json(req, "{\"ok\":true}");
}

static esp_err_t portal_api_settings_units_get_handler(httpd_req_t *req)
{
    log_http_request(req);

    tpms_units_t units;
    if (tpms_manager_get_units(&units) != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"unavailable\"}");
    }

    char body[128];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"temp_unit\":\"%s\",\"pressure_unit\":\"%s\"}",
             units.temp_unit, units.pressure_unit);
    return portal_send_json(req, body);
}

static esp_err_t portal_api_settings_units_post_handler(httpd_req_t *req)
{
    log_http_request(req);

    char buf[PORTAL_JSON_BUF_MAX];
    esp_err_t err = portal_recv_body(req, buf, sizeof(buf), NULL);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    tpms_units_t units;
    if (tpms_manager_get_units(&units) != ESP_OK) {
        cJSON_Delete(json);
        return portal_send_json(req, "{\"ok\":false,\"error\":\"unavailable\"}");
    }

    cJSON *temp = cJSON_GetObjectItem(json, "temp_unit");
    if (cJSON_IsString(temp)) {
        const char *v = cJSON_GetStringValue(temp);
        if (strcmp(v, "F") == 0 || strcmp(v, "f") == 0) {
            strncpy(units.temp_unit, "F", sizeof(units.temp_unit) - 1);
        } else {
            strncpy(units.temp_unit, "C", sizeof(units.temp_unit) - 1);
        }
        units.temp_unit[sizeof(units.temp_unit) - 1] = '\0';
    }

    cJSON *press = cJSON_GetObjectItem(json, "pressure_unit");
    if (cJSON_IsString(press)) {
        const char *v = cJSON_GetStringValue(press);
        if (strcmp(v, "psi") == 0 || strcmp(v, "PSI") == 0) {
            strncpy(units.pressure_unit, "psi", sizeof(units.pressure_unit) - 1);
        } else {
            strncpy(units.pressure_unit, "kpa", sizeof(units.pressure_unit) - 1);
        }
        units.pressure_unit[sizeof(units.pressure_unit) - 1] = '\0';
    }

    cJSON_Delete(json);

    err = tpms_manager_set_units(&units);
    if (err != ESP_OK) {
        return portal_send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }

    return portal_send_json(req, "{\"ok\":true}");
}

/** Must outlive the DHCP server (see esp_netif_dhcps_option). */
static char s_captiveportal_uri[48];

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task = NULL;

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define DNS_TASK_STACK 4096

#define DNS_OPCODE_MASK 0x7800
#define DNS_QR_FLAG (1 << 7)
#define DNS_QTYPE_A 0x0001
#define DNS_QTYPE_AAAA 0x001C
#define DNS_ANS_TTL_SEC 300

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((packed)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

static const char *portal_http_method_str(httpd_method_t method)
{
    switch (method) {
    case HTTP_GET:
        return "GET";
    case HTTP_HEAD:
        return "HEAD";
    case HTTP_POST:
        return "POST";
    case HTTP_PUT:
        return "PUT";
    case HTTP_DELETE:
        return "DELETE";
    default:
        return "OTHER";
    }
}

static void log_http_request(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP %s %s", portal_http_method_str(req->method), req->uri);
}

/** EMBED_TXTFILES append a trailing NUL; do not send it in the HTTP body. */
static size_t portal_embedded_len(const char *start, const char *end)
{
    size_t len = (size_t)(end - start);
    if (len > 0 && start[len - 1] == '\0') {
        len--;
    }
    return len;
}

static esp_err_t portal_send_static(httpd_req_t *req, const portal_static_file_t *file)
{
    log_http_request(req);
    httpd_resp_set_type(req, file->content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, file->start, portal_embedded_len(file->start, file->end));
}

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    return portal_send_static(req, &s_static_files[0]);
}

static esp_err_t portal_service_handler(httpd_req_t *req)
{
    return portal_send_static(req, &s_static_files[0]);
}

static esp_err_t portal_service_css_handler(httpd_req_t *req)
{
    return portal_send_static(req, &s_static_files[1]);
}

static esp_err_t portal_service_js_handler(httpd_req_t *req)
{
    return portal_send_static(req, &s_static_files[2]);
}

/**
 * Android checks /generate_204: must NOT return HTTP 204 — serve setup HTML instead.
 */
static esp_err_t portal_android_probe_handler(httpd_req_t *req)
{
    log_http_request(req);
    ESP_LOGI(TAG, "Captive probe (Android 204 check): %s", req->uri);

    if (req->method == HTTP_HEAD) {
        httpd_resp_set_status(req, "200 OK");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, service_html_start,
                           portal_embedded_len(service_html_start, service_html_end));
}

/**
 * Captive-portal probes: 302 to portal URL. iOS needs a body on GET, not on HEAD.
 */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    char location[56];

    log_http_request(req);
    snprintf(location, sizeof(location), "%s/", s_captiveportal_uri);
    ESP_LOGI(TAG, "Captive redirect: %s -> %s", req->uri, location);

    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    if (req->method == HTTP_HEAD) {
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    log_http_request(req);

    if (strncmp(req->uri, "/api/", 5) == 0) {
        ESP_LOGW(TAG, "API 404: %s", req->uri);
        httpd_resp_set_status(req, "404 Not Found");
        return portal_send_json(req, "{\"ok\":false,\"error\":\"not found\"}");
    }

    ESP_LOGI(TAG, "Captive redirect (404): %s -> /", req->uri);
    return portal_redirect_handler(req);
}

#define PORTAL_URI_GET(path) \
    { .uri = path, .method = HTTP_GET, .handler = portal_redirect_handler }
#define PORTAL_URI_HEAD(path) \
    { .uri = path, .method = HTTP_HEAD, .handler = portal_redirect_handler }
#define PORTAL_URI_GET_ANDROID(path) \
    { .uri = path, .method = HTTP_GET, .handler = portal_android_probe_handler }
#define PORTAL_URI_HEAD_ANDROID(path) \
    { .uri = path, .method = HTTP_HEAD, .handler = portal_android_probe_handler }

static const httpd_uri_t s_portal_uris[] = {
    { .uri = "/", .method = HTTP_GET, .handler = portal_root_handler },
    { .uri = "/service", .method = HTTP_GET, .handler = portal_service_handler },
    { .uri = "/service.html", .method = HTTP_GET, .handler = portal_service_handler },
    { .uri = "/service.css", .method = HTTP_GET, .handler = portal_service_css_handler },
    { .uri = "/service.js", .method = HTTP_GET, .handler = portal_service_js_handler },
    { .uri = "/api/rtc", .method = HTTP_GET, .handler = portal_api_rtc_get_handler },
    { .uri = "/api/rtc/set", .method = HTTP_POST, .handler = portal_api_rtc_set_handler },
    { .uri = "/api/pages/order", .method = HTTP_GET, .handler = portal_api_pages_order_get_handler },
    { .uri = "/api/pages/order", .method = HTTP_POST, .handler = portal_api_pages_order_post_handler },
    { .uri = "/api/tpms", .method = HTTP_GET, .handler = portal_api_tpms_get_handler },
    { .uri = "/api/tpms/scan", .method = HTTP_POST, .handler = portal_api_tpms_scan_handler },
    { .uri = "/api/tpms/sensors", .method = HTTP_DELETE, .handler = portal_api_tpms_forget_handler },
    { .uri = "/api/tpms/sensors/forget", .method = HTTP_POST, .handler = portal_api_tpms_forget_handler },
    { .uri = "/api/tpms/sensors/position", .method = HTTP_PUT, .handler = portal_api_tpms_position_handler },
    { .uri = "/api/tpms/sensors/position", .method = HTTP_POST, .handler = portal_api_tpms_position_handler },
    { .uri = "/api/tpms/sensors/primary", .method = HTTP_POST, .handler = portal_api_tpms_primary_handler },
    { .uri = "/api/settings/units", .method = HTTP_GET, .handler = portal_api_settings_units_get_handler },
    { .uri = "/api/settings/units", .method = HTTP_POST, .handler = portal_api_settings_units_post_handler },
    PORTAL_URI_GET("/index.html"),
    PORTAL_URI_GET_ANDROID("/generate_204"),
    PORTAL_URI_HEAD_ANDROID("/generate_204"),
    PORTAL_URI_GET_ANDROID("/gen_204"),
    PORTAL_URI_HEAD_ANDROID("/gen_204"),
    PORTAL_URI_GET("/hotspot-detect.html"),
    PORTAL_URI_HEAD("/hotspot-detect.html"),
    PORTAL_URI_GET("/library/test/success.html"),
    PORTAL_URI_HEAD("/library/test/success.html"),
    PORTAL_URI_GET("/connecttest.txt"),
    PORTAL_URI_HEAD("/connecttest.txt"),
    PORTAL_URI_GET("/ncsi.txt"),
    PORTAL_URI_HEAD("/ncsi.txt"),
    PORTAL_URI_GET("/fwlink"),
    PORTAL_URI_HEAD("/fwlink"),
    PORTAL_URI_GET("/redirect"),
    PORTAL_URI_GET("/canonical.html"),
    PORTAL_URI_GET("/success.txt"),
    PORTAL_URI_HEAD("/success.txt"),
};

static char *parse_dns_name(char *raw_name, char *parsed_name, size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;

    do {
        int sub_name_len = (unsigned char)*label;
        name_len += sub_name_len + 1;
        if (name_len > (int)parsed_name_max_len) {
            return NULL;
        }
        memcpy(name_itr, label + 1, (size_t)sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += sub_name_len + 1;
        label += sub_name_len + 1;
    } while (*label != 0);

    if (name_len > 0) {
        parsed_name[name_len - 1] = '\0';
    } else {
        parsed_name[0] = '\0';
    }
    return label + 1;
}

static const char *dns_qtype_str(uint16_t qtype)
{
    switch (qtype) {
    case DNS_QTYPE_A:
        return "A";
    case DNS_QTYPE_AAAA:
        return "AAAA";
    default:
        return "OTHER";
    }
}

/**
 * Answer all A queries with AP IP; AAAA/other types get NOERROR with no data
 * so clients fall back to A or HTTP probes.
 */
static int build_dns_reply(const uint8_t *req, int req_len, uint8_t *reply, int reply_max, uint32_t ap_ip)
{
    if (req_len > reply_max) {
        return -1;
    }

    memcpy(reply, req, (size_t)req_len);
    dns_header_t *header = (dns_header_t *)reply;

    if ((ntohs(header->flags) & DNS_OPCODE_MASK) != 0) {
        return 0;
    }

    uint16_t qd_count = ntohs(header->qd_count);
    if (qd_count == 0) {
        return 0;
    }

    header->flags = htons(ntohs(header->flags) | DNS_QR_FLAG);
    header->ns_count = 0;
    header->ar_count = 0;

    char *cur_qd = (char *)reply + sizeof(dns_header_t);
    char *cur_ans = (char *)reply + req_len;
    char name[128];
    uint16_t an_count = 0;
    bool needs_empty_reply = false;

    for (uint16_t qd_i = 0; qd_i < qd_count; qd_i++) {
        char *qname_start = cur_qd;
        char *name_end = parse_dns_name(cur_qd, name, sizeof(name));
        if (!name_end) {
            return -1;
        }

        dns_question_t *question = (dns_question_t *)name_end;
        uint16_t qtype = ntohs(question->type);
        cur_qd = name_end + sizeof(dns_question_t);

        if (qtype == DNS_QTYPE_A) {
            if ((cur_ans + sizeof(dns_answer_t)) > (char *)reply + reply_max) {
                return -1;
            }

            dns_answer_t *answer = (dns_answer_t *)cur_ans;
            answer->ptr_offset = htons(0xC000 | (uint16_t)(qname_start - (char *)reply));
            answer->type = htons(DNS_QTYPE_A);
            answer->class = question->class;
            answer->ttl = htonl(DNS_ANS_TTL_SEC);
            answer->addr_len = htons(4);
            answer->ip_addr = ap_ip;
            cur_ans += sizeof(dns_answer_t);
            an_count++;

            ESP_LOGD(TAG, "DNS answer %s A -> " IPSTR, name, IP2STR(&(esp_ip4_addr_t){ .addr = ap_ip }));
        } else {
            needs_empty_reply = true;
            ESP_LOGD(TAG, "DNS query %s %s (NOERROR, no answer)", name, dns_qtype_str(qtype));
        }
    }

    header->an_count = htons(an_count);

    if (an_count > 0) {
        return (int)(cur_ans - (char *)reply);
    }
    if (needs_empty_reply) {
        return req_len;
    }
    return 0;
}

static void dns_server_task(void *arg)
{
    uint32_t ap_ip = (uint32_t)(uintptr_t)arg;
    uint8_t *rx_buf = heap_caps_malloc(DNS_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *tx_buf = heap_caps_malloc(DNS_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rx_buf == NULL || tx_buf == NULL) {
        ESP_LOGE(TAG, "DNS buffers alloc failed");
        heap_caps_free(rx_buf);
        heap_caps_free(tx_buf);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed: errno %d", errno);
        heap_caps_free(rx_buf);
        heap_caps_free(tx_buf);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: errno %d", errno);
        close(sock);
        heap_caps_free(rx_buf);
        heap_caps_free(tx_buf);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS started on UDP/53 (catch-all A -> " IPSTR ")", IP2STR(&(esp_ip4_addr_t){ .addr = ap_ip }));

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buf, DNS_MAX_LEN, 0, (struct sockaddr *)&source_addr, &socklen);
        if (len <= 0) {
            continue;
        }

        int reply_len = build_dns_reply(rx_buf, len, tx_buf, DNS_MAX_LEN, ap_ip);
        if (reply_len > 0) {
            sendto(sock, tx_buf, (size_t)reply_len, 0, (struct sockaddr *)&source_addr, socklen);
        } else {
            ESP_LOGD(TAG, "DNS recv %d bytes, no reply", len);
        }
    }
}

static esp_err_t start_dns_server(uint32_t ap_ip)
{
    if (s_dns_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(dns_server_task, "dns_portal", DNS_TASK_STACK,
                                (void *)(uintptr_t)ap_ip, 5, &s_dns_task);
    if (ok != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGE(TAG, "Failed to create DNS task (free heap=%u, internal=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t set_dhcp_portal_options(esp_netif_t *ap_netif, const esp_netif_ip_info_t *ip_info)
{
    snprintf(s_captiveportal_uri, sizeof(s_captiveportal_uri), "http://" IPSTR, IP2STR(&ip_info->ip));

    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_captiveportal_uri, strlen(s_captiveportal_uri));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP captive portal URI failed: %s", esp_err_to_name(err));
        esp_netif_dhcps_start(ap_netif);
        return err;
    }
    ESP_LOGI(TAG, "DHCP option 114 (captive portal): %s", s_captiveportal_uri);

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info->ip.addr;
    err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP DNS server IP failed: %s", esp_err_to_name(err));
        esp_netif_dhcps_start(ap_netif);
        return err;
    }
    ESP_LOGI(TAG, "DHCP DNS server: " IPSTR, IP2STR(&ip_info->ip));

    uint8_t offer_dns = 1;
    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP DNS offer flag failed: %s", esp_err_to_name(err));
    }

    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "dhcps_start: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    const size_t uri_count = sizeof(s_portal_uris) / sizeof(s_portal_uris[0]);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = (uint16_t)(uri_count + 8);
    /* LWIP_MAX_SOCKETS=10 → httpd allows at most 7 (3 reserved internally). */
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < uri_count; i++) {
        ret = httpd_register_uri_handler(s_httpd, &s_portal_uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s", s_portal_uris[i].uri);
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return ret;
        }
    }

    ret = httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, portal_not_found_handler);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "404 handler not registered: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "HTTP config portal on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t config_portal_start(void)
{
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "Starting config portal (free heap=%u bytes)",
             (unsigned)esp_get_free_heap_size());

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ESP_LOGE(TAG, "WIFI_AP_DEF netif not found");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (ret != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGE(TAG, "AP has no IP yet");
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "SoftAP IP: " IPSTR, IP2STR(&ip_info.ip));

    ret = set_dhcp_portal_options(ap_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = start_dns_server(ip_info.ip.addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Captive DNS unavailable; open http://" IPSTR " manually", IP2STR(&ip_info.ip));
    }

    ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s (free heap=%u)",
                 esp_err_to_name(ret), (unsigned)esp_get_free_heap_size());
        return ret;
    }

    if (s_dns_task == NULL) {
        ESP_LOGW(TAG, "Config portal HTTP ready without DNS (captive auto-open disabled)");
    } else {
        ESP_LOGI(TAG, "Config portal ready (free heap=%u)", (unsigned)esp_get_free_heap_size());
    }
    return ESP_OK;
}
