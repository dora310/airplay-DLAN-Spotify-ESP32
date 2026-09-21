#include "settings.h"

#include "dac.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "settings";

#define NVS_NAMESPACE  "airplay"
#define NVS_KEY_VOLUME "volume_db"
#ifdef CONFIG_BT_A2DP_ENABLE
#define NVS_KEY_BT_VOLUME "bt_vol"
#endif
#define NVS_KEY_WIFI_SSID      "wifi_ssid"
#define NVS_KEY_WIFI_PASSWORD  "wifi_pass"
#define NVS_KEY_DEVICE_NAME    "device_name"
#define NVS_KEY_EQ_GAINS       "eq_gains"
#define NVS_KEY_LED_BRIGHTNESS "led_bright"
#define NVS_KEY_WEB_PASSWORD   "web_pw_sha"
#define NVS_KEY_MAINTENANCE    "maint_v1"
#define NVS_KEY_SCHEMA         "cfg_schema"
#define NVS_KEY_MQTT           "mqtt_v1"
#define NVS_KEY_PENDING_SSID   "wifi_test_s"
#define NVS_KEY_PENDING_PASS   "wifi_test_p"

#define MAX_WIFI_SSID_LEN     32
#define MAX_WIFI_PASSWORD_LEN 64
#define MAX_DEVICE_NAME_LEN   64

// Cached values  (defaults = 50 %)
static float g_volume_db = -15.0f;
static bool g_volume_loaded = false;

#ifdef CONFIG_BT_A2DP_ENABLE
static uint8_t g_bt_volume = 64; /* default: 50 % */
static bool g_bt_volume_loaded = false;
#endif

static float g_eq_gains[SETTINGS_EQ_BANDS];
static bool g_eq_loaded = false;
static uint32_t g_schema_version;

static esp_err_t migrate_settings(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;

  uint32_t from = 0;
  err = nvs_get_u32(nvs, NVS_KEY_SCHEMA, &from);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    from = 0;
    err = ESP_OK;
  }
  if (err != ESP_OK) {
    nvs_close(nvs);
    return err;
  }
  if (from > SETTINGS_SCHEMA_VERSION) {
    ESP_LOGW(TAG, "Settings schema %lu is newer than firmware schema %u",
             (unsigned long)from, SETTINGS_SCHEMA_VERSION);
    g_schema_version = from;
    nvs_close(nvs);
    return ESP_OK;
  }

  if (from < SETTINGS_SCHEMA_VERSION) {
    /* Migrations are deliberately additive. Existing Wi-Fi, device, EQ,
       radio, password and maintenance keys remain untouched. New settings
       use safe defaults when their versioned blob is absent. */
    ESP_LOGI(TAG, "Migrating settings schema %lu -> %u",
             (unsigned long)from, SETTINGS_SCHEMA_VERSION);
    err = nvs_set_u32(nvs, NVS_KEY_SCHEMA, SETTINGS_SCHEMA_VERSION);
    if (err == ESP_OK) err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  if (err == ESP_OK) g_schema_version = SETTINGS_SCHEMA_VERSION;
  return err;
}

esp_err_t settings_init(void) {
  esp_err_t migration = migrate_settings();
  if (migration != ESP_OK) {
    ESP_LOGE(TAG, "Settings migration failed: %s",
             esp_err_to_name(migration));
    return migration;
  }
  // Load volume on init
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    int32_t vol_fixed;
    err = nvs_get_i32(nvs, NVS_KEY_VOLUME, &vol_fixed);
    if (err == ESP_OK) {
      g_volume_db = (float)vol_fixed / 100.0f;
      g_volume_loaded = true;
      ESP_LOGI(TAG, "Loaded volume: %.2f dB", g_volume_db);
    }

    /* Load EQ gains blob */
    size_t eq_size = sizeof(g_eq_gains);
    err = nvs_get_blob(nvs, NVS_KEY_EQ_GAINS, g_eq_gains, &eq_size);
    if (err == ESP_OK && eq_size == sizeof(g_eq_gains)) {
      g_eq_loaded = true;
      ESP_LOGI(TAG, "Loaded EQ gains (%d bands)", SETTINGS_EQ_BANDS);
    }

    nvs_close(nvs);
  }

  return ESP_OK;
}

uint32_t settings_schema_version(void) { return g_schema_version; }

esp_err_t settings_get_volume(float *volume_db) {
  if (!volume_db) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!g_volume_loaded) {
    return ESP_ERR_NOT_FOUND;
  }

  *volume_db = g_volume_db;
  return ESP_OK;
}

