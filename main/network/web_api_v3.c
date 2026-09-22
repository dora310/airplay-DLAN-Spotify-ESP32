#include "web_api_v3.h"

#include "audio/software_dsp.h"
#include "audio/audio_test.h"
#include "cJSON.h"
#include "dlna_renderer.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "log_stream.h"
#include "maintenance.h"
#include "mqtt_control.h"
#include "github_ota.h"
#include "recovery.h"
#include "playback_control.h"
#include "settings.h"
#include "source_manager.h"
#include "system_monitor.h"
#include "wifi.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "web_api_v3";
static esp_timer_handle_t s_sleep_timer;
static volatile uint32_t s_sleep_deadline_seconds;

static uint32_t uptime_seconds(void) {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void sleep_timer_callback(void *arg) {
  (void)arg;
  s_sleep_deadline_seconds = 0;
  playback_source_t source = playback_control_get_source();
  if (source == PLAYBACK_SOURCE_DLNA) {
    dlna_renderer_set_muted(true);
    ESP_LOGI(TAG, "Sleep timer expired; DLNA/radio audio muted");
  } else if (source == PLAYBACK_SOURCE_AIRPLAY &&
             !playback_control_is_muted()) {
    playback_control_toggle_mute();
    ESP_LOGI(TAG, "Sleep timer expired; AirPlay audio muted");
  }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
  char *body = cJSON_PrintUnformatted(json);
  if (!body) return ESP_ERR_NO_MEM;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, body);
  free(body);
  return err;
}

static cJSON *read_json(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 16384) return NULL;
  char *body = calloc(1, req->content_len + 1);
  if (!body) return NULL;
  size_t offset = 0;
  while (offset < req->content_len) {
    int n = httpd_req_recv(req, body + offset, req->content_len - offset);
    if (n <= 0) { free(body); return NULL; }
    offset += (size_t)n;
  }
  cJSON *json = cJSON_Parse(body);
  free(body);
  return json;
}

static bool authorized(httpd_req_t *req) {
  if (!settings_web_password_is_set()) return true;
  size_t len = httpd_req_get_hdr_value_len(req, "X-API-Key");
  if (!len || len > 64) return false;
  char key[65];
  if (httpd_req_get_hdr_value_str(req, "X-API-Key", key, sizeof(key)) != ESP_OK)
    return false;
  return settings_verify_web_password(key);
}

static esp_err_t require_auth(httpd_req_t *req) {
  if (authorized(req)) return ESP_OK;
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"X-API-Key required\"}");
  return ESP_ERR_INVALID_STATE;
}

static esp_err_t status_get(httpd_req_t *req) {
  system_health_t h;
  system_monitor_get(&h);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "success", true);
  cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
  cJSON_AddStringToObject(j, "source",
                          source_manager_name(source_manager_current()));
  cJSON_AddBoolToObject(j, "wifi", wifi_is_connected());
  cJSON_AddNumberToObject(j, "volume", playback_control_get_volume_percent());
  cJSON_AddBoolToObject(j, "muted", playback_control_is_muted());
  cJSON_AddNumberToObject(j, "uptime_seconds", h.uptime_seconds);
  cJSON_AddNumberToObject(j, "free_heap", h.free_heap);
  cJSON_AddBoolToObject(j, "password_set", settings_web_password_is_set());
  cJSON_AddBoolToObject(j, "safe_mode", recovery_is_safe_mode());
  cJSON_AddNumberToObject(j, "settings_schema", settings_schema_version());
  esp_err_t err = send_json(req, j);
  cJSON_Delete(j);
  return err;
}

static esp_err_t health_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  system_health_t h;
  system_monitor_get(&h);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "success", true);
  cJSON_AddNumberToObject(j, "uptime_seconds", h.uptime_seconds);
  cJSON_AddNumberToObject(j, "free_heap", h.free_heap);
  cJSON_AddNumberToObject(j, "minimum_free_heap", h.minimum_free_heap);
  cJSON_AddNumberToObject(j, "largest_internal_block", h.largest_internal_block);
  cJSON_AddNumberToObject(j, "free_psram", h.free_psram);
  cJSON_AddNumberToObject(j, "restart_count", h.restart_count);
  cJSON_AddNumberToObject(j, "reset_reason", h.reset_reason);
  cJSON_AddNumberToObject(j, "low_memory_events", h.low_memory_events);
  cJSON_AddNumberToObject(j, "source_switches", source_manager_switch_count());
  cJSON_AddNumberToObject(j, "limiter_events", software_dsp_limiter_count());
  cJSON_AddNumberToObject(j, "clipping_events", software_dsp_clipping_count());
  cJSON_AddBoolToObject(j, "limiter_active", software_dsp_limiter_active());
  cJSON_AddNumberToObject(j, "diagnostic_bytes_dropped",
                          log_stream_persistent_dropped());
  cJSON_AddBoolToObject(j, "ota_pending_verify",
                        maintenance_ota_pending_verify());
  esp_err_t err = send_json(req, j);
  cJSON_Delete(j);
  return err;
}

