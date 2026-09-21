#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SOURCE_MANAGER_NONE = 0,
  SOURCE_MANAGER_RADIO,
  SOURCE_MANAGER_SQUEEZELITE,
  SOURCE_MANAGER_SNAPCAST,
  SOURCE_MANAGER_DLNA,
  SOURCE_MANAGER_AIRPLAY,
} source_manager_source_t;

typedef void (*source_manager_stop_cb_t)(void *ctx);

/** Central, thread-safe ownership gate for the single audio output. */
esp_err_t source_manager_init(void);
esp_err_t source_manager_register(source_manager_source_t source,
                                  source_manager_stop_cb_t stop_cb, void *ctx);
bool source_manager_acquire(source_manager_source_t source);
void source_manager_release(source_manager_source_t source);
source_manager_source_t source_manager_current(void);
const char *source_manager_name(source_manager_source_t source);
uint32_t source_manager_switch_count(void);