esp_err_t settings_set_volume(float volume_db) {
  // Skip if unchanged
  if (g_volume_loaded && volume_db == g_volume_db) {
    return ESP_OK;
  }

  dac_set_volume(volume_db);

  g_volume_db = volume_db;
  g_volume_loaded = true;
  return ESP_OK;
}

esp_err_t settings_persist_volume(void) {
  if (!g_volume_loaded) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  int32_t vol_fixed = (int32_t)(g_volume_db * 100.0f);
  err = nvs_set_i32(nvs, NVS_KEY_VOLUME, vol_fixed);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Persisted volume: %.2f dB", g_volume_db);
  } else {
    ESP_LOGE(TAG, "Failed to persist volume: %s", esp_err_to_name(err));
  }

  return err;
}

#ifdef CONFIG_BT_A2DP_ENABLE
esp_err_t settings_get_bt_volume(uint8_t *volume) {
  if (!volume) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!g_bt_volume_loaded) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
      return err;
    }
    err = nvs_get_u8(nvs, NVS_KEY_BT_VOLUME, &g_bt_volume);
    nvs_close(nvs);
    if (err != ESP_OK) {
      return err;
    }
    g_bt_volume_loaded = true;
  }
  *volume = g_bt_volume;
  return ESP_OK;
}

esp_err_t settings_set_bt_volume(uint8_t volume) {
  if (g_bt_volume_loaded && volume == g_bt_volume) {
    return ESP_OK;
  }

  g_bt_volume = volume;
  g_bt_volume_loaded = true;
  return ESP_OK;
}

esp_err_t settings_persist_bt_volume(void) {
  if (!g_bt_volume_loaded) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_BT_VOLUME, g_bt_volume);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Persisted BT volume: %d/127", g_bt_volume);
  } else {
    ESP_LOGE(TAG, "Failed to persist BT volume: %s", esp_err_to_name(err));
  }
  return err;
}
#endif