static esp_err_t control_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *action = in ? cJSON_GetObjectItem(in, "action") : NULL;
  bool ok = action && cJSON_IsString(action);
  if (ok) {
    const char *a = action->valuestring;
    if (!strcmp(a, "play_pause")) playback_control_play_pause();
    else if (!strcmp(a, "next")) playback_control_next();
    else if (!strcmp(a, "previous")) playback_control_prev();
    else if (!strcmp(a, "volume_up")) playback_control_volume_up();
    else if (!strcmp(a, "volume_down")) playback_control_volume_down();
    else if (!strcmp(a, "mute")) playback_control_toggle_mute();
    else ok = false;
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", ok);
  if (!ok) cJSON_AddStringToObject(out, "error", "Unknown action");
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static void dsp_to_json(cJSON *j, const software_dsp_config_t *c) {
  cJSON_AddBoolToObject(j, "enabled", c->enabled);
  cJSON_AddBoolToObject(j, "limiter", c->limiter_enabled);
  cJSON_AddBoolToObject(j, "normalization", c->normalization_enabled);
  cJSON_AddNumberToObject(j, "normalization_target_dbfs", c->normalization_target_dbfs);
  cJSON_AddNumberToObject(j, "balance", c->balance);
  cJSON_AddNumberToObject(j, "channel", c->channel);
  cJSON_AddNumberToObject(j, "crossover", c->crossover);
  cJSON_AddNumberToObject(j, "crossover_hz", c->crossover_hz);
  cJSON *bands = cJSON_AddArrayToObject(j, "bands");
  for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "frequency_hz", c->bands[i].frequency_hz);
    cJSON_AddNumberToObject(b, "gain_db", c->bands[i].gain_db);
    cJSON_AddNumberToObject(b, "q", c->bands[i].q);
    cJSON_AddItemToArray(bands, b);
  }
}

static void dsp_from_json(cJSON *j, software_dsp_config_t *c) {
  if (!j || !c) return;
#define DSP_READ_BOOL(name, field) do { cJSON *item=cJSON_GetObjectItem(j,name); if(cJSON_IsBool(item)) c->field=cJSON_IsTrue(item); } while(0)
#define DSP_READ_NUM(name, field) do { cJSON *item=cJSON_GetObjectItem(j,name); if(cJSON_IsNumber(item)) c->field=(float)item->valuedouble; } while(0)
  DSP_READ_BOOL("enabled", enabled);
  DSP_READ_BOOL("limiter", limiter_enabled);
  DSP_READ_BOOL("normalization", normalization_enabled);
  DSP_READ_NUM("normalization_target_dbfs", normalization_target_dbfs);
  DSP_READ_NUM("balance", balance);
  DSP_READ_NUM("crossover_hz", crossover_hz);
  cJSON *v = cJSON_GetObjectItem(j, "channel");
  if (cJSON_IsNumber(v)) c->channel=(software_dsp_channel_t)v->valueint;
  v = cJSON_GetObjectItem(j, "crossover");
  if (cJSON_IsNumber(v)) c->crossover=(software_dsp_crossover_t)v->valueint;
  cJSON *bands = cJSON_GetObjectItem(j, "bands");
  if (cJSON_IsArray(bands)) for (int i=0; i<SOFTWARE_DSP_PEAK_BANDS; i++) {
    cJSON *b=cJSON_GetArrayItem(bands,i); if(!cJSON_IsObject(b)) continue;
    v=cJSON_GetObjectItem(b,"frequency_hz"); if(cJSON_IsNumber(v)) c->bands[i].frequency_hz=(float)v->valuedouble;
    v=cJSON_GetObjectItem(b,"gain_db"); if(cJSON_IsNumber(v)) c->bands[i].gain_db=(float)v->valuedouble;
    v=cJSON_GetObjectItem(b,"q"); if(cJSON_IsNumber(v)) c->bands[i].q=(float)v->valuedouble;
  }
#undef DSP_READ_BOOL
#undef DSP_READ_NUM
}

static esp_err_t dsp_get(httpd_req_t *req) {
  software_dsp_config_t cfg;
  software_dsp_get_config(&cfg);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "success", true);
  dsp_to_json(j, &cfg);
  esp_err_t err = send_json(req, j);
  cJSON_Delete(j);
  return err;
}

static esp_err_t dsp_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *j = read_json(req);
  if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
  software_dsp_config_t c;
  software_dsp_get_config(&c);
  dsp_from_json(j, &c);
  software_dsp_set_config(&c);
  cJSON_Delete(j);
  cJSON *out=cJSON_CreateObject(); cJSON_AddBoolToObject(out,"success",true); dsp_to_json(out,&c);
  esp_err_t err=send_json(req,out); cJSON_Delete(out); return err;
}

static esp_err_t radio_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *j=read_json(req); cJSON *u=j?cJSON_GetObjectItem(j,"url"):NULL;
  esp_err_t result=ESP_ERR_INVALID_ARG;
  if(cJSON_IsString(u)) result=dlna_renderer_play_uri(u->valuestring);
  cJSON *out=cJSON_CreateObject(); cJSON_AddBoolToObject(out,"success",result==ESP_OK);
  if(result!=ESP_OK) cJSON_AddStringToObject(out,"error",esp_err_to_name(result));
  esp_err_t err=send_json(req,out); cJSON_Delete(out); cJSON_Delete(j); return err;
}

