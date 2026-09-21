#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Web server for control panel
 * Provides:
 * - WiFi configuration
 * - Device name configuration
 * - OTA update
 */

/**
 * Initialize and start the web server
 * @param port HTTP server port (default: 80)
 */
esp_err_t web_server_start(uint16_t port);

/**
 * Stop the web server
 */
void web_server_stop(void);

/** Return the shared HTTP server so DLNA can register its endpoints. */
httpd_handle_t web_server_get_handle(void);
