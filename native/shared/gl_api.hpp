// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

// The GL of each host: 4.1 core on macOS and the PS5, OpenGL ES 3.2 on Android. Shaders start with
// GLSL_VERSION and are written in the subset both dialects share.
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(PROSPERO)
#include <GL/glcorearb.h>
#elif defined(__ANDROID__)
#include <GLES3/gl32.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

#include <cstddef>
#include <cstdint>

#ifdef __ANDROID__
#define GL_ES_HOST 1
#define GLSL_VERSION                                                                                         \
  "#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"            \
  "precision highp samplerBuffer;\n"
#else
#define GL_ES_HOST 0
#define GLSL_VERSION "#version 410 core\n"
#endif

namespace gl {

// Premultiplied 0xAARRGGBB words lie in memory as B, G, R, A. Desktop GL uploads them as BGRA; GLES has
// no such format, so it takes the bytes as RGBA into a texture that swaps red and blue back when sampled.
inline void tex_image_argb(int width, int height, const void* pixels) {
#if GL_ES_HOST
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#else
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
#endif
}

// RGB565 pixels into the bound texture.
inline void tex_image_rgb565(int width, int height, const void* pixels) {
#if GL_ES_HOST
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
#else
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
#endif
}

// A rect of the bound read framebuffer as ARGB words, rows as GL stores them (bottom first).
inline void read_argb(int x, int y, int width, int height, std::uint32_t* out) {
#if GL_ES_HOST
  glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out);
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t p = out[i];
    out[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
  }
#else
  glReadPixels(x, y, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out);
#endif
}

}  // namespace gl