esp_err_t settings_get_wifi_ssid(char *ssid, size_t len) {
  if (!ssid || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  size_t required_size = len;
  err = nvs_get_str(nvs, NVS_KEY_WIFI_SSID, ssid, &required_size);
  nvs_close(nvs);

  if (err == ESP_OK && required_size > len) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  return err;
}

esp_err_t settings_get_wifi_password(char *password, size_t len) {
  if (!password || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  size_t required_size = len;
  err = nvs_get_str(nvs, NVS_KEY_WIFI_PASSWORD, password, &required_size);
  nvs_close(nvs);

  if (err == ESP_OK && required_size > len) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  return err;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  if (!ssid || strlen(ssid) == 0 || strlen(ssid) > MAX_WIFI_SSID_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!password || strlen(password) > MAX_WIFI_PASSWORD_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_WIFI_PASSWORD, password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved WiFi credentials: SSID=%s", ssid);
  } else {
    ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
  }

  return err;
}

esp_err_t settings_set_pending_wifi_credentials(const char *ssid,
                                                const char *password) {
  if (!ssid || !ssid[0] || strlen(ssid) > MAX_WIFI_SSID_LEN || !password ||
      strlen(password) > MAX_WIFI_PASSWORD_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_str(nvs, NVS_KEY_PENDING_SSID, ssid);
  if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_PENDING_PASS, password);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Staged WiFi credentials for transactional test: %s", ssid);
  }
  return err;
}

bool settings_has_pending_wifi_credentials(void) {
  char ssid[MAX_WIFI_SSID_LEN + 1] = {0};
  char password[MAX_WIFI_PASSWORD_LEN + 1] = {0};
  return settings_get_pending_wifi_credentials(
             ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK &&
         ssid[0] != '\0';
}

esp_err_t settings_get_pending_wifi_credentials(char *ssid, size_t ssid_len,
                                                char *password,
                                                size_t password_len) {
  if (!ssid || !ssid_len || !password || !password_len)
    return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
  size_t size = ssid_len;
  err = nvs_get_str(nvs, NVS_KEY_PENDING_SSID, ssid, &size);
  if (err == ESP_OK) {
    size = password_len;
    err = nvs_get_str(nvs, NVS_KEY_PENDING_PASS, password, &size);
  }
  nvs_close(nvs);
  return err;
}

esp_err_t settings_clear_pending_wifi_credentials(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  esp_err_t a = nvs_erase_key(nvs, NVS_KEY_PENDING_SSID);
  esp_err_t b = nvs_erase_key(nvs, NVS_KEY_PENDING_PASS);
  if (a != ESP_OK && a != ESP_ERR_NVS_NOT_FOUND) err = a;
  else if (b != ESP_OK && b != ESP_ERR_NVS_NOT_FOUND) err = b;
  else err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

esp_err_t settings_promote_pending_wifi_credentials(void) {
  char ssid[MAX_WIFI_SSID_LEN + 1] = {0};
  char password[MAX_WIFI_PASSWORD_LEN + 1] = {0};
  esp_err_t err = settings_get_pending_wifi_credentials(
      ssid, sizeof(ssid), password, sizeof(password));
  if (err != ESP_OK) return err;
  err = settings_set_wifi_credentials(ssid, password);
  if (err == ESP_OK) err = settings_clear_pending_wifi_credentials();
  if (err == ESP_OK) ESP_LOGI(TAG, "WiFi credential test passed; settings committed");
  return err;
}

bool settings_has_wifi_credentials(void) {
  char ssid[MAX_WIFI_SSID_LEN + 1] = {0};
  return settings_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK && ssid[0] != '\0';
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (!name || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t required_size = len;
    err = nvs_get_str(nvs, NVS_KEY_DEVICE_NAME, name, &required_size);
    nvs_close(nvs);

    if (err == ESP_OK && required_size <= len) {
      // Migrate the original firmware default without overwriting genuine
      // custom names. This also works for OTA updates that preserve NVS.
      if (strcmp(name, "ESP32 AirPlay") == 0) {
        strlcpy(name, SETTINGS_DEFAULT_DEVICE_NAME, len);
      }
      return ESP_OK;
    }
  }

  // Return default if not found or error
  strncpy(name, SETTINGS_DEFAULT_DEVICE_NAME, len - 1);
  name[len - 1] = '\0';
  return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name) {
  if (!name || strlen(name) == 0 || strlen(name) > MAX_DEVICE_NAME_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs, NVS_KEY_DEVICE_NAME, name);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved device name: %s", name);
  } else {
    ESP_LOGE(TAG, "Failed to save device name: %s", esp_err_to_name(err));
  }

  return err;
}

/* ================================================================== */
/*  LED Brightness                                                     */
/* ================================================================== */

esp_err_t settings_get_led_brightness(uint8_t *brightness) {
  if (!brightness) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  err = nvs_get_u8(nvs, NVS_KEY_LED_BRIGHTNESS, brightness);
  nvs_close(nvs);
  return err;
}

esp_err_t settings_set_led_brightness(uint8_t brightness) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_LED_BRIGHTNESS, brightness);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved LED brightness: %d", brightness);
  } else {
    ESP_LOGE(TAG, "Failed to save LED brightness: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  EQ Gains                                                           */
/* ================================================================== */

esp_err_t settings_get_eq_gains(float gains_db[SETTINGS_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!g_eq_loaded) {
    return ESP_ERR_NOT_FOUND;
  }

  memcpy(gains_db, g_eq_gains, sizeof(g_eq_gains));
  return ESP_OK;
}

esp_err_t settings_set_eq_gains(const float gains_db[SETTINGS_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Skip write if unchanged (compare element-by-element to avoid
     memcmp on floats, which is flagged by
     bugprone-suspicious-memory-comparison) */
  if (g_eq_loaded) {
    bool unchanged = true;
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      if (gains_db[i] != g_eq_gains[i]) {
        unchanged = false;
        break;
      }
    }
    if (unchanged) {
      return ESP_OK;
    }
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_blob(nvs, NVS_KEY_EQ_GAINS, gains_db,
                     sizeof(float) * SETTINGS_EQ_BANDS);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    memcpy(g_eq_gains, gains_db, sizeof(g_eq_gains));
    g_eq_loaded = true;
    ESP_LOGI(TAG, "Saved EQ gains (%d bands)", SETTINGS_EQ_BANDS);
  } else {
    ESP_LOGE(TAG, "Failed to save EQ gains: %s", esp_err_to_name(err));
  }

  return err;
}

esp_err_t settings_clear_eq(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_erase_key(nvs, NVS_KEY_EQ_GAINS);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_commit(nvs);
    memset(g_eq_gains, 0, sizeof(g_eq_gains));
    g_eq_loaded = false;
    err = ESP_OK;
  }

  nvs_close(nvs);
  return err;
}

bool settings_has_eq(void) {
  return g_eq_loaded;
}

static void password_digest(const char *password, uint8_t digest[32]) {
  static const char domain[] = "airplay-esp32-web-v1:";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)domain,
                        sizeof(domain) - 1);
  mbedtls_sha256_update(&ctx, (const unsigned char *)password,
                        strlen(password));
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
}

