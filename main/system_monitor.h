#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef struct {
  uint32_t uptime_seconds;
  uint32_t free_heap;
  uint32_t minimum_free_heap;
  uint32_t largest_internal_block;
  uint32_t free_psram;
  uint32_t restart_count;
  uint32_t low_memory_events;
  int reset_reason;
} system_health_t;

esp_err_t system_monitor_init(void);
void system_monitor_get(system_health_t *health);
