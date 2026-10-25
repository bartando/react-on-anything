// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "image_decode_platform.hpp"

#include <android/imagedecoder.h>
#include <cstdlib>
#include <cstring>

unsigned char* platform_decode_image(const void* bytes, std::size_t size, int min_width, int min_height, int& width,
                                     int& height) {
  AImageDecoder* decoder = nullptr;
  if (AImageDecoder_createFromBuffer(bytes, size, &decoder) != ANDROID_IMAGE_DECODER_SUCCESS) return nullptr;
  const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(decoder);
  int w = AImageDecoderHeaderInfo_getWidth(info), h = AImageDecoderHeaderInfo_getHeight(info);
  // Halving is what a JPEG decoder does for free, inside its transform.
  while (w / 2 >= min_width && h / 2 >= min_height && w / 2 > 0 && h / 2 > 0) w /= 2, h /= 2;
  unsigned char* pixels = nullptr;
  if (AImageDecoder_setAndroidBitmapFormat(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888) == ANDROID_IMAGE_DECODER_SUCCESS &&
      AImageDecoder_setUnpremultipliedRequired(decoder, true) == ANDROID_IMAGE_DECODER_SUCCESS &&
      AImageDecoder_setTargetSize(decoder, w, h) == ANDROID_IMAGE_DECODER_SUCCESS) {
    const std::size_t stride = AImageDecoder_getMinimumStride(decoder), row = static_cast<std::size_t>(w) * 4;
    pixels = static_cast<unsigned char*>(std::malloc(stride * h));
    if (pixels && AImageDecoder_decodeImage(decoder, pixels, stride, stride * h) == ANDROID_IMAGE_DECODER_SUCCESS) {
      if (stride != row)
        for (int y = 1; y < h; ++y) std::memmove(pixels + y * row, pixels + y * stride, row);
      width = w;
      height = h;
    } else {
      std::free(pixels);
      pixels = nullptr;
    }
  }
  AImageDecoder_delete(decoder);
  return pixels;
}
