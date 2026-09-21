#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *pixels;
  size_t data_size;
  uint16_t width;
  uint16_t height;
} artwork_rgb565_t;

bool artwork_decoder_decode_jpeg(const uint8_t *jpeg, size_t jpeg_len,
                                 uint16_t max_dimension,
                                 artwork_rgb565_t *decoded);
void artwork_decoder_free(artwork_rgb565_t *decoded);
