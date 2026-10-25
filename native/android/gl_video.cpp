// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The presenter's video layers on Android: MediaCodec's SurfaceTexture, drawn through the element's fit
// and clip.
#include "gl_video.hpp"
#include "gl_api.hpp"
#include "media_codec.hpp"

#include <GLES2/gl2ext.h>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// Textures of released pictures, deleted on the GL thread at the next present.
std::mutex retired_lock;
std::vector<GLuint> retired;
GLuint program = 0, vao = 0;

const char* vertex_source = GLSL_VERSION R"(
uniform vec4 dst;   // framebuffer pixels: x0 y0 x1 y1
uniform vec4 src;   // the part of the picture drawn, 0..1: x0 y0 x1 y1; beyond it, `contain`'s black bars
uniform vec2 size;  // the framebuffer
out vec2 at;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 p = mix(dst.xy, dst.zw, corner);
  gl_Position = vec4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
  at = mix(src.xy, src.zw, corner);
}
)";
// The SurfaceTexture's transform maps texture coordinates with t = 0 at the picture's bottom.
const char* fragment_source = R"(#version 320 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES picture;
uniform mat4 transform;
uniform float alpha;
in vec2 at;
out vec4 color;
void main() {
  if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) {
    color = vec4(0.0, 0.0, 0.0, alpha);
    return;
  }
  vec2 t = (transform * vec4(at.x, 1.0 - at.y, 0.0, 1.0)).xy;
  color = vec4(texture(picture, t).rgb, 1.0) * alpha;
}
)";

GLuint compile(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    std::fprintf(stderr, "[video] shader: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

void video::retire_texture(unsigned int texture) {
  if (!texture) return;
  std::lock_guard lock(retired_lock);
  retired.push_back(texture);
}

bool GlVideo::init(bool) {
  if (program) return true;
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source), fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  if (vertex && fragment) {
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
      glDeleteProgram(program);
      program = 0;
    }
  }
  if (vertex) glDeleteShader(vertex);
  if (fragment) glDeleteShader(fragment);
  glGenVertexArrays(1, &vao);
  return program != 0;
}

void GlVideo::release() {
  end_present();
  if (program) glDeleteProgram(program);
  if (vao) glDeleteVertexArrays(1, &vao);
  program = vao = 0;
}

void GlVideo::draw(std::uint64_t, const video::Frame& frame, Fit fit, const Target& target, int x, int y, int w,
                   int h, int fb_w, int fb_h, int, const Compare&) {
  auto* surface = static_cast<video::SurfaceFrame*>(frame.owner.get());
  if (!program || !surface || frame.width <= 0 || frame.height <= 0) return;
  video::CodecOutput& output = *surface->output;
  if (!output.native) return;
  if (!output.texture) {
    glGenTextures(1, &output.texture);
    if (ASurfaceTexture_attachToGLContext(output.native, output.texture) != 0) {
      glDeleteTextures(1, &output.texture);
      output.texture = 0;
      return;
    }
  }
  surface->render();
  // The picture reaches the texture through the codec's thread: latch until it is there, a few
  // milliseconds at most, or show the one before.
  for (int tries = 0; surface->render_ns > 0 && output.latched_ns != surface->render_ns && tries < 8; ++tries) {
    if (tries) std::this_thread::sleep_for(std::chrono::microseconds(500));
    if (ASurfaceTexture_updateTexImage(output.native) != 0) break;
    output.latched_ns = ASurfaceTexture_getTimestamp(output.native);
    ASurfaceTexture_getTransformMatrix(output.native, output.transform);
  }
  if (!output.latched_ns) return;
  // As gl_video.cpp fits the picture into the element's box.
  const float bw = float(target.x1 - target.x0), bh = float(target.y1 - target.y0);
  if (bw <= 0 || bh <= 0) return;
  const float shape = frame.width * frame.aspect / frame.height, box = bw / bh;
  float sx0 = 0, sy0 = 0, sx1 = 1, sy1 = 1;
  if (fit == Fit::contain) {
    if (shape > box) {
      const float part = shape / box;
      sy0 = 0.5f - part / 2;
      sy1 = 0.5f + part / 2;
    } else {
      const float part = box / shape;
      sx0 = 0.5f - part / 2;
      sx1 = 0.5f + part / 2;
    }
  } else if (fit == Fit::cover) {
    if (shape > box) {
      const float part = box / shape;
      sx0 = (1 - part) / 2;
      sx1 = sx0 + part;
    } else {
      const float part = shape / box;
      sy0 = (1 - part) / 2;
      sy1 = sy0 + part;
    }
  }
  // GL counts scissor rows from the bottom of the window.
  const float sx = float(w) / fb_w, sy = float(h) / fb_h;
  const int cx0 = x + int(target.clip_x0 * sx + 0.5f), cx1 = x + int(target.clip_x1 * sx + 0.5f);
  const int cy0 = y + h - int(target.clip_y1 * sy + 0.5f), cy1 = y + h - int(target.clip_y0 * sy + 0.5f);
  if (cx1 <= cx0 || cy1 <= cy0) return;
  glEnable(GL_SCISSOR_TEST);
  glScissor(cx0, cy0, cx1 - cx0, cy1 - cy0);
  glUseProgram(program);
  glBindVertexArray(vao);
  glUniform4f(glGetUniformLocation(program, "dst"), float(target.x0), float(target.y0), float(target.x1),
              float(target.y1));
  glUniform4f(glGetUniformLocation(program, "src"), sx0, sy0, sx1, sy1);
  glUniform2f(glGetUniformLocation(program, "size"), float(fb_w), float(fb_h));
  glUniformMatrix4fv(glGetUniformLocation(program, "transform"), 1, GL_FALSE, output.transform);
  glUniform1f(glGetUniformLocation(program, "alpha"), target.alpha);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, output.texture);
  glUniform1i(glGetUniformLocation(program, "picture"), 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  glDisable(GL_SCISSOR_TEST);
}

void GlVideo::end_present() {
  std::lock_guard lock(retired_lock);
  if (!retired.empty()) glDeleteTextures(static_cast<GLsizei>(retired.size()), retired.data());
  retired.clear();
}

std::string GlVideo::take_upscale_log() { return {}; }
void GlVideo::dump_upscale_at(const std::string&, std::uint64_t) {}

// No compute upscalers on Android yet: setUpscale('') is the only model.
namespace upscalers {
int find(const char*) { return kNone; }
const char* name(int) { return ""; }
int count() { return 0; }
std::string names() { return ""; }
} // namespace upscalers
