#include "optional_backends.h"

#include "audio_output.h"
#include "esp_log.h"

static const char *TAG = "optional";

__attribute__((weak)) esp_err_t snapcast_adapter_start(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
__attribute__((weak)) void snapcast_adapter_stop(void) {
}
__attribute__((weak)) esp_err_t squeezelite_adapter_start(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
__attribute__((weak)) void squeezelite_adapter_stop(void) {
}
__attribute__((weak)) esp_err_t ble_provisioning_adapter_start(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t optional_backend_write_pcm(int16_t *pcm, size_t frames,
                                     uint32_t sample_rate) {
#ifdef CONFIG_AUDIO_OUTPUT_I2S
  audio_output_set_sample_rate(sample_rate);
  return audio_output_write_pcm(pcm, frames, portMAX_DELAY);
#else
  (void)pcm;
  (void)frames;
  (void)sample_rate;
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t optional_backends_init(void) {
  esp_err_t result = ESP_OK;
#ifdef CONFIG_SNAPCAST_ADAPTER_ENABLE
  result = snapcast_adapter_start();
  if (result != ESP_OK)
    ESP_LOGW(TAG, "Snapcast adapter unavailable: %s", esp_err_to_name(result));
#endif
#ifdef CONFIG_SQUEEZELITE_ADAPTER_ENABLE
  result = squeezelite_adapter_start();
  if (result != ESP_OK)
    ESP_LOGW(TAG, "Squeezelite adapter unavailable: %s",
             esp_err_to_name(result));
#endif
#ifdef CONFIG_BLE_PROVISIONING_ENABLE
  result = ble_provisioning_adapter_start();
  if (result != ESP_OK)
    ESP_LOGW(TAG, "BLE provisioning adapter unavailable: %s",
             esp_err_to_name(result));
#endif
  return result == ESP_ERR_NOT_SUPPORTED ? ESP_OK : result;
}
