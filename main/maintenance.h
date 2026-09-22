#pragma once

#include "esp_err.h"
#include "settings.h"
#include <stdbool.h>
#include <stdint.h>

/** Start OTA validation and idle-only scheduled-restart supervision. */
esp_err_t maintenance_init(void);

/** Called after the configuration server and core services start successfully. */
void maintenance_mark_services_ready(void);

/** Store new maintenance preferences and apply them immediately. */
esp_err_t maintenance_set_config(const settings_maintenance_t *config);

/** Return the active preferences. */
void maintenance_get_config(settings_maintenance_t *config);

/** True when no AirPlay/DLNA/radio source currently owns audio. */
bool maintenance_audio_is_idle(void);

/** Seconds until restart becomes due; zero means disabled or already due. */
uint32_t maintenance_restart_remaining_seconds(void);

/** Whether this boot is still awaiting OTA validity confirmation. */
bool maintenance_ota_pending_verify(void);