static esp_err_t radio_presets_get(httpd_req_t *req) {
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  cJSON *items = cJSON_AddArrayToObject(out, "presets");
  for (uint8_t slot = 0; slot < SETTINGS_RADIO_PRESET_COUNT; slot++) {
    settings_radio_preset_t preset;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "slot", slot);
    if (settings_get_radio_preset(slot, &preset) == ESP_OK) {
      cJSON_AddStringToObject(item, "name", preset.name);
      cJSON_AddStringToObject(item, "url", preset.url);
    } else {
      cJSON_AddStringToObject(item, "name", "");
      cJSON_AddStringToObject(item, "url", "");
    }
    cJSON_AddItemToArray(items, item);
  }
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t radio_presets_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *slot = in ? cJSON_GetObjectItem(in, "slot") : NULL;
  cJSON *name = in ? cJSON_GetObjectItem(in, "name") : NULL;
  cJSON *url = in ? cJSON_GetObjectItem(in, "url") : NULL;
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (cJSON_IsNumber(slot) && cJSON_IsString(name) && cJSON_IsString(url) &&
      slot->valueint >= 0 && slot->valueint < SETTINGS_RADIO_PRESET_COUNT) {
    result = settings_set_radio_preset((uint8_t)slot->valueint,
                                       name->valuestring, url->valuestring);
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (result != ESP_OK) cJSON_AddStringToObject(out, "error", "Invalid preset");
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static void sleep_status_to_json(cJSON *out) {
  uint32_t deadline = s_sleep_deadline_seconds;
  uint32_t now = uptime_seconds();
  uint32_t remaining = 0;
  if (deadline != 0 && (int32_t)(deadline - now) > 0) {
    remaining = deadline - now;
  }
  cJSON_AddBoolToObject(out, "active", remaining > 0);
  cJSON_AddNumberToObject(out, "remaining_seconds", remaining);
}

static esp_err_t sleep_get(httpd_req_t *req) {
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  sleep_status_to_json(out);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t sleep_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *minutes = in ? cJSON_GetObjectItem(in, "minutes") : NULL;
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (cJSON_IsNumber(minutes) && minutes->valueint >= 0 &&
      minutes->valueint <= 720 && s_sleep_timer) {
    esp_timer_stop(s_sleep_timer);
    s_sleep_deadline_seconds = 0;
    if (minutes->valueint == 0) {
      result = ESP_OK;
    } else {
      uint32_t seconds = (uint32_t)minutes->valueint * 60U;
      result = esp_timer_start_once(s_sleep_timer,
                                    (uint64_t)seconds * 1000000ULL);
      if (result == ESP_OK) {
        s_sleep_deadline_seconds = uptime_seconds() + seconds;
      }
    }
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (result == ESP_OK) sleep_status_to_json(out);
  else cJSON_AddStringToObject(out, "error", "Minutes must be 0-720");
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static esp_err_t auth_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  cJSON_AddBoolToObject(out, "password_set", settings_web_password_is_set());
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static void maintenance_to_json(cJSON *out,
                                const settings_maintenance_t *config) {
  cJSON_AddBoolToObject(out, "speaker_protection",
                        config->speaker_protection_enabled);
  cJSON_AddNumberToObject(out, "speaker_threshold_percent",
                          config->speaker_threshold_percent);
  cJSON_AddBoolToObject(out, "scheduled_restart",
                        config->scheduled_restart_enabled);
  cJSON_AddNumberToObject(out, "restart_interval_hours",
                          config->scheduled_restart_hours);
  cJSON_AddNumberToObject(out, "restart_remaining_seconds",
                          maintenance_restart_remaining_seconds());
  cJSON_AddBoolToObject(out, "audio_idle", maintenance_audio_is_idle());
  cJSON_AddNumberToObject(out, "theme", config->theme);
  cJSON_AddBoolToObject(out, "ota_pending_verify",
                        maintenance_ota_pending_verify());
  cJSON_AddBoolToObject(out, "limiter_active",
                        software_dsp_limiter_active());
  cJSON_AddNumberToObject(out, "limiter_events",
                          software_dsp_limiter_count());
  cJSON_AddNumberToObject(out, "clipping_events",
                          software_dsp_clipping_count());
}

static esp_err_t maintenance_get(httpd_req_t *req) {
  settings_maintenance_t config;
  maintenance_get_config(&config);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  maintenance_to_json(out, &config);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t maintenance_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  if (!in) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
  settings_maintenance_t config;
  maintenance_get_config(&config);
  cJSON *v = cJSON_GetObjectItem(in, "speaker_protection");
  if (cJSON_IsBool(v)) config.speaker_protection_enabled = cJSON_IsTrue(v);
  v = cJSON_GetObjectItem(in, "speaker_threshold_percent");
  if (cJSON_IsNumber(v)) config.speaker_threshold_percent = (uint8_t)v->valueint;
  v = cJSON_GetObjectItem(in, "scheduled_restart");
  if (cJSON_IsBool(v)) config.scheduled_restart_enabled = cJSON_IsTrue(v);
  v = cJSON_GetObjectItem(in, "restart_interval_hours");
  if (cJSON_IsNumber(v)) config.scheduled_restart_hours = (uint16_t)v->valueint;
  v = cJSON_GetObjectItem(in, "theme");
  if (cJSON_IsNumber(v)) config.theme = (settings_theme_t)v->valueint;
  v = cJSON_GetObjectItem(in, "reset_counters");
  if (cJSON_IsTrue(v)) software_dsp_reset_protection_counters();

  esp_err_t result = maintenance_set_config(&config);
  if (result == ESP_OK) {
    maintenance_get_config(&config);
    software_dsp_set_speaker_protection(
        config.speaker_protection_enabled,
        config.speaker_threshold_percent);
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (result == ESP_OK) maintenance_to_json(out, &config);
  else cJSON_AddStringToObject(out, "error", esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static esp_err_t diagnostics_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  const size_t capacity = 24576;
  char *buffer = heap_caps_calloc(1, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) buffer = calloc(1, capacity);
  if (!buffer) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                           "Not enough memory");
  size_t length = log_stream_read_persistent(buffer, capacity);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_send(req, buffer, (ssize_t)length);
  free(buffer);
  return err;
}

static esp_err_t diagnostics_clear_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  log_stream_clear_persistent();
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static void bytes_to_hex(const uint8_t *bytes, size_t count, char *out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < count; i++) {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 15];
  }
  out[count * 2] = '\0';
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t count) {
  if (!hex || strlen(hex) != count * 2) return false;
  for (size_t i = 0; i < count; i++) {
    unsigned value;
    if (sscanf(hex + i * 2, "%2x", &value) != 1) return false;
    out[i] = (uint8_t)value;
  }
  return true;
}

static esp_err_t backup_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "backup_schema", 1);
  cJSON_AddStringToObject(root, "product", "AirPlay and DLNA Receiver");
  cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);

  char text[257] = {0};
  settings_get_device_name(text, sizeof(text));
  cJSON_AddStringToObject(root, "device_name", text);
  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  text[0] = '\0'; settings_get_wifi_ssid(text, sizeof(text));
  cJSON_AddStringToObject(wifi, "ssid", text);
  text[0] = '\0'; settings_get_wifi_password(text, sizeof(text));
  cJSON_AddStringToObject(wifi, "password", text);
  float volume = -15.0f;
  settings_get_volume(&volume);
  cJSON_AddNumberToObject(root, "volume_db", volume);
