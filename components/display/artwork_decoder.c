#include "artwork_decoder.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "artwork_decoder";

bool artwork_decoder_decode_jpeg(const uint8_t *jpeg, size_t jpeg_len,
                                 uint16_t max_dimension,
                                 artwork_rgb565_t *decoded) {
  (void)jpeg;
  (void)jpeg_len;
  (void)max_dimension;
  if (decoded)
    memset(decoded, 0, sizeof(*decoded));
  ESP_LOGW(TAG, "JPEG decoder is unavailable in this experimental build");
  return false;
}

void artwork_decoder_free(artwork_rgb565_t *decoded) {
  if (!decoded)
    return;
  if (decoded->pixels)
    heap_caps_free(decoded->pixels);
  memset(decoded, 0, sizeof(*decoded));
}
