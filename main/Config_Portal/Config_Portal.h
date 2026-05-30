#pragma once

#include "esp_err.h"

/**
 * Start captive-portal DNS (UDP/53) and HTTP server on the default WiFi AP netif.
 * Call after esp_wifi_start() in AP mode.
 */
esp_err_t config_portal_start(void);