#ifdef CONFIG_BT_A2DP_ENABLE
  uint8_t bt_volume = 64;
  if (settings_get_bt_volume(&bt_volume) == ESP_OK)
    cJSON_AddNumberToObject(root, "bluetooth_volume", bt_volume);
#endif
  uint8_t brightness = 128;
  settings_get_led_brightness(&brightness);
  cJSON_AddNumberToObject(root, "led_brightness", brightness);

  software_dsp_config_t dsp;
  software_dsp_get_config(&dsp);
  cJSON *dsp_json = cJSON_AddObjectToObject(root, "dsp");
  dsp_to_json(dsp_json, &dsp);
  float hardware_eq[SETTINGS_EQ_BANDS];
  if (settings_get_eq_gains(hardware_eq) == ESP_OK) {
    cJSON *hardware_eq_json = cJSON_AddArrayToObject(root, "hardware_eq_gains");
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++)
      cJSON_AddItemToArray(hardware_eq_json,
                           cJSON_CreateNumber(hardware_eq[i]));
  }

  settings_maintenance_t maintenance;
  maintenance_get_config(&maintenance);
  cJSON *maintenance_json = cJSON_AddObjectToObject(root, "maintenance");
  maintenance_to_json(maintenance_json, &maintenance);

  settings_mqtt_t mqtt;
  settings_get_mqtt(&mqtt);
  cJSON *mqtt_json = cJSON_AddObjectToObject(root, "mqtt");
  cJSON_AddBoolToObject(mqtt_json, "enabled", mqtt.enabled);
  cJSON_AddBoolToObject(mqtt_json, "home_assistant_discovery",
                        mqtt.home_assistant_discovery);
  cJSON_AddStringToObject(mqtt_json, "broker_uri", mqtt.broker_uri);
  cJSON_AddStringToObject(mqtt_json, "username", mqtt.username);
  cJSON_AddStringToObject(mqtt_json, "password", mqtt.password);
  cJSON_AddStringToObject(mqtt_json, "topic_prefix", mqtt.topic_prefix);

  cJSON *presets = cJSON_AddArrayToObject(root, "radio_presets");
  for (uint8_t slot = 0; slot < SETTINGS_RADIO_PRESET_COUNT; slot++) {
    settings_radio_preset_t preset;
    if (settings_get_radio_preset(slot, &preset) != ESP_OK) continue;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "slot", slot);
    cJSON_AddStringToObject(item, "name", preset.name);
    cJSON_AddStringToObject(item, "url", preset.url);
    cJSON_AddItemToArray(presets, item);
  }

  uint8_t digest[32];
  cJSON_AddBoolToObject(root, "web_password_set",
                        settings_web_password_is_set());
  if (settings_get_web_password_digest(digest) == ESP_OK) {
    char digest_hex[65];
    bytes_to_hex(digest, sizeof(digest), digest_hex);
    cJSON_AddStringToObject(root, "web_password_sha256", digest_hex);
  }
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=airplay-dlna-settings.json");
  esp_err_t err = send_json(req, root);
  cJSON_Delete(root);
  return err;
}

