#pragma once

#include "esp_err.h"
#include "esp_system.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool safe_mode;
  bool forced;
  uint8_t consecutive_crashes;
  esp_reset_reason_t reset_reason;
} recovery_status_t;

/** Inspect the previous reset and decide whether this boot uses safe mode. */
esp_err_t recovery_init(void);
bool recovery_is_safe_mode(void);
void recovery_get_status(recovery_status_t *status);

/** Clear the crash counter after the receiver remains healthy for 60 seconds. */
void recovery_mark_services_ready(void);

/** Force or clear safe mode for the next boot. */
esp_err_t recovery_force_safe_mode(bool enabled);
esp_err_t recovery_clear(void);
