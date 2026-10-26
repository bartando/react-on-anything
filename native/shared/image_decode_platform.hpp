// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A host's own image decoder, which the image loader tries before stb_image: Android's (image_decoder.cpp
// there) decodes JPEG with NEON and shrinks while decoding, several times faster on the Shield's cores.
#pragma once

#include <cstddef>

// Straight-alpha RGBA rows of at least `min_width` x `min_height` (the image shrunk by a power of two
// while that still covers them), and their size; null where the host has no decoder or it failed.
// Freed with std::free.
#ifdef __ANDROID__
unsigned char* platform_decode_image(const void* bytes, std::size_t size, int min_width, int min_height, int& width,
                                     int& height);
#else
inline unsigned char* platform_decode_image(const void*, std::size_t, int, int, int&, int&) { return nullptr; }
#endif