static esp_err_t backup_restore_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *root = read_json(req);
  cJSON *schema = root ? cJSON_GetObjectItem(root, "backup_schema") : NULL;
  if (!root || !cJSON_IsNumber(schema) || schema->valueint != 1) {
    cJSON_Delete(root);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Unsupported backup file");
  }
  esp_err_t result = ESP_OK;
  cJSON *v = cJSON_GetObjectItem(root, "device_name");
  if (cJSON_IsString(v)) result = settings_set_device_name(v->valuestring);
  cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
  cJSON *ssid = cJSON_IsObject(wifi) ? cJSON_GetObjectItem(wifi, "ssid") : NULL;
  cJSON *password = cJSON_IsObject(wifi) ? cJSON_GetObjectItem(wifi, "password") : NULL;
  if (result == ESP_OK && cJSON_IsString(ssid) && ssid->valuestring[0] &&
      cJSON_IsString(password)) {
    result = settings_set_wifi_credentials(ssid->valuestring, password->valuestring);
  }
  v = cJSON_GetObjectItem(root, "volume_db");
  if (result == ESP_OK && cJSON_IsNumber(v)) {
    result = settings_set_volume((float)v->valuedouble);
    if (result == ESP_OK) result = settings_persist_volume();
  }
#ifdef CONFIG_BT_A2DP_ENABLE
  v = cJSON_GetObjectItem(root, "bluetooth_volume");
  if (result == ESP_OK && cJSON_IsNumber(v)) {
    result = settings_set_bt_volume((uint8_t)v->valueint);
    if (result == ESP_OK) result = settings_persist_bt_volume();
  }
