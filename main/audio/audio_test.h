#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  AUDIO_TEST_BOTH = 0,
  AUDIO_TEST_LEFT,
  AUDIO_TEST_RIGHT,
} audio_test_channel_t;

void audio_test_set_output_ready(bool ready);
bool audio_test_is_running(void);

/** Start a short sine-wave speaker test. Audio must be idle. */
esp_err_t audio_test_start(audio_test_channel_t channel, uint16_t frequency_hz,
                           uint16_t duration_ms);
void audio_test_stop(void);
