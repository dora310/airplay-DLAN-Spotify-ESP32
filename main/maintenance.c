#include "maintenance.h"

#include "dlna_renderer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "source_manager.h"

static const char *TAG = "maintenance";
static SemaphoreHandle_t s_lock;
static settings_maintenance_t s_config;
static uint64_t s_restart_due_us;
static volatile bool s_ota_pending;
static volatile bool s_services_ready;

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static void set_restart_deadline_locked(void) {
  if (!s_config.scheduled_restart_enabled) {
    s_restart_due_us = 0;
    return;
  }
  s_restart_due_us = now_us() +
      (uint64_t)s_config.scheduled_restart_hours * 3600ULL * 1000000ULL;
}

bool maintenance_audio_is_idle(void) {
  return source_manager_current() == SOURCE_MANAGER_NONE &&
         !dlna_renderer_is_playing();
}

static void maintenance_task(void *arg) {
  (void)arg;
  uint32_t healthy_seconds = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* A new OTA image remains PENDING_VERIFY until it survives 30 seconds.
       A reset/crash before this point lets the bootloader roll it back. */
    if (s_ota_pending) healthy_seconds++;
    if (s_ota_pending && s_services_ready && healthy_seconds >= 30) {
      esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      if (err == ESP_OK) {
        s_ota_pending = false;
        ESP_LOGI(TAG, "OTA image confirmed after stability window");
      } else {
        ESP_LOGE(TAG, "Could not confirm OTA image: %s", esp_err_to_name(err));
      }
    }
    if (s_ota_pending && !s_services_ready && healthy_seconds >= 60) {
      ESP_LOGE(TAG, "New OTA image did not start core services; rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    uint64_t deadline = 0;
    bool enabled = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
      deadline = s_restart_due_us;
      enabled = s_config.scheduled_restart_enabled;
      xSemaphoreGive(s_lock);
    }
    if (enabled && deadline && now_us() >= deadline &&
        maintenance_audio_is_idle()) {
      ESP_LOGI(TAG, "Scheduled restart: receiver is idle");
      vTaskDelay(pdMS_TO_TICKS(250));
      esp_restart();
    }
  }
}

esp_err_t maintenance_init(void) {
  if (s_lock) return ESP_OK;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;
  settings_get_maintenance(&s_config);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  set_restart_deadline_locked();
  xSemaphoreGive(s_lock);

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    s_ota_pending = true;
    ESP_LOGW(TAG, "OTA image pending verification; 30-second rollback window active");
  }

  return xTaskCreate(maintenance_task, "maintenance", 3072, NULL, 3, NULL) ==
                 pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

esp_err_t maintenance_set_config(const settings_maintenance_t *config) {
  if (!config || !s_lock) return ESP_ERR_INVALID_STATE;
  esp_err_t err = settings_set_maintenance(config);
  if (err != ESP_OK) return err;
  settings_maintenance_t saved;
  settings_get_maintenance(&saved);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool schedule_changed =
      saved.scheduled_restart_enabled != s_config.scheduled_restart_enabled ||
      saved.scheduled_restart_hours != s_config.scheduled_restart_hours;
  s_config = saved;
  if (schedule_changed) set_restart_deadline_locked();
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

void maintenance_get_config(settings_maintenance_t *config) {
  if (!config || !s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *config = s_config;
  xSemaphoreGive(s_lock);
}

uint32_t maintenance_restart_remaining_seconds(void) {
  if (!s_lock) return 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  uint64_t deadline = s_restart_due_us;
  bool enabled = s_config.scheduled_restart_enabled;
  xSemaphoreGive(s_lock);
  uint64_t now = now_us();
  if (!enabled || !deadline || now >= deadline) return 0;
  uint64_t seconds = (deadline - now) / 1000000ULL;
  return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

bool maintenance_ota_pending_verify(void) { return s_ota_pending; }

void maintenance_mark_services_ready(void) { s_services_ready = true; }
