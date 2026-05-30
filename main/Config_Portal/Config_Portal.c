#include "Config_Portal.h"

#include <errno.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "Config_Portal";

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

/** Must outlive the DHCP server (see esp_netif_dhcps_option). */
static char s_captiveportal_uri[48];

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task = NULL;

#define DNS_PORT 53
#define DNS_MAX_LEN 512
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
    default:
        return "OTHER";
    }
}

static void log_http_request(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP %s %s", portal_http_method_str(req->method), req->uri);
}

static esp_err_t portal_send_static(httpd_req_t *req, const portal_static_file_t *file)
{
    log_http_request(req);
    httpd_resp_set_type(req, file->content_type);
    return httpd_resp_send(req, file->start, (size_t)(file->end - file->start));
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
                           (size_t)(service_html_end - service_html_start));
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

            ESP_LOGI(TAG, "DNS answer %s A -> " IPSTR, name, IP2STR(&(esp_ip4_addr_t){ .addr = ap_ip }));
        } else {
            needs_empty_reply = true;
            ESP_LOGI(TAG, "DNS query %s %s (NOERROR, no answer)", name, dns_qtype_str(qtype));
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
    uint8_t rx_buf[DNS_MAX_LEN];
    uint8_t tx_buf[DNS_MAX_LEN];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed: errno %d", errno);
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
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS started on UDP/53 (catch-all A -> " IPSTR ")", IP2STR(&(esp_ip4_addr_t){ .addr = ap_ip }));

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len <= 0) {
            continue;
        }

        int reply_len = build_dns_reply(rx_buf, len, tx_buf, sizeof(tx_buf), ap_ip);
        if (reply_len > 0) {
            sendto(sock, tx_buf, (size_t)reply_len, 0, (struct sockaddr *)&source_addr, socklen);
        } else {
            ESP_LOGD(TAG, "DNS recv %d bytes, no reply", len);
        }
    }
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
    config.max_uri_handlers = (uint16_t)(uri_count + 2);
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

    ret = start_http_server();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_dns_task == NULL) {
        BaseType_t ok = xTaskCreate(dns_server_task, "dns_portal", 4096,
                                    (void *)(uintptr_t)ip_info.ip.addr, 5, &s_dns_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create DNS task");
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
