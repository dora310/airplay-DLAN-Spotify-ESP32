#include "recovery.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

#define RECOVERY_NAMESPACE "recovery"
#define RECOVERY_KEY_CRASHES "crashes"
#define RECOVERY_KEY_FORCED "forced"
#define RECOVERY_CRASH_LIMIT 3

static const char *TAG = "recovery";
static recovery_status_t s_status;
static bool s_initialized;
static bool s_health_task_started;

static bool is_crash_reset(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

static esp_err_t store_u8(const char *key, uint8_t value) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(RECOVERY_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_u8(nvs, key, value);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

esp_err_t recovery_init(void) {
  if (s_initialized) return ESP_OK;
  memset(&s_status, 0, sizeof(s_status));
  s_status.reset_reason = esp_reset_reason();
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(RECOVERY_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  uint8_t crashes = 0, forced = 0;
  nvs_get_u8(nvs, RECOVERY_KEY_CRASHES, &crashes);
  nvs_get_u8(nvs, RECOVERY_KEY_FORCED, &forced);
  if (is_crash_reset(s_status.reset_reason) && crashes < UINT8_MAX) crashes++;
  else if (!is_crash_reset(s_status.reset_reason) && !forced) crashes = 0;
  err = nvs_set_u8(nvs, RECOVERY_KEY_CRASHES, crashes);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) return err;

  s_status.consecutive_crashes = crashes;
  s_status.forced = forced != 0;
  s_status.safe_mode = s_status.forced || crashes >= RECOVERY_CRASH_LIMIT;
  s_initialized = true;
  if (s_status.safe_mode) {
    ESP_LOGW(TAG, "SAFE MODE active (forced=%s crashes=%u reset=%d)",
             s_status.forced ? "yes" : "no", crashes, s_status.reset_reason);
  } else {
    ESP_LOGI(TAG, "Normal boot (crashes=%u reset=%d)", crashes,
             s_status.reset_reason);
  }
  return ESP_OK;
}

bool recovery_is_safe_mode(void) { return s_status.safe_mode; }

void recovery_get_status(recovery_status_t *status) {
  if (status) *status = s_status;
}

static void healthy_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(60000));
  if (!s_status.safe_mode) {
    if (store_u8(RECOVERY_KEY_CRASHES, 0) == ESP_OK) {
      s_status.consecutive_crashes = 0;
      ESP_LOGI(TAG, "Healthy boot confirmed; crash counter cleared");
    }
  }
  vTaskDelete(NULL);
}

void recovery_mark_services_ready(void) {
  if (s_health_task_started || s_status.safe_mode) return;
  if (xTaskCreate(healthy_task, "boot_health", 2048, NULL, 2, NULL) == pdPASS)
    s_health_task_started = true;
}

esp_err_t recovery_force_safe_mode(bool enabled) {
  esp_err_t err = store_u8(RECOVERY_KEY_FORCED, enabled ? 1 : 0);
  if (err == ESP_OK) {
    s_status.forced = enabled;
    s_status.safe_mode = enabled ||
                         s_status.consecutive_crashes >= RECOVERY_CRASH_LIMIT;
  }
  return err;
}

esp_err_t recovery_clear(void) {
  esp_err_t err = store_u8(RECOVERY_KEY_CRASHES, 0);
  if (err == ESP_OK) err = store_u8(RECOVERY_KEY_FORCED, 0);
  if (err == ESP_OK) {
    s_status.consecutive_crashes = 0;
    s_status.forced = false;
    s_status.safe_mode = false;
  }
  return err;
}
