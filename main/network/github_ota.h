#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  GITHUB_OTA_IDLE = 0,
  GITHUB_OTA_CHECKING,
  GITHUB_OTA_AVAILABLE,
  GITHUB_OTA_CURRENT,
  GITHUB_OTA_DOWNLOADING,
  GITHUB_OTA_READY_TO_REBOOT,
  GITHUB_OTA_ERROR,
} github_ota_state_t;

typedef struct {
  github_ota_state_t state;
  bool update_available;
  uint8_t progress_percent;
  char current_version[32];
  char latest_version[32];
  char asset_url[384];
  char error[128];
} github_ota_status_t;

esp_err_t github_ota_check(void);
esp_err_t github_ota_install(void);
void github_ota_get_status(github_ota_status_t *status);
