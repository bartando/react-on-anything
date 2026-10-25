// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The presenter's video layers on Android. A picture is not drawn with GL: its hardware buffer goes on a
// system layer below the app's window, which the display composes, 10-bit HDR included, and the window
// is left transparent over it. The presenter draws only the hole, and `contain`'s black bars around it;
// what is drawn above the video in the page blends over the hole as it would over the picture.
#include "gl_video.hpp"
#include "gl_api.hpp"
#include "host_platform.hpp"
#include "java.hpp"
#include "media_codec.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
GLuint program = 0, vao = 0;

const char* vertex_source = GLSL_VERSION R"(
uniform vec4 dst;   // framebuffer pixels: x0 y0 x1 y1
uniform vec2 size;  // the framebuffer
out vec2 at;        // framebuffer pixels
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  at = mix(dst.xy, dst.zw, corner);
  gl_Position = vec4(at.x / size.x * 2.0 - 1.0, 1.0 - at.y / size.y * 2.0, 0.0, 1.0);
}
)";
// Black at the element's opacity: blended normally it is the bars; blended with (zero, one minus source
// alpha) inside the picture it takes that much of what is below away, so the layer shows through.
const char* fragment_source = GLSL_VERSION R"(
uniform vec4 picture; // framebuffer pixels: x0 y0 x1 y1
uniform bool hole;
uniform float alpha;
in vec2 at;
out vec4 color;
void main() {
  bool inside = all(greaterThanEqual(at, picture.xy)) && all(lessThan(at, picture.zw));
  if (inside != hole) discard;
  color = vec4(0.0, 0.0, 0.0, alpha);
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

// The window's size in the display's coordinates, which a child layer is placed in: the window's
// buffers are the layout's size, scaled to it.
bool window_size(int& width, int& height) {
  JNIEnv* env = java::env();
  jobject activity = java::activity();
  if (!env || !activity) return false;
  jclass activity_class = env->GetObjectClass(activity);
  jobject window = env->CallObjectMethod(activity, env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;"));
  jobject decor = window ? env->CallObjectMethod(window, env->GetMethodID(env->GetObjectClass(window), "getDecorView",
                                                                         "()Landroid/view/View;"))
                         : nullptr;
  if (java::failed(env) || !decor) return false;
  jclass view = env->GetObjectClass(decor);
  width = env->CallIntMethod(decor, env->GetMethodID(view, "getWidth", "()I"));
  height = env->CallIntMethod(decor, env->GetMethodID(view, "getHeight", "()I"));
  env->DeleteLocalRef(view);
  env->DeleteLocalRef(decor);
  env->DeleteLocalRef(window);
  env->DeleteLocalRef(activity_class);
  return !java::failed(env) && width > 0 && height > 0;
}

// A video's system layer, and the pictures it may still be reading.
struct Layer {
  std::uint64_t id = 0;
  ANativeWindow* parent = nullptr;
  ASurfaceControl* control = nullptr;
  std::shared_ptr<void> showing, before; // the display may read `before` until `showing` is on screen
  ARect source{}, destination{};
  bool drawn = false;
};
std::vector<Layer> layers;
int window_width = 0, window_height = 0;
ANativeWindow* sized_for = nullptr;

void remove(Layer& layer) {
  if (!layer.control) return;
  ASurfaceTransaction* transaction = ASurfaceTransaction_create();
  ASurfaceTransaction_reparent(transaction, layer.control, nullptr);
  ASurfaceTransaction_apply(transaction);
  ASurfaceTransaction_delete(transaction);
  ASurfaceControl_release(layer.control);
  layer.control = nullptr;
}

bool same(const ARect& a, const ARect& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// Puts `frame` on the video's layer, cut to `source` (buffer pixels) and placed at `destination` (window
// pixels); empty rects hide it.
void show(std::uint64_t id, const video::Frame& frame, const ARect& source, const ARect& destination) {
  ANativeWindow* parent = android_window();
  if (!parent) return;
  auto found = std::find_if(layers.begin(), layers.end(), [&](const Layer& l) { return l.id == id; });
  if (found == layers.end()) {
    layers.push_back({});
    found = layers.end() - 1;
    found->id = id;
  }
  Layer& layer = *found;
  layer.drawn = true;
  if (layer.control && layer.parent != parent) remove(layer);
  const bool fresh = !layer.control;
  if (fresh) {
    layer.control = ASurfaceControl_createFromWindow(parent, "video");
    layer.parent = parent;
    if (!layer.control) return;
  }
  auto* picture = static_cast<video::LayerFrame*>(frame.owner.get());
  const bool new_picture = layer.showing != frame.owner;
  if (!fresh && !new_picture && same(layer.source, source) && same(layer.destination, destination)) return;
  ASurfaceTransaction* transaction = ASurfaceTransaction_create();
  if (fresh) {
    // Below the window, which is transparent where the presenter left the hole.
    ASurfaceTransaction_setZOrder(transaction, layer.control, -1);
    ASurfaceTransaction_setVisibility(transaction, layer.control, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
  }
  if (new_picture) {
    ASurfaceTransaction_setBuffer(transaction, layer.control, picture->buffer, -1);
    // The NDK takes the metadata by mutable pointer; it only reads it.
    video::LayerColor color = picture->output->color;
    ASurfaceTransaction_setBufferDataSpace(transaction, layer.control, color.space);
    if (color.has_mastering) ASurfaceTransaction_setHdrMetadata_smpte2086(transaction, layer.control, &color.mastering);
    if (color.has_light_level) ASurfaceTransaction_setHdrMetadata_cta861_3(transaction, layer.control, &color.light_level);
    layer.before = std::move(layer.showing);
    layer.showing = frame.owner;
  }
  ASurfaceTransaction_setGeometry(transaction, layer.control, source, destination, ANATIVEWINDOW_TRANSFORM_IDENTITY);
  ASurfaceTransaction_apply(transaction);
  ASurfaceTransaction_delete(transaction);
  layer.source = source;
  layer.destination = destination;
}
} // namespace

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
  for (Layer& layer : layers) remove(layer);
  layers.clear();
  if (program) glDeleteProgram(program);
  if (vao) glDeleteVertexArrays(1, &vao);
  program = vao = 0;
}

void GlVideo::draw(std::uint64_t id, const video::Frame& frame, Fit fit, const Target& target, int x, int y, int w,
                   int h, int fb_w, int fb_h, int, const Compare&) {
  auto* picture = static_cast<video::LayerFrame*>(frame.owner.get());
  if (!program || !picture || !picture->buffer || frame.width <= 0 || frame.height <= 0) return;
  // The part of the picture the element's box shows, 0..1, as gl_video.cpp fits it; beyond 0..1 are
  // `contain`'s bars.
  const float bw = float(target.x1 - target.x0), bh = float(target.y1 - target.y0);
  if (bw <= 0 || bh <= 0) return;
  const float shape = frame.width * frame.aspect / frame.height, box = bw / bh;
  float sx0 = 0, sy0 = 0, sx1 = 1, sy1 = 1;
  if (fit == Fit::contain) {
    const float part = shape > box ? shape / box : box / shape;
    (shape > box ? sy0 : sx0) = 0.5f - part / 2;
    (shape > box ? sy1 : sx1) = 0.5f + part / 2;
  } else if (fit == Fit::cover) {
    const float part = shape > box ? box / shape : shape / box;
    (shape > box ? sx0 : sy0) = (1 - part) / 2;
    (shape > box ? sx1 : sy1) = (1 - part) / 2 + part;
  }
  // The picture's rect in framebuffer pixels, then the part of it inside the box and the clip.
  const auto at_x = [&](float u) { return target.x0 + (u - sx0) / (sx1 - sx0) * bw; };
  const auto at_y = [&](float v) { return target.y0 + (v - sy0) / (sy1 - sy0) * bh; };
  const float px0 = std::max({at_x(0), float(target.x0), float(target.clip_x0)});
  const float py0 = std::max({at_y(0), float(target.y0), float(target.clip_y0)});
  const float px1 = std::min({at_x(1), float(target.x1), float(target.clip_x1)});
  const float py1 = std::min({at_y(1), float(target.y1), float(target.clip_y1)});

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
  glUniform2f(glGetUniformLocation(program, "size"), float(fb_w), float(fb_h));
  glUniform4f(glGetUniformLocation(program, "picture"), px0, py0, px1, py1);
  glUniform1f(glGetUniformLocation(program, "alpha"), target.alpha);
  glEnable(GL_BLEND);
  glUniform1i(glGetUniformLocation(program, "hole"), 0);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glUniform1i(glGetUniformLocation(program, "hole"), 1);
  glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_SCISSOR_TEST);

  // The layer: the same part of the buffer, in the window's pixels.
  ANativeWindow* window = android_window();
  if (window && window != sized_for && window_size(window_width, window_height)) sized_for = window;
  if (window_width <= 0 || px1 <= px0 || py1 <= py0) return;
  const float wx = float(window_width) / fb_w, wy = float(window_height) / fb_h;
  // The window's buffers are the framebuffer's size (host.cpp), so the viewport is all of them.
  const ARect destination{int(px0 * wx + 0.5f), int(py0 * wy + 0.5f), int(px1 * wx + 0.5f), int(py1 * wy + 0.5f)};
  const auto buffer_x = [&](float p) { return (sx0 + (p - target.x0) / bw * (sx1 - sx0)) * picture->width; };
  const auto buffer_y = [&](float p) { return (sy0 + (p - target.y0) / bh * (sy1 - sy0)) * picture->height; };
  const ARect source{int(buffer_x(px0) + 0.5f), int(buffer_y(py0) + 0.5f), int(buffer_x(px1) + 0.5f),
                     int(buffer_y(py1) + 0.5f)};
  if (source.right <= source.left || source.bottom <= source.top) return;
  show(id, frame, source, destination);
}

// A layer no video drew this present goes: its element is gone or hidden.
void GlVideo::end_present() {
  for (Layer& layer : layers)
    if (!layer.drawn) remove(layer);
  std::erase_if(layers, [](const Layer& layer) { return !layer.control && !layer.drawn; });
  for (Layer& layer : layers) layer.drawn = false;
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
