#include "system_monitor.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "health";
static uint32_t s_restart_count;
static volatile uint32_t s_low_memory_events;

static void monitor_task(void *arg) {
  (void)arg;
  bool reported = false;
  while (1) {
    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal < 48 * 1024) {
      if (!reported) {
        s_low_memory_events++;
        ESP_LOGW(TAG, "Low internal heap: %u bytes", (unsigned)internal);
        reported = true;
      }
    } else if (internal > 64 * 1024) {
      reported = false;
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

esp_err_t system_monitor_init(void) {
  nvs_handle_t nvs;
  if (nvs_open("airplay", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_get_u32(nvs, "boot_count", &s_restart_count);
    s_restart_count++;
    nvs_set_u32(nvs, "boot_count", s_restart_count);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  return xTaskCreate(monitor_task, "health_mon", 2048, NULL, 2, NULL) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

void system_monitor_get(system_health_t *h) {
  if (!h) return;
  h->uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  h->free_heap = esp_get_free_heap_size();
  h->minimum_free_heap = esp_get_minimum_free_heap_size();
  h->largest_internal_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  h->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  h->restart_count = s_restart_count;
  h->low_memory_events = s_low_memory_events;
  h->reset_reason = (int)esp_reset_reason();
}
