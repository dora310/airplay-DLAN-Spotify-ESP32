#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Persistent settings storage (NVS)
 */

// Default name advertised to AirPlay, DLNA and other network clients.
#define SETTINGS_DEFAULT_DEVICE_NAME "AirPlay and DLNA"

// Fixed product name displayed in the System information panel.
#define SETTINGS_SYSTEM_DEVICE_NAME "AirPlay and DLNA Receiver"
#define SETTINGS_SCHEMA_VERSION 4

/**
 * Initialize settings module (call once at startup)
 */
esp_err_t settings_init(void);

/** Version of the NVS layout after automatic migration. */
uint32_t settings_schema_version(void);

/**
 * Get saved volume in dB
 * @param volume_db Output: volume in dB (0 = max, -30 = mute)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_volume(float *volume_db);

/**
 * Apply volume (updates cached value and DAC, does NOT write to NVS).
 * @param volume_db Volume in dB (0 = max, -30 = mute)
 */
esp_err_t settings_set_volume(float volume_db);

/**
 * Persist the current cached volume to NVS.
 * Call once at session disconnect rather than on every change.
 */
esp_err_t settings_persist_volume(void);

#ifdef CONFIG_BT_A2DP_ENABLE
/**
 * Get saved Bluetooth volume (AVRC 0-127 scale).
 * @param volume Output: 0 (mute) to 127 (max)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_bt_volume(uint8_t *volume);

/**
 * Update cached Bluetooth volume (does NOT write to NVS).
 * Caller is responsible for calling dac_set_volume().
 * @param volume 0 (mute) to 127 (max)
 */
esp_err_t settings_set_bt_volume(uint8_t volume);

/**
 * Persist the current cached BT volume to NVS.
 * Call once at session disconnect rather than on every change.
 */
esp_err_t settings_persist_bt_volume(void);
#endif

/**
 * Get saved WiFi SSID
 * @param ssid Output buffer for SSID
 * @param len Size of SSID buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_wifi_ssid(char *ssid, size_t len);

/**
 * Get saved WiFi password
 * @param password Output buffer for password
 * @param len Size of password buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_wifi_password(char *password, size_t len);

/**
 * Save WiFi credentials to persistent storage
 * @param ssid WiFi SSID
 * @param password WiFi password
 */
esp_err_t settings_set_wifi_credentials(const char *ssid, const char *password);

/** Stage credentials for a transactional connection test on the next boot. */
esp_err_t settings_set_pending_wifi_credentials(const char *ssid,
                                                const char *password);
bool settings_has_pending_wifi_credentials(void);
esp_err_t settings_get_pending_wifi_credentials(char *ssid, size_t ssid_len,
                                                char *password,
                                                size_t password_len);
esp_err_t settings_promote_pending_wifi_credentials(void);
esp_err_t settings_clear_pending_wifi_credentials(void);

/**
 * Check if WiFi credentials are stored
 * @return true if credentials exist, false otherwise
 */
bool settings_has_wifi_credentials(void);

/**
 * Get device name (returns default if none saved)
 * @param name Output buffer for device name
 * @param len Size of name buffer
 * @return ESP_OK (always returns a valid name)
 */
esp_err_t settings_get_device_name(char *name, size_t len);

/**
 * Save device name to persistent storage
 * @param name Device name
 */
esp_err_t settings_set_device_name(const char *name);

// ---- LED settings ----

/**
 * Get saved LED brightness (0–255). Returns compile-time default if not set.
 */
esp_err_t settings_get_led_brightness(uint8_t *brightness);

/**
 * Save LED brightness (0–255) to persistent storage.
 */
esp_err_t settings_set_led_brightness(uint8_t brightness);

// ---- EQ settings ----

/** Number of EQ bands stored in NVS */
#define SETTINGS_EQ_BANDS 15

/**
 * Get saved EQ gains.
 * @param gains_db Output array of SETTINGS_EQ_BANDS floats
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved EQ
 */
esp_err_t settings_get_eq_gains(float gains_db[SETTINGS_EQ_BANDS]);

/**
 * Save EQ gains to persistent storage.
 * @param gains_db Array of SETTINGS_EQ_BANDS floats (dB)
 */
esp_err_t settings_set_eq_gains(const float gains_db[SETTINGS_EQ_BANDS]);

/**
 * Clear saved EQ (revert to flat on next boot).
 */
esp_err_t settings_clear_eq(void);

/**
 * Check if EQ gains are saved.
 */
bool settings_has_eq(void);

/** Web/API password. Only a SHA-256 digest is stored in NVS. */
esp_err_t settings_set_web_password(const char *password);
bool settings_web_password_is_set(void);
bool settings_verify_web_password(const char *password);

/** Export/import the stored password digest for authenticated backups. */
esp_err_t settings_get_web_password_digest(uint8_t digest[32]);
esp_err_t settings_set_web_password_digest(const uint8_t digest[32]);

// ---- Maintenance and user-interface preferences ----

typedef enum {
  SETTINGS_THEME_AUTO = 0,
  SETTINGS_THEME_DARK,
  SETTINGS_THEME_LIGHT,
} settings_theme_t;

typedef struct {
  bool speaker_protection_enabled;
  uint8_t speaker_threshold_percent; /* 50..98 % of full scale */
  bool scheduled_restart_enabled;
  uint16_t scheduled_restart_hours; /* uptime interval; restart waits for idle */
  settings_theme_t theme;
} settings_maintenance_t;

esp_err_t settings_get_maintenance(settings_maintenance_t *config);
esp_err_t settings_set_maintenance(const settings_maintenance_t *config);

// ---- MQTT / Home Assistant ----

#define SETTINGS_MQTT_URI_LEN 128
#define SETTINGS_MQTT_USER_LEN 64
#define SETTINGS_MQTT_PASSWORD_LEN 96
#define SETTINGS_MQTT_TOPIC_LEN 96

typedef struct {
  bool enabled;
  bool home_assistant_discovery;
  char broker_uri[SETTINGS_MQTT_URI_LEN];
  char username[SETTINGS_MQTT_USER_LEN];
  char password[SETTINGS_MQTT_PASSWORD_LEN];
  char topic_prefix[SETTINGS_MQTT_TOPIC_LEN];
} settings_mqtt_t;

esp_err_t settings_get_mqtt(settings_mqtt_t *config);
esp_err_t settings_set_mqtt(const settings_mqtt_t *config);

/** Erase all receiver settings, including Wi-Fi and the access password. */
esp_err_t settings_factory_reset(void);

// ---- Internet-radio presets ----

#define SETTINGS_RADIO_PRESET_COUNT 8
#define SETTINGS_RADIO_PRESET_NAME_LEN 32
#define SETTINGS_RADIO_PRESET_URL_LEN 256

typedef struct {
  char name[SETTINGS_RADIO_PRESET_NAME_LEN];
  char url[SETTINGS_RADIO_PRESET_URL_LEN];
} settings_radio_preset_t;

/** Read one preset slot. Empty/unset slots return ESP_ERR_NOT_FOUND. */
esp_err_t settings_get_radio_preset(uint8_t slot,
                                    settings_radio_preset_t *preset);

/**
 * Save one preset slot. Passing an empty URL clears that slot.
 * Slots are zero based and must be below SETTINGS_RADIO_PRESET_COUNT.
 */
esp_err_t settings_set_radio_preset(uint8_t slot, const char *name,
                                    const char *url);
