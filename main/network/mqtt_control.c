#include "mqtt_control.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "playback_control.h"
#include "settings.h"
#include "source_manager.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static settings_mqtt_t s_config;
static mqtt_control_status_t s_status;
static char s_unique_id[40];
static TaskHandle_t s_publish_task;

static void make_topic(char *out, size_t length, const char *suffix) {
  snprintf(out, length, "%s/%s", s_config.topic_prefix, suffix);
}

static void publish_state(void) {
  if (!s_client || !s_status.connected) return;
  wifi_diagnostics_t wifi;
  wifi_get_diagnostics(&wifi);
  char topic[160], attributes_topic[160], payload[384];
  make_topic(topic, sizeof(topic), "state");
  esp_mqtt_client_publish(
      s_client, topic,
      playback_control_get_source() == PLAYBACK_SOURCE_NONE ? "idle" : "playing",
      0, 1, 1);
  make_topic(attributes_topic, sizeof(attributes_topic), "attributes");
  snprintf(payload, sizeof(payload),
           "{\"source\":\"%s\",\"volume\":%d,"
           "\"muted\":%s,\"wifi_rssi\":%d}",
           source_manager_name(source_manager_current()),
           playback_control_get_volume_percent(),
           playback_control_is_muted() ? "true" : "false", wifi.rssi);
  esp_mqtt_client_publish(s_client, attributes_topic, payload, 0, 1, 1);
}

static void publish_discovery(void) {
  if (!s_config.home_assistant_discovery) return;
  char device_name[65], topic[192], command_topic[160], state_topic[160];
  // The maximum configured device name and topic prefix can produce a Home
  // Assistant discovery document slightly larger than 1 KiB.  Keep this
  // fixed-size buffer comfortably above the compiler-proven 1133-byte bound;
  // builds use -Werror=format-truncation so undersizing this is a hard error.
  char attributes_topic[160], availability_topic[160], payload[1536];
  settings_get_device_name(device_name, sizeof(device_name));
  make_topic(command_topic, sizeof(command_topic), "command");
  make_topic(state_topic, sizeof(state_topic), "state");
  make_topic(attributes_topic, sizeof(attributes_topic), "attributes");
  make_topic(availability_topic, sizeof(availability_topic), "availability");
  snprintf(topic, sizeof(topic), "homeassistant/media_player/%s/config",
           s_unique_id);
  snprintf(payload, sizeof(payload),
           "{\"name\":\"%s\",\"unique_id\":\"%s\","
           "\"command_topic\":\"%s\",\"state_topic\":\"%s\","
           "\"json_attributes_topic\":\"%s\","
           "\"availability_topic\":\"%s\",\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\","
           "\"device\":{\"identifiers\":[\"%s\"],"
           "\"manufacturer\":\"Open source ESP32\","
           "\"model\":\"AirPlay 2 and DLNA+ Receiver\",\"name\":\"%s\"}}",
           device_name, s_unique_id, command_topic, state_topic,
           attributes_topic, availability_topic, s_unique_id, device_name);
  esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
  esp_mqtt_client_publish(s_client, availability_topic, "online", 0, 1, 1);
  publish_state();
}

static void state_publish_task(void *arg) {
  (void)arg;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    publish_state();
  }
}

static bool command_equals(const esp_mqtt_event_handle_t event,
                           const char *command) {
  size_t length = strlen(command);
  return event->data_len == (int)length &&
         strncasecmp(event->data, command, length) == 0;
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)arg;
  (void)base;
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
  if (id == MQTT_EVENT_CONNECTED) {
    s_status.connected = true;
    s_status.last_error = 0;
    char topic[160];
    make_topic(topic, sizeof(topic), "command");
    esp_mqtt_client_subscribe(s_client, topic, 1);
    publish_discovery();
    ESP_LOGI(TAG, "Connected to MQTT broker");
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    s_status.connected = false;
  } else if (id == MQTT_EVENT_ERROR) {
    s_status.connected = false;
    s_status.last_error = event->error_handle
                              ? event->error_handle->error_type
                              : -1;
  } else if (id == MQTT_EVENT_DATA) {
    if (command_equals(event, "play_pause") || command_equals(event, "PLAY") ||
        command_equals(event, "PAUSE"))
      playback_control_play_pause();
    else if (command_equals(event, "next") || command_equals(event, "NEXT"))
      playback_control_next();
    else if (command_equals(event, "previous") ||
             command_equals(event, "PREVIOUS"))
      playback_control_prev();
    else if (command_equals(event, "vol_up") ||
             command_equals(event, "VOLUME_UP"))
      playback_control_volume_up();
    else if (command_equals(event, "vol_down") ||
             command_equals(event, "VOLUME_DOWN"))
      playback_control_volume_down();
    else if (command_equals(event, "mute") || command_equals(event, "MUTE"))
      playback_control_toggle_mute();
    publish_state();
  }
}

esp_err_t mqtt_control_start(void) {
  if (s_client) return ESP_OK;
  settings_get_mqtt(&s_config);
  memset(&s_status, 0, sizeof(s_status));
  s_status.enabled = s_config.enabled;
  strlcpy(s_status.broker_uri, s_config.broker_uri,
          sizeof(s_status.broker_uri));
  strlcpy(s_status.topic_prefix, s_config.topic_prefix,
          sizeof(s_status.topic_prefix));
  if (!s_config.enabled) {
    ESP_LOGI(TAG, "MQTT integration disabled");
    return ESP_OK;
  }
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_unique_id, sizeof(s_unique_id),
           "airplay_dlna_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  esp_mqtt_client_config_t config = {
      .broker.address.uri = s_config.broker_uri,
      .credentials.username = s_config.username[0] ? s_config.username : NULL,
      .credentials.authentication.password =
          s_config.password[0] ? s_config.password : NULL,
  };
  s_client = esp_mqtt_client_init(&config);
  if (!s_client) return ESP_ERR_NO_MEM;
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
  ESP_LOGI(TAG, "Starting MQTT/Home Assistant integration at %s",
           s_config.broker_uri);
  esp_err_t err = esp_mqtt_client_start(s_client);
  if (err == ESP_OK && !s_publish_task) {
    xTaskCreate(state_publish_task, "mqtt_state", 2560, NULL, 2,
                &s_publish_task);
  }
  return err;
}

void mqtt_control_stop(void) {
  if (!s_client) return;
  esp_mqtt_client_handle_t client = s_client;
  if (s_status.connected) {
    char topic[160];
    make_topic(topic, sizeof(topic), "availability");
    esp_mqtt_client_publish(client, topic, "offline", 0, 1, 1);
  }
  s_client = NULL;
  esp_mqtt_client_stop(client);
  esp_mqtt_client_destroy(client);
  s_status.connected = false;
}

esp_err_t mqtt_control_reload(void) {
  mqtt_control_stop();
  return mqtt_control_start();
}

void mqtt_control_get_status(mqtt_control_status_t *status) {
  if (status) *status = s_status;
}