esp_err_t settings_set_web_password(const char *password) {
  if (!password || (password[0] && strlen(password) < 8) ||
      strlen(password) > 64) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  if (!password[0]) {
    err = nvs_erase_key(nvs, NVS_KEY_WEB_PASSWORD);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  } else {
    uint8_t digest[32];
    password_digest(password, digest);
    err = nvs_set_blob(nvs, NVS_KEY_WEB_PASSWORD, digest, sizeof(digest));
  }
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

bool settings_web_password_is_set(void) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
  size_t size = 0;
  esp_err_t err = nvs_get_blob(nvs, NVS_KEY_WEB_PASSWORD, NULL, &size);
  nvs_close(nvs);
  return err == ESP_OK && size == 32;
}

bool settings_verify_web_password(const char *password) {
  if (!settings_web_password_is_set()) return true;
  if (!password) return false;
  uint8_t expected[32], actual[32];
  size_t size = sizeof(expected);
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
  esp_err_t err = nvs_get_blob(nvs, NVS_KEY_WEB_PASSWORD, expected, &size);
  nvs_close(nvs);
  if (err != ESP_OK || size != sizeof(expected)) return false;
  password_digest(password, actual);
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(expected); i++) difference |= expected[i] ^ actual[i];
  return difference == 0;
}

