#include "audio_test.h"

#include "audio_output.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "maintenance.h"
#include <math.h>
#include <stdlib.h>

#define TEST_FRAMES 256
#define TEST_AMPLITUDE 5000.0f
#define TEST_PI 3.14159265358979323846f

typedef struct {
  audio_test_channel_t channel;
  uint16_t frequency_hz;
  uint16_t duration_ms;
} test_request_t;

static const char *TAG = "audio_test";
static volatile bool s_ready;
static volatile bool s_running;
static volatile bool s_stop;

static void test_task(void *arg) {
  test_request_t request = *(test_request_t *)arg;
  free(arg);
  int16_t *pcm = malloc(TEST_FRAMES * 2 * sizeof(int16_t));
  if (!pcm) {
    s_running = false;
    vTaskDelete(NULL);
    return;
  }
  audio_output_stop();
  const uint32_t rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ;
  uint32_t total = ((uint32_t)request.duration_ms * rate) / 1000U;
  float phase = 0.0f;
  float step = 2.0f * TEST_PI * request.frequency_hz / rate;
  for (uint32_t offset = 0; offset < total && !s_stop; offset += TEST_FRAMES) {
    size_t frames = total - offset;
    if (frames > TEST_FRAMES) frames = TEST_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      int16_t sample = (int16_t)(sinf(phase) * TEST_AMPLITUDE);
      phase += step;
      if (phase > 2.0f * TEST_PI) phase -= 2.0f * TEST_PI;
      pcm[i * 2] = request.channel == AUDIO_TEST_RIGHT ? 0 : sample;
      pcm[i * 2 + 1] = request.channel == AUDIO_TEST_LEFT ? 0 : sample;
    }
    if (audio_output_write(pcm, frames * 2 * sizeof(int16_t),
                           pdMS_TO_TICKS(250)) != ESP_OK) break;
  }
  free(pcm);
  audio_output_flush();
  audio_output_start();
  s_running = false;
  s_stop = false;
  ESP_LOGI(TAG, "Speaker test finished");
  vTaskDelete(NULL);
}

void audio_test_set_output_ready(bool ready) { s_ready = ready; }
bool audio_test_is_running(void) { return s_running; }

esp_err_t audio_test_start(audio_test_channel_t channel, uint16_t frequency_hz,
                           uint16_t duration_ms) {
  if (!s_ready) return ESP_ERR_INVALID_STATE;
  if (s_running || !maintenance_audio_is_idle()) return ESP_ERR_INVALID_STATE;
  if (channel > AUDIO_TEST_RIGHT || frequency_hz < 100 || frequency_hz > 4000 ||
      duration_ms < 100 || duration_ms > 5000) return ESP_ERR_INVALID_ARG;
  test_request_t *request = malloc(sizeof(*request));
  if (!request) return ESP_ERR_NO_MEM;
  *request = (test_request_t){channel, frequency_hz, duration_ms};
  s_running = true;
  s_stop = false;
  if (xTaskCreate(test_task, "audio_test", 3072, request, 6, NULL) != pdPASS) {
    free(request);
    s_running = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void audio_test_stop(void) { s_stop = true; }
