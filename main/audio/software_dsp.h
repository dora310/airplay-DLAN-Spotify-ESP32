#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOFTWARE_DSP_PEAK_BANDS 5

typedef enum {
  DSP_CHANNEL_STEREO = 0,
  DSP_CHANNEL_MONO,
  DSP_CHANNEL_LEFT,
  DSP_CHANNEL_RIGHT,
  DSP_CHANNEL_SWAP,
} software_dsp_channel_t;

typedef enum {
  DSP_CROSSOVER_OFF = 0,
  DSP_CROSSOVER_HIGH_PASS,
  DSP_CROSSOVER_LOW_PASS,
} software_dsp_crossover_t;

typedef struct {
  float frequency_hz;
  float gain_db;
  float q;
} software_dsp_band_t;

typedef struct {
  bool enabled;
  bool limiter_enabled;
  bool normalization_enabled;
  float normalization_target_dbfs;
  float balance;
  software_dsp_channel_t channel;
  software_dsp_crossover_t crossover;
  float crossover_hz;
  software_dsp_band_t bands[SOFTWARE_DSP_PEAK_BANDS];
} software_dsp_config_t;

esp_err_t software_dsp_init(uint32_t sample_rate);
void software_dsp_set_sample_rate(uint32_t sample_rate);
void software_dsp_get_config(software_dsp_config_t *config);
esp_err_t software_dsp_set_config(const software_dsp_config_t *config);
void software_dsp_process(int16_t *pcm, size_t frames, int channels);
void software_dsp_reset_state(void);
uint32_t software_dsp_limiter_count(void);
uint32_t software_dsp_clipping_count(void);
bool software_dsp_limiter_active(void);
bool software_dsp_speaker_protection_enabled(void);
uint8_t software_dsp_speaker_threshold_percent(void);
void software_dsp_set_speaker_protection(bool enabled,
                                         uint8_t threshold_percent);
void software_dsp_reset_protection_counters(void);
