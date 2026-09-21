#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  LED_OFF,
  LED_STEADY,
  LED_BLINK_SLOW,   // 100ms on, 2500ms off (standby)
  LED_BLINK_MEDIUM, // 500ms on/off (paused)
  LED_BLINK_FAST,   // 250ms on/off
  LED_VU,           // Audio visualization
} led_mode_t;

typedef enum {
  LED_WIFI_DISCONNECTED,
  LED_WIFI_CONNECTING,
  LED_WIFI_CONNECTED,
} led_wifi_state_t;

/**
 * Initialize LED subsystem and register for RTSP events.
 */
void led_init(void);

/**
 * Feed audio samples for VU meter mode.
 * Call this from the audio path when playing.
 */
void led_audio_feed(const int16_t *pcm, size_t stereo_samples);

/**
 * Set error state (e.g., speaker fault, decode failure).
 * Clears automatically on next playback state change.
 */
void led_set_error(bool error);

/**
 * Update the RGB LED's Wi-Fi indication. Playback has priority and temporarily
 * replaces this indication with the audio-reactive spectrum.
 */
void led_set_wifi_state(led_wifi_state_t state);

/**
 * Set LED brightness (0–255). Persists to NVS and takes effect immediately.
 */
esp_err_t led_set_brightness(uint8_t brightness);

/**
 * Get current LED brightness (0–255).
 */
uint8_t led_get_brightness(void);
