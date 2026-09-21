#include "source_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "source_mgr";
static SemaphoreHandle_t s_lock;
static source_manager_source_t s_current;
static uint32_t s_switches;

typedef struct {
  source_manager_stop_cb_t stop;
  void *ctx;
} source_slot_t;

static source_slot_t s_slots[SOURCE_MANAGER_AIRPLAY + 1];

static int priority(source_manager_source_t source) { return (int)source; }

esp_err_t source_manager_init(void) {
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
  }
  return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t source_manager_register(source_manager_source_t source,
                                  source_manager_stop_cb_t stop_cb, void *ctx) {
  if (!s_lock || source <= SOURCE_MANAGER_NONE ||
      source > SOURCE_MANAGER_AIRPLAY) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_slots[source].stop = stop_cb;
  s_slots[source].ctx = ctx;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

bool source_manager_acquire(source_manager_source_t source) {
  if (!s_lock || source <= SOURCE_MANAGER_NONE ||
      source > SOURCE_MANAGER_AIRPLAY) {
    return false;
  }

  source_manager_stop_cb_t stop = NULL;
  void *ctx = NULL;
  source_manager_source_t previous;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  previous = s_current;
  if (previous != SOURCE_MANAGER_NONE && previous != source &&
      priority(source) < priority(previous)) {
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "%s denied; %s owns audio", source_manager_name(source),
             source_manager_name(previous));
    return false;
  }
  if (previous != SOURCE_MANAGER_NONE && previous != source) {
    stop = s_slots[previous].stop;
    ctx = s_slots[previous].ctx;
  }
  s_current = source;
  if (previous != source) {
    s_switches++;
  }
  xSemaphoreGive(s_lock);

  /* Never call a backend while holding the manager lock. */
  if (stop) {
    stop(ctx);
  }
  ESP_LOGI(TAG, "Audio owner: %s", source_manager_name(source));
  return true;
}

void source_manager_release(source_manager_source_t source) {
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_current == source) {
    s_current = SOURCE_MANAGER_NONE;
  }
  xSemaphoreGive(s_lock);
}

source_manager_source_t source_manager_current(void) {
  if (!s_lock) {
    return SOURCE_MANAGER_NONE;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  source_manager_source_t value = s_current;
  xSemaphoreGive(s_lock);
  return value;
}

uint32_t source_manager_switch_count(void) { return s_switches; }

const char *source_manager_name(source_manager_source_t source) {
  switch (source) {
  case SOURCE_MANAGER_RADIO: return "radio";
  case SOURCE_MANAGER_SQUEEZELITE: return "squeezelite";
  case SOURCE_MANAGER_SNAPCAST: return "snapcast";
  case SOURCE_MANAGER_DLNA: return "dlna";
  case SOURCE_MANAGER_AIRPLAY: return "airplay";
  default: return "none";
  }
}
