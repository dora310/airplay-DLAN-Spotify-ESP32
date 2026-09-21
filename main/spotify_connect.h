#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the experimental Spotify Connect service on the existing web server.
 */
esp_err_t spotify_connect_start(httpd_handle_t server, uint16_t port,
                                const char *device_name);

/** True after a Spotify controller has selected this receiver. */
bool spotify_connect_is_active(void);

#ifdef __cplusplus
}
#endif
