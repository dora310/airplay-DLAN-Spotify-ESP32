#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Stable adapter contract for large external engines. These weak functions
 * intentionally do not embed Snapcast, Squeezelite or BLE stacks in the
 * AirPlay build. External components may override them without modifying the
 * AirPlay protocol code.
 */
esp_err_t optional_backends_init(void);
esp_err_t optional_backend_write_pcm(int16_t *pcm, size_t frames,
                                     uint32_t sample_rate);

esp_err_t snapcast_adapter_start(void);
void snapcast_adapter_stop(void);
esp_err_t squeezelite_adapter_start(void);
void squeezelite_adapter_stop(void);
esp_err_t ble_provisioning_adapter_start(void);
