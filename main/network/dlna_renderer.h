#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

/** Register the UPnP/DLNA MediaRenderer on the existing web server. */
esp_err_t dlna_renderer_register(httpd_handle_t server, uint16_t server_port);

/** Stop discovery and any active DLNA stream. */
void dlna_renderer_stop(void);

/**
 * Give AirPlay exclusive priority while an AirPlay client owns the session.
 * Entering the active state stops DLNA playback before AirPlay starts writing
 * to I2S. Leaving it resumes DLNA discovery, but never auto-resumes a stream.
 */
void dlna_renderer_set_airplay_active(bool active);

/** True while a DLNA media stream is actively decoding or paused. */
bool dlna_renderer_is_playing(void);

/** Local controls used by hardware buttons. */
void dlna_renderer_toggle_pause(void);
void dlna_renderer_toggle_mute(void);
void dlna_renderer_set_muted(bool muted);
bool dlna_renderer_is_muted(void);
void dlna_renderer_volume_step(int percent);

/** Play an HTTP(S) radio/media URL through the shared DLNA decoder. */
esp_err_t dlna_renderer_play_uri(const char *uri);
void dlna_renderer_stop_playback(void);
const char *dlna_renderer_current_uri(void);