#endif
  v = cJSON_GetObjectItem(root, "led_brightness");
  if (result == ESP_OK && cJSON_IsNumber(v)) result = led_set_brightness((uint8_t)v->valueint);
  cJSON *dsp_json = cJSON_GetObjectItem(root, "dsp");
  if (result == ESP_OK && cJSON_IsObject(dsp_json)) {
    software_dsp_config_t dsp;
    software_dsp_get_config(&dsp);
    dsp_from_json(dsp_json, &dsp);
    result = software_dsp_set_config(&dsp);
  }
  cJSON *hardware_eq_json = cJSON_GetObjectItem(root, "hardware_eq_gains");
  if (result == ESP_OK && cJSON_IsArray(hardware_eq_json) &&
      cJSON_GetArraySize(hardware_eq_json) == SETTINGS_EQ_BANDS) {
    float hardware_eq[SETTINGS_EQ_BANDS];
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON *gain = cJSON_GetArrayItem(hardware_eq_json, i);
      hardware_eq[i] = cJSON_IsNumber(gain) ? (float)gain->valuedouble : 0.0f;
    }
    result = settings_set_eq_gains(hardware_eq);
  }
  cJSON *m = cJSON_GetObjectItem(root, "maintenance");
  if (result == ESP_OK && cJSON_IsObject(m)) {
    settings_maintenance_t config;
    maintenance_get_config(&config);
    v=cJSON_GetObjectItem(m,"speaker_protection"); if(cJSON_IsBool(v)) config.speaker_protection_enabled=cJSON_IsTrue(v);
    v=cJSON_GetObjectItem(m,"speaker_threshold_percent"); if(cJSON_IsNumber(v)) config.speaker_threshold_percent=(uint8_t)v->valueint;
    v=cJSON_GetObjectItem(m,"scheduled_restart"); if(cJSON_IsBool(v)) config.scheduled_restart_enabled=cJSON_IsTrue(v);
    v=cJSON_GetObjectItem(m,"restart_interval_hours"); if(cJSON_IsNumber(v)) config.scheduled_restart_hours=(uint16_t)v->valueint;
    v=cJSON_GetObjectItem(m,"theme"); if(cJSON_IsNumber(v)) config.theme=(settings_theme_t)v->valueint;
    result = maintenance_set_config(&config);
    software_dsp_set_speaker_protection(config.speaker_protection_enabled,
                                         config.speaker_threshold_percent);
  }
  cJSON *presets = cJSON_GetObjectItem(root, "radio_presets");
  if (result == ESP_OK && cJSON_IsArray(presets)) {
    for (uint8_t slot=0; slot<SETTINGS_RADIO_PRESET_COUNT; slot++)
      settings_set_radio_preset(slot, "", "");
    cJSON *item;
    cJSON_ArrayForEach(item, presets) {
      cJSON *slot=cJSON_GetObjectItem(item,"slot");
      cJSON *name=cJSON_GetObjectItem(item,"name");
      cJSON *url=cJSON_GetObjectItem(item,"url");
      if(cJSON_IsNumber(slot)&&cJSON_IsString(name)&&cJSON_IsString(url))
        settings_set_radio_preset((uint8_t)slot->valueint,name->valuestring,url->valuestring);
    }
  }
  cJSON *mqtt_json = cJSON_GetObjectItem(root, "mqtt");
  if (result == ESP_OK && cJSON_IsObject(mqtt_json)) {
    settings_mqtt_t mqtt;
    settings_get_mqtt(&mqtt);
    v=cJSON_GetObjectItem(mqtt_json,"enabled"); if(cJSON_IsBool(v)) mqtt.enabled=cJSON_IsTrue(v);
    v=cJSON_GetObjectItem(mqtt_json,"home_assistant_discovery"); if(cJSON_IsBool(v)) mqtt.home_assistant_discovery=cJSON_IsTrue(v);
#define RESTORE_MQTT_STRING(name, field) do { v=cJSON_GetObjectItem(mqtt_json,name); if(cJSON_IsString(v)) strlcpy(mqtt.field,v->valuestring,sizeof(mqtt.field)); } while(0)
    RESTORE_MQTT_STRING("broker_uri", broker_uri);
    RESTORE_MQTT_STRING("username", username);
    RESTORE_MQTT_STRING("password", password);
    RESTORE_MQTT_STRING("topic_prefix", topic_prefix);
#undef RESTORE_MQTT_STRING
    result = settings_set_mqtt(&mqtt);
  }
  cJSON *password_set = cJSON_GetObjectItem(root, "web_password_set");
  v = cJSON_GetObjectItem(root, "web_password_sha256");
  if (result == ESP_OK && cJSON_IsFalse(password_set)) {
    result = settings_set_web_password("");
  } else if (result == ESP_OK && cJSON_IsString(v)) {
    uint8_t digest[32];
    if (hex_to_bytes(v->valuestring, digest, sizeof(digest)))
      result = settings_set_web_password_digest(digest);
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  cJSON_AddBoolToObject(out, "restart_required", result == ESP_OK);
  if (result != ESP_OK) cJSON_AddStringToObject(out, "error", esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(root);
  return err;
}

static esp_err_t factory_reset_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *confirmation = in ? cJSON_GetObjectItem(in, "confirmation") : NULL;
  bool confirmed = cJSON_IsString(confirmation) &&
                   strcmp(confirmation->valuestring, "ERASE ALL SETTINGS") == 0;
  esp_err_t result = confirmed ? settings_factory_reset() : ESP_ERR_INVALID_ARG;
  if (result == ESP_OK) {
    log_stream_clear_persistent();
    recovery_clear();
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (!confirmed) cJSON_AddStringToObject(out, "error",
      "Type ERASE ALL SETTINGS exactly");
  else if (result != ESP_OK) cJSON_AddStringToObject(out, "error", esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  if (result == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }
  return err;
}

static esp_err_t password_post(httpd_req_t *req) {
  if (settings_web_password_is_set() && require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *j=read_json(req); cJSON *p=j?cJSON_GetObjectItem(j,"password"):NULL;
  esp_err_t result=cJSON_IsString(p)?settings_set_web_password(p->valuestring):ESP_ERR_INVALID_ARG;
  cJSON *out=cJSON_CreateObject(); cJSON_AddBoolToObject(out,"success",result==ESP_OK);
  if(result!=ESP_OK) cJSON_AddStringToObject(out,"error","Password must be empty or 8-64 characters");
  esp_err_t err=send_json(req,out); cJSON_Delete(out); cJSON_Delete(j); return err;
}

static esp_err_t recovery_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  recovery_status_t status;
  recovery_get_status(&status);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  cJSON_AddBoolToObject(out, "safe_mode", status.safe_mode);
  cJSON_AddBoolToObject(out, "forced", status.forced);
  cJSON_AddNumberToObject(out, "consecutive_crashes", status.consecutive_crashes);
  cJSON_AddNumberToObject(out, "reset_reason", status.reset_reason);
  cJSON_AddNumberToObject(out, "settings_schema", settings_schema_version());
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t recovery_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *action = in ? cJSON_GetObjectItem(in, "action") : NULL;
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (cJSON_IsString(action)) {
    if (!strcmp(action->valuestring, "clear")) result = recovery_clear();
    else if (!strcmp(action->valuestring, "force"))
      result = recovery_force_safe_mode(true);
    else if (!strcmp(action->valuestring, "normal"))
      result = recovery_force_safe_mode(false);
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  cJSON_AddBoolToObject(out, "restart_required", result == ESP_OK);
  if (result != ESP_OK) cJSON_AddStringToObject(out, "error", "Invalid recovery action");
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static esp_err_t wifi_diagnostics_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  wifi_diagnostics_t d;
  wifi_get_diagnostics(&d);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  cJSON_AddBoolToObject(out, "initialized", d.initialized);
  cJSON_AddBoolToObject(out, "connected", d.connected);
  cJSON_AddBoolToObject(out, "setup_ap_enabled", d.setup_ap_enabled);
  cJSON_AddBoolToObject(out, "pending_credential_test", d.pending_credential_test);
  cJSON_AddNumberToObject(out, "retry_count", d.retry_count);
  cJSON_AddNumberToObject(out, "last_disconnect_reason", d.last_disconnect_reason);
  cJSON_AddNumberToObject(out, "rssi", d.rssi);
  cJSON_AddNumberToObject(out, "channel", d.channel);
  cJSON_AddStringToObject(out, "ssid", d.ssid);
  cJSON_AddStringToObject(out, "bssid", d.bssid);
  cJSON_AddStringToObject(out, "ip", d.ip);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t wifi_test_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *ssid = in ? cJSON_GetObjectItem(in, "ssid") : NULL;
  cJSON *password = in ? cJSON_GetObjectItem(in, "password") : NULL;
  wifi_credential_check_t check;
  memset(&check, 0, sizeof(check));
  esp_err_t result = cJSON_IsString(ssid) && cJSON_IsString(password)
                         ? wifi_check_credentials(ssid->valuestring,
                                                  password->valuestring, &check)
                         : ESP_ERR_INVALID_ARG;
  bool staged = false;
  if (result == ESP_OK && check.network_visible &&
      check.password_format_valid && !check.already_connected) {
    result = settings_set_pending_wifi_credentials(ssid->valuestring,
                                                   password->valuestring);
    staged = result == ESP_OK;
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  cJSON_AddBoolToObject(out, "network_visible", check.network_visible);
  cJSON_AddBoolToObject(out, "password_format_valid", check.password_format_valid);
  cJSON_AddBoolToObject(out, "already_connected", check.already_connected);
  cJSON_AddBoolToObject(out, "staged", staged);
  cJSON_AddBoolToObject(out, "restart_required", staged);
  cJSON_AddNumberToObject(out, "rssi", check.rssi);
  cJSON_AddNumberToObject(out, "channel", check.channel);
  cJSON_AddNumberToObject(out, "authmode", check.authmode);
  cJSON_AddStringToObject(out, "message", check.message);
  if (result != ESP_OK) cJSON_AddStringToObject(out, "error", esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static void mqtt_to_json(cJSON *out, const settings_mqtt_t *config) {
  mqtt_control_status_t status;
  mqtt_control_get_status(&status);
  cJSON_AddBoolToObject(out, "enabled", config->enabled);
  cJSON_AddBoolToObject(out, "home_assistant_discovery",
                        config->home_assistant_discovery);
  cJSON_AddStringToObject(out, "broker_uri", config->broker_uri);
  cJSON_AddStringToObject(out, "username", config->username);
  cJSON_AddStringToObject(out, "topic_prefix", config->topic_prefix);
  cJSON_AddBoolToObject(out, "password_set", config->password[0] != '\0');
  cJSON_AddBoolToObject(out, "connected", status.connected);
  cJSON_AddNumberToObject(out, "last_error", status.last_error);
}

static esp_err_t mqtt_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  settings_mqtt_t config;
  settings_get_mqtt(&config);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  mqtt_to_json(out, &config);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t mqtt_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  settings_mqtt_t config;
  settings_get_mqtt(&config);
  cJSON *v = in ? cJSON_GetObjectItem(in, "enabled") : NULL;
  if (cJSON_IsBool(v)) config.enabled = cJSON_IsTrue(v);
  v = in ? cJSON_GetObjectItem(in, "home_assistant_discovery") : NULL;
  if (cJSON_IsBool(v)) config.home_assistant_discovery = cJSON_IsTrue(v);
#define MQTT_STRING(name, field) do { v=in?cJSON_GetObjectItem(in,name):NULL; if(cJSON_IsString(v)) strlcpy(config.field,v->valuestring,sizeof(config.field)); } while(0)
  MQTT_STRING("broker_uri", broker_uri);
  MQTT_STRING("username", username);
  MQTT_STRING("topic_prefix", topic_prefix);
  MQTT_STRING("password", password);
#undef MQTT_STRING
  esp_err_t result = settings_set_mqtt(&config);
  if (result == ESP_OK && !recovery_is_safe_mode()) result = mqtt_control_reload();
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (result == ESP_OK) mqtt_to_json(out, &config);
  else cJSON_AddStringToObject(out, "error", esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static esp_err_t audio_test_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *channel = in ? cJSON_GetObjectItem(in, "channel") : NULL;
  cJSON *frequency = in ? cJSON_GetObjectItem(in, "frequency_hz") : NULL;
  cJSON *duration = in ? cJSON_GetObjectItem(in, "duration_ms") : NULL;
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (cJSON_IsNumber(channel) && cJSON_IsNumber(frequency) &&
      cJSON_IsNumber(duration)) {
    result = audio_test_start((audio_test_channel_t)channel->valueint,
                              (uint16_t)frequency->valueint,
                              (uint16_t)duration->valueint);
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  cJSON_AddBoolToObject(out, "running", audio_test_is_running());
  if (result != ESP_OK)
    cJSON_AddStringToObject(out, "error",
        result == ESP_ERR_INVALID_STATE ? "Audio must be idle and initialized" : esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static void github_status_json(cJSON *out) {
  github_ota_status_t status;
  github_ota_get_status(&status);
  cJSON_AddNumberToObject(out, "state", status.state);
  cJSON_AddBoolToObject(out, "update_available", status.update_available);
  cJSON_AddNumberToObject(out, "progress_percent", status.progress_percent);
  cJSON_AddStringToObject(out, "current_version", status.current_version);
  cJSON_AddStringToObject(out, "latest_version", status.latest_version);
  cJSON_AddStringToObject(out, "error", status.error);
}

static esp_err_t github_ota_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  github_status_json(out);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t github_ota_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK) return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *action = in ? cJSON_GetObjectItem(in, "action") : NULL;
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (cJSON_IsString(action)) {
    if (!maintenance_audio_is_idle()) result = ESP_ERR_INVALID_STATE;
    else if (!strcmp(action->valuestring, "check")) result = github_ota_check();
    else if (!strcmp(action->valuestring, "install")) result = github_ota_install();
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  github_status_json(out);
  if (result != ESP_OK) cJSON_AddStringToObject(
      out, "request_error",
      result == ESP_ERR_INVALID_STATE ? "Stop playback before checking or installing an update"
                                      : esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

esp_err_t web_api_v3_register(httpd_handle_t s) {
  if (!s_sleep_timer) {
    const esp_timer_create_args_t timer_args = {
        .callback = sleep_timer_callback,
        .name = "sleep_timer",
    };
    esp_err_t timer_err = esp_timer_create(&timer_args, &s_sleep_timer);
    if (timer_err != ESP_OK) return timer_err;
  }
  const httpd_uri_t routes[] = {
    {.uri="/api/v1/status", .method=HTTP_GET, .handler=status_get},
    {.uri="/api/v1/health", .method=HTTP_GET, .handler=health_get},
    {.uri="/api/v1/control", .method=HTTP_POST, .handler=control_post},
    {.uri="/api/v1/dsp", .method=HTTP_GET, .handler=dsp_get},
    {.uri="/api/v1/dsp", .method=HTTP_POST, .handler=dsp_post},
    {.uri="/api/v1/radio", .method=HTTP_POST, .handler=radio_post},
    {.uri="/api/v1/radio/presets", .method=HTTP_GET, .handler=radio_presets_get},
    {.uri="/api/v1/radio/presets", .method=HTTP_POST, .handler=radio_presets_post},
    {.uri="/api/v1/sleep", .method=HTTP_GET, .handler=sleep_get},
    {.uri="/api/v1/sleep", .method=HTTP_POST, .handler=sleep_post},
    {.uri="/api/v1/auth", .method=HTTP_GET, .handler=auth_get},
    {.uri="/api/v1/maintenance", .method=HTTP_GET, .handler=maintenance_get},
    {.uri="/api/v1/maintenance", .method=HTTP_POST, .handler=maintenance_post},
    {.uri="/api/v1/diagnostics/log", .method=HTTP_GET, .handler=diagnostics_get},
    {.uri="/api/v1/diagnostics/clear", .method=HTTP_POST, .handler=diagnostics_clear_post},
    {.uri="/api/v1/backup", .method=HTTP_GET, .handler=backup_get},
    {.uri="/api/v1/backup/restore", .method=HTTP_POST, .handler=backup_restore_post},
    {.uri="/api/v1/factory-reset", .method=HTTP_POST, .handler=factory_reset_post},
    {.uri="/api/v1/security/password", .method=HTTP_POST, .handler=password_post},
    {.uri="/api/v1/recovery", .method=HTTP_GET, .handler=recovery_get},
    {.uri="/api/v1/recovery", .method=HTTP_POST, .handler=recovery_post},
    {.uri="/api/v1/wifi/diagnostics", .method=HTTP_GET, .handler=wifi_diagnostics_get},
    {.uri="/api/v1/wifi/test", .method=HTTP_POST, .handler=wifi_test_post},
    {.uri="/api/v1/mqtt", .method=HTTP_GET, .handler=mqtt_get},
    {.uri="/api/v1/mqtt", .method=HTTP_POST, .handler=mqtt_post},
    {.uri="/api/v1/audio/test", .method=HTTP_POST, .handler=audio_test_post},
    {.uri="/api/v1/github-ota", .method=HTTP_GET, .handler=github_ota_get},
    {.uri="/api/v1/github-ota", .method=HTTP_POST, .handler=github_ota_post},
  };
  for (size_t i=0; i<sizeof(routes)/sizeof(routes[0]); i++) {
    esp_err_t err=httpd_register_uri_handler(s,&routes[i]); if(err!=ESP_OK) return err;
  }
  return ESP_OK;
}