esp_err_t settings_get_web_password_digest(uint8_t digest[32]) {
  if (!digest) return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return err;
  size_t size = 32;
  err = nvs_get_blob(nvs, NVS_KEY_WEB_PASSWORD, digest, &size);
  nvs_close(nvs);
  return err == ESP_OK && size == 32 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t settings_set_web_password_digest(const uint8_t digest[32]) {
  if (!digest) return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, NVS_KEY_WEB_PASSWORD, digest, 32);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

static void maintenance_defaults(settings_maintenance_t *config) {
  memset(config, 0, sizeof(*config));
  config->speaker_threshold_percent = 90;
  config->scheduled_restart_hours = 24;
  config->theme = SETTINGS_THEME_AUTO;
}

static void maintenance_sanitize(settings_maintenance_t *config) {
  if (config->speaker_threshold_percent < 50 ||
      config->speaker_threshold_percent > 98) {
    config->speaker_threshold_percent = 90;
  }
  if (config->scheduled_restart_hours < 1 ||
      config->scheduled_restart_hours > 720) {
    config->scheduled_restart_hours = 24;
  }
  if (config->theme < SETTINGS_THEME_AUTO ||
      config->theme > SETTINGS_THEME_LIGHT) {
    config->theme = SETTINGS_THEME_AUTO;
  }
}

esp_err_t settings_get_maintenance(settings_maintenance_t *config) {
  if (!config) return ESP_ERR_INVALID_ARG;
  maintenance_defaults(config);
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return ESP_OK;
  size_t size = sizeof(*config);
  settings_maintenance_t saved;
  err = nvs_get_blob(nvs, NVS_KEY_MAINTENANCE, &saved, &size);
  nvs_close(nvs);
  if (err == ESP_OK && size == sizeof(saved)) *config = saved;
  maintenance_sanitize(config);
  return ESP_OK;
}

esp_err_t settings_set_maintenance(const settings_maintenance_t *config) {
  if (!config) return ESP_ERR_INVALID_ARG;
  settings_maintenance_t value = *config;
  maintenance_sanitize(&value);
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, NVS_KEY_MAINTENANCE, &value, sizeof(value));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

static void mqtt_defaults(settings_mqtt_t *config) {
  memset(config, 0, sizeof(*config));
  config->home_assistant_discovery = true;
  strlcpy(config->broker_uri, "mqtt://192.168.1.2",
          sizeof(config->broker_uri));
  strlcpy(config->topic_prefix, "airplay-dlna",
          sizeof(config->topic_prefix));
#ifdef CONFIG_MQTT_CONTROL_ENABLE
  config->enabled = true;
#ifdef CONFIG_MQTT_BROKER_URI
  strlcpy(config->broker_uri, CONFIG_MQTT_BROKER_URI,
          sizeof(config->broker_uri));
#endif
#ifdef CONFIG_MQTT_TOPIC_PREFIX
  strlcpy(config->topic_prefix, CONFIG_MQTT_TOPIC_PREFIX,
          sizeof(config->topic_prefix));
#endif
#endif
}

static bool mqtt_valid(const settings_mqtt_t *config) {
  if (!config) return false;
  if (config->enabled &&
      (strncmp(config->broker_uri, "mqtt://", 7) != 0 &&
       strncmp(config->broker_uri, "mqtts://", 8) != 0)) return false;
  return config->topic_prefix[0] != '\0' &&
         strlen(config->topic_prefix) < SETTINGS_MQTT_TOPIC_LEN;
}

esp_err_t settings_get_mqtt(settings_mqtt_t *config) {
  if (!config) return ESP_ERR_INVALID_ARG;
  mqtt_defaults(config);
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return ESP_OK;
  settings_mqtt_t saved;
  size_t size = sizeof(saved);
  err = nvs_get_blob(nvs, NVS_KEY_MQTT, &saved, &size);
  nvs_close(nvs);
  if (err == ESP_OK && size == sizeof(saved) && mqtt_valid(&saved)) {
    *config = saved;
  }
  return ESP_OK;
}

esp_err_t settings_set_mqtt(const settings_mqtt_t *config) {
  if (!mqtt_valid(config)) return ESP_ERR_INVALID_ARG;
  settings_mqtt_t value = *config;
  value.broker_uri[sizeof(value.broker_uri) - 1] = '\0';
  value.username[sizeof(value.username) - 1] = '\0';
  value.password[sizeof(value.password) - 1] = '\0';
  value.topic_prefix[sizeof(value.topic_prefix) - 1] = '\0';
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(nvs, NVS_KEY_MQTT, &value, sizeof(value));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

esp_err_t settings_factory_reset(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_erase_all(nvs);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err == ESP_OK) {
    g_volume_db = -15.0f;
    g_volume_loaded = false;
    memset(g_eq_gains, 0, sizeof(g_eq_gains));
    g_eq_loaded = false;
  }
  return err;
}

esp_err_t settings_get_radio_preset(uint8_t slot,
                                    settings_radio_preset_t *preset) {
  if (!preset || slot >= SETTINGS_RADIO_PRESET_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(preset, 0, sizeof(*preset));

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

  char name_key[12];
  char url_key[12];
  snprintf(name_key, sizeof(name_key), "rp%u_name", (unsigned)slot);
  snprintf(url_key, sizeof(url_key), "rp%u_url", (unsigned)slot);

  size_t name_size = sizeof(preset->name);
  size_t url_size = sizeof(preset->url);
  esp_err_t name_err = nvs_get_str(nvs, name_key, preset->name, &name_size);
  err = nvs_get_str(nvs, url_key, preset->url, &url_size);
  nvs_close(nvs);

  if (err != ESP_OK || preset->url[0] == '\0') {
    memset(preset, 0, sizeof(*preset));
    return ESP_ERR_NOT_FOUND;
  }
  if (name_err != ESP_OK || preset->name[0] == '\0') {
    snprintf(preset->name, sizeof(preset->name), "Preset %u",
             (unsigned)slot + 1);
  }
  return ESP_OK;
}

esp_err_t settings_set_radio_preset(uint8_t slot, const char *name,
                                    const char *url) {
  if (slot >= SETTINGS_RADIO_PRESET_COUNT || !name || !url ||
      strlen(name) >= SETTINGS_RADIO_PRESET_NAME_LEN ||
      strlen(url) >= SETTINGS_RADIO_PRESET_URL_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;

  char name_key[12];
  char url_key[12];
  snprintf(name_key, sizeof(name_key), "rp%u_name", (unsigned)slot);
  snprintf(url_key, sizeof(url_key), "rp%u_url", (unsigned)slot);

  if (url[0] == '\0') {
    esp_err_t name_err = nvs_erase_key(nvs, name_key);
    esp_err_t url_err = nvs_erase_key(nvs, url_key);
    if (name_err != ESP_OK && name_err != ESP_ERR_NVS_NOT_FOUND) err = name_err;
    else if (url_err != ESP_OK && url_err != ESP_ERR_NVS_NOT_FOUND) err = url_err;
    else err = ESP_OK;
  } else {
    err = nvs_set_str(nvs, name_key, name[0] ? name : "Radio preset");
    if (err == ESP_OK) err = nvs_set_str(nvs, url_key, url);
  }
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}
