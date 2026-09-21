#include "github_ota.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

#define RELEASE_API_URL \
  "https://api.github.com/repos/dora310/airplay-DLNA-esp32/releases/latest"
#define RELEASE_BUFFER_SIZE (32 * 1024)

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} response_buffer_t;

static const char *TAG = "github_ota";
static github_ota_status_t s_status;
static SemaphoreHandle_t s_lock;

static void ensure_initialized(void) {
  if (!s_lock)
    s_lock = xSemaphoreCreateMutex();
  if (!s_status.current_version[0]) {
    strlcpy(s_status.current_version, esp_app_get_description()->version,
            sizeof(s_status.current_version));
  }
}

static void set_error(const char *error) {
  ensure_initialized();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.state = GITHUB_OTA_ERROR;
  strlcpy(s_status.error, error ? error : "Unknown update error",
          sizeof(s_status.error));
  xSemaphoreGive(s_lock);
  ESP_LOGE(TAG, "%s", s_status.error);
}

static esp_err_t response_event(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data ||
      event->data_len <= 0)
    return ESP_OK;
  response_buffer_t *buffer = (response_buffer_t *)event->user_data;
  if (buffer->length + event->data_len + 1 > buffer->capacity)
    return ESP_ERR_NO_MEM;
  memcpy(buffer->data + buffer->length, event->data, event->data_len);
  buffer->length += event->data_len;
  buffer->data[buffer->length] = '\0';
  return ESP_OK;
}

static bool ota_asset_name(const char *name) {
#if CONFIG_IDF_TARGET_ESP32S3
  const char *target = "esp32s3";
#elif CONFIG_IDF_TARGET_ESP32S2
  const char *target = "esp32s2";
#else
  const char *target = "esp32";
#endif
  return name && strstr(name, target) && strstr(name, "ota") &&
         strstr(name, ".bin");
}

esp_err_t github_ota_check(void) {
  ensure_initialized();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_status.state == GITHUB_OTA_DOWNLOADING) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_status.state = GITHUB_OTA_CHECKING;
  s_status.error[0] = '\0';
  s_status.latest_version[0] = '\0';
  s_status.asset_url[0] = '\0';
  s_status.progress_percent = 0;
  xSemaphoreGive(s_lock);

  response_buffer_t response = {
      .data = heap_caps_calloc(1, RELEASE_BUFFER_SIZE,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
      .capacity = RELEASE_BUFFER_SIZE,
  };
  if (!response.data)
    response.data = calloc(1, RELEASE_BUFFER_SIZE);
  if (!response.data) {
    set_error("Not enough memory to check GitHub");
    return ESP_ERR_NO_MEM;
  }
  esp_http_client_config_t config = {
      .url = RELEASE_API_URL,
      .event_handler = response_event,
      .user_data = &response,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .user_agent = "AirPlay-DLNA-ESP32/3.4.0",
      .timeout_ms = 15000,
      .buffer_size = 2048,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
  int status = client ? esp_http_client_get_status_code(client) : 0;
  if (client)
    esp_http_client_cleanup(client);
  if (err != ESP_OK || status != 200) {
    char message[96];
    snprintf(message, sizeof(message), "GitHub check failed (%s, HTTP %d)",
             esp_err_to_name(err), status);
    free(response.data);
    set_error(message);
    return err == ESP_OK ? ESP_FAIL : err;
  }

  cJSON *root = cJSON_Parse(response.data);
  free(response.data);
  cJSON *tag = root ? cJSON_GetObjectItem(root, "tag_name") : NULL;
  cJSON *assets = root ? cJSON_GetObjectItem(root, "assets") : NULL;
  const char *url = NULL;
  if (cJSON_IsArray(assets)) {
    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
      cJSON *name = cJSON_GetObjectItem(asset, "name");
      cJSON *download = cJSON_GetObjectItem(asset, "browser_download_url");
      if (cJSON_IsString(name) && cJSON_IsString(download) &&
          ota_asset_name(name->valuestring)) {
        url = download->valuestring;
        break;
      }
    }
  }
  if (!cJSON_IsString(tag) || !url || strncmp(url, "https://github.com/", 19)) {
    cJSON_Delete(root);
    set_error("Latest release has no matching OTA image");
    return ESP_ERR_NOT_FOUND;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  strlcpy(s_status.latest_version, tag->valuestring,
          sizeof(s_status.latest_version));
  strlcpy(s_status.asset_url, url, sizeof(s_status.asset_url));
  const char *current = s_status.current_version;
  const char *latest = s_status.latest_version[0] == 'v'
                           ? s_status.latest_version + 1
                           : s_status.latest_version;
  s_status.update_available = strcmp(current, latest) != 0;
  s_status.state =
      s_status.update_available ? GITHUB_OTA_AVAILABLE : GITHUB_OTA_CURRENT;
  xSemaphoreGive(s_lock);
  cJSON_Delete(root);
  return ESP_OK;
}

static void install_task(void *arg) {
  (void)arg;
  char url[sizeof(s_status.asset_url)];
  xSemaphoreTake(s_lock, portMAX_DELAY);
  strlcpy(url, s_status.asset_url, sizeof(url));
  s_status.state = GITHUB_OTA_DOWNLOADING;
  s_status.progress_percent = 0;
  xSemaphoreGive(s_lock);

  esp_http_client_config_t http = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 20000,
      .keep_alive_enable = true,
  };
  esp_https_ota_config_t config = {.http_config = &http};
  esp_https_ota_handle_t handle = NULL;
  esp_err_t err = esp_https_ota_begin(&config, &handle);
  if (err != ESP_OK) {
    set_error(esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }
  esp_app_desc_t incoming;
  err = esp_https_ota_get_img_desc(handle, &incoming);
  const esp_app_desc_t *running = esp_app_get_description();
  if (err != ESP_OK || strncmp(incoming.project_name, running->project_name,
                               sizeof(incoming.project_name)) != 0) {
    esp_https_ota_abort(handle);
    set_error("Downloaded image is for a different product");
    vTaskDelete(NULL);
    return;
  }
  while ((err = esp_https_ota_perform(handle)) ==
         ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    int read = esp_https_ota_get_image_len_read(handle);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    int total = target ? (int)target->size : 0;
    uint8_t progress = total > 0 ? (uint8_t)((read * 95LL) / total) : 0;
    if (progress > 95)
      progress = 95;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.progress_percent = progress;
    xSemaphoreGive(s_lock);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (err == ESP_OK)
    err = esp_https_ota_finish(handle);
  else
    esp_https_ota_abort(handle);
  if (err != ESP_OK) {
    set_error(esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.progress_percent = 100;
  s_status.state = GITHUB_OTA_READY_TO_REBOOT;
  xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "GitHub OTA complete; rebooting into rollback-protected image");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

esp_err_t github_ota_install(void) {
  ensure_initialized();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool ready =
      s_status.state == GITHUB_OTA_AVAILABLE && s_status.asset_url[0] != '\0';
  xSemaphoreGive(s_lock);
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  return xTaskCreate(install_task, "github_ota", 8192, NULL, 4, NULL) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

void github_ota_get_status(github_ota_status_t *status) {
  if (!status)
    return;
  ensure_initialized();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *status = s_status;
  xSemaphoreGive(s_lock);
}
