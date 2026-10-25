// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "gpu_renderer.hpp"

#include "gl_presenter.hpp"

#include "gl_api.hpp"

extern "C" {
#include "er_canvas.h"
#include "native_renderer.h"
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

struct GpuRenderer::Op {
  ERCanvasOp op;
  std::vector<std::uint32_t> pixels;  // ER_CANVAS_PIXELS: a copy, the engine's buffer is the call's only
};

namespace {

std::mutex record_lock;

// The pixel's coverage by a rounded rect (x, y, w, h; radii tl, tr, br, bl), from its signed distance at
// the pixel centre: the software rasterizer's analytic fringe, within a level or two.
constexpr const char* kCoverage = R"(
float rbox(vec2 c, vec4 rect, vec4 radii) {
  vec2 half_size = rect.zw * 0.5;
  vec2 p = c - (rect.xy + half_size);
  float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
  vec2 q = abs(p) - half_size + r;
  float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
  return clamp(0.5 - d, 0.0, 1.0);
}
uniform int u_clips;
uniform vec4 u_clip_rect[4];
uniform vec4 u_clip_radii[4];
// Rounded clips: rect clips are the scissor.
float clipped(vec2 c) {
  float cov = 1.0;
  for (int i = 0; i < u_clips; i++) cov *= rbox(c, u_clip_rect[i], u_clip_radii[i]);
  return cov;
}
)";

constexpr const char* kVertex = GLSL_VERSION R"(
uniform vec2 u_size;
uniform vec2 u_origin;
uniform vec4 u_quad;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 local = u_quad.xy + corner * u_quad.zw - u_origin;
  gl_Position = vec4(local / u_size * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Rounded rects: mode 0 fills, 1 fills inside a border, 2 strokes the border only. Mode 5 is the
// animated focus ring (border_sweep.c): a ring inside the rect, bright at its head and fading behind it.
constexpr const char* kShapeFragment = R"(
uniform vec2 u_origin;
uniform vec4 u_rect;
uniform vec4 u_radii;
uniform vec4 u_edges;
uniform vec4 u_inner_radii;
uniform int u_mode;
uniform vec4 u_fill;
uniform vec4 u_edge_color[4];
uniform float u_alpha;
// Gradients (mode 3 linear, 4 radial), as the software ramp: a table of n premultiplied 8.8 colours,
// read at a fixed-point index and dithered with a 4x4 Bayer threshold per screen pixel.
uniform vec4 u_stop_color[4];  // straight a, r, g, b in 0..255
uniform float u_stop_pos[4];
uniform int u_stop_count;
uniform vec2 u_dir;
uniform float u_min_p;
uniform float u_to_idx;
uniform int u_step;
uniform int u_lut_n;
// The sweep: the ring (outer radius, width, inner radius), its centre line (straight half-lengths a, b and
// corner radius), and the head's place on the perimeter and the tail's length, as fractions of it.
uniform vec3 u_ring;
uniform vec3 u_line;
uniform vec2 u_sweep;
out vec4 frag;
const float kPi = 3.14159265;
float sd_rrect(vec2 p, vec2 half_size, float r) {
  vec2 q = abs(p) - (half_size - r);
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float perimeter_at(vec2 p, float a, float b, float r) {
  float quarter = r * kPi * 0.5;
  float total = 4.0 * a + 4.0 * b + 4.0 * quarter;
  if (total <= 0.0) return 0.0;
  float s;
  if (abs(p.x) <= a && p.y < 0.0) s = p.x + a;
  else if (p.x > a && p.y < -b) s = 2.0 * a + r * (atan(p.y + b, p.x - a) + kPi * 0.5);
  else if (p.x > 0.0 && abs(p.y) <= b) s = 2.0 * a + quarter + (p.y + b);
  else if (p.x > a && p.y > b) s = 2.0 * a + 2.0 * b + quarter + r * atan(p.y - b, p.x - a);
  else if (abs(p.x) <= a && p.y > 0.0) s = 2.0 * a + 2.0 * b + 2.0 * quarter + (a - p.x);
  else if (p.x < -a && p.y > b) s = 4.0 * a + 2.0 * b + 2.0 * quarter + r * (atan(p.y - b, p.x + a) - kPi * 0.5);
  else if (p.x < 0.0 && abs(p.y) <= b) s = 4.0 * a + 2.0 * b + 3.0 * quarter + (b - p.y);
  else s = 4.0 * a + 4.0 * b + 3.0 * quarter + r * (atan(p.y + b, p.x + a) + kPi);
  return s / total;
}
float sweep_alpha(float t) {
  const float lead = 0.01;
  float d = u_sweep.x - t;
  d -= floor(d);
  if (d > 1.0 - lead) return (d - (1.0 - lead)) / lead;
  if (d >= u_sweep.y) return 0.0;
  float f = 1.0 - d / u_sweep.y;
  return f * f;
}
vec4 sweep(vec2 c) {
  vec2 half_size = u_rect.zw * 0.5;
  vec2 p = c - (u_rect.xy + half_size);
  float cov = clamp(0.5 - sd_rrect(p, half_size, u_ring.x), 0.0, 1.0);
  cov *= clamp(0.5 + sd_rrect(p, half_size - u_ring.y, u_ring.z), 0.0, 1.0);
  return u_fill * (cov * sweep_alpha(perimeter_at(p, u_line.x, u_line.y, u_line.z)));
}
const int kBayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
vec4 premul_f(vec4 s) { return vec4(s.x, s.y * s.x / 255.0, s.z * s.x / 255.0, s.w * s.x / 255.0); }
vec4 lut(int idx) {
  float t = float(idx) / float(u_lut_n - 1);
  int last = u_stop_count - 1;
  vec4 c;
  if (t <= u_stop_pos[0]) c = premul_f(u_stop_color[0]);
  else if (t >= u_stop_pos[last]) c = premul_f(u_stop_color[last]);
  else {
    int seg = 0;
    for (int i = 0; i < 3; i++) if (i < last && t > u_stop_pos[i + 1]) seg = i + 1;
    vec4 c0 = premul_f(u_stop_color[seg]), c1 = premul_f(u_stop_color[seg + 1]);
    float span = u_stop_pos[seg + 1] - u_stop_pos[seg];
    float lt = span > 0.0 ? (t - u_stop_pos[seg]) / span : 1.0;
    c = c0 + (c1 - c0) * lt;
  }
  vec4 v = floor(c * 256.0 + 0.5);
  return vec4(v.x, min(v.yzw, vec3(v.x)));
}
vec4 gradient(vec2 c) {
  ivec2 screen = ivec2(floor(c));
  ivec2 local = screen - ivec2(u_rect.xy);
  int idx;
  if (u_mode == 3) {
    int f = int(round((float(local.y) * u_dir.y - u_min_p) * u_to_idx * 65536.0)) + local.x * u_step;
    idx = clamp((f + 32768) >> 16, 0, u_lut_n - 1);
  } else {
    vec2 centre = (u_rect.zw - 1.0) * 0.5;
    vec2 d = vec2(local) - centre;
    idx = min(int(sqrt(d.x * d.x + d.y * d.y) * u_to_idx + 0.5), u_lut_n - 1);
  }
  float threshold = float(kBayer[(screen.y & 3) * 4 + (screen.x & 3)] * 16 + 8);
  vec4 argb = floor((lut(idx) + threshold) / 256.0);
  return vec4(argb.yzw, argb.x) / 255.0;
}
vec4 edge_color(vec2 c, vec4 inner) {
  float dl = (inner.x - c.x) / max(u_edges.x, 0.001);
  float dt = (inner.y - c.y) / max(u_edges.y, 0.001);
  float dr = (c.x - inner.x - inner.z) / max(u_edges.z, 0.001);
  float db = (c.y - inner.y - inner.w) / max(u_edges.w, 0.001);
  float best = dl; vec4 color = u_edge_color[0];
  if (dt > best) { best = dt; color = u_edge_color[1]; }
  if (dr > best) { best = dr; color = u_edge_color[2]; }
  if (db > best) { best = db; color = u_edge_color[3]; }
  return color;
}
void main() {
  vec2 c = gl_FragCoord.xy + u_origin;
  float outer = rbox(c, u_rect, u_radii);
  vec4 color;
  if (u_mode == 0) {
    color = u_fill * outer;
  } else if (u_mode == 5) {
    color = sweep(c);
  } else if (u_mode >= 3) {
    color = gradient(c) * outer;
  } else {
    vec4 inner = vec4(u_rect.xy + u_edges.xy, u_rect.zw - u_edges.xy - u_edges.zw);
    float in_cov = (inner.z > 0.0 && inner.w > 0.0) ? rbox(c, inner, u_inner_radii) : 0.0;
    vec4 fill = u_mode == 2 ? vec4(0.0) : u_fill;
    color = edge_color(c, inner) * max(outer - in_cov, 0.0) + fill * min(in_cov, outer);
  }
  frag = color * (u_alpha * clipped(c));
}
)";

// Pixels from a texture: kind 0 copies texel (floor(c) - shift), 1 maps the pixel through an inverse
// 2D transform and samples bilinearly with 8-bit weights, as the software transform does.
constexpr const char* kImageFragment = R"(
uniform vec2 u_origin;
uniform sampler2D u_tex;
uniform int u_kind;
uniform vec2 u_shift;
uniform vec2 u_src_size;
uniform float u_m[6];
uniform float u_alpha;
uniform vec4 u_color;
uniform int u_italic;
uniform vec4 u_dst;       // image region: where it lands
uniform vec4 u_src;       // and the part of the image it scales
uniform vec4 u_tint;      // rgb 0..255, a = 1 when tinted
uniform vec4 u_mask;      // the box a rounded image is cut to
uniform vec4 u_mask_radii;
out vec4 frag;
// One axis of the software scaler: the source position of destination pixel d, clamped to the region,
// as two texels and an 8-bit weight.
ivec3 axis(float start, float len, float dst_len, int d) {
  float s = start + (float(d) + 0.5) * len / dst_len - 0.5;
  s = clamp(s, start, start + len - 1.0);
  int i0 = int(s);
  int i1 = float(i0 + 1) < start + len ? i0 + 1 : i0;
  return ivec3(i0, i1, int((s - float(i0)) * 256.0 + 0.5));
}
// (a * (256 - f) + b * f) >> 8 per 8-bit channel, as the software lerp truncates.
vec4 lerp8(vec4 a, vec4 b, int f) {
  return floor((floor(a * 255.0 + 0.5) * float(256 - f) + floor(b * 255.0 + 0.5) * float(f)) / 256.0) / 255.0;
}
float cov(int d, int row) {
  if (d < 0 || float(d) >= u_src_size.x) return 0.0;
  return texelFetch(u_tex, ivec2(d, row), 0).r;
}
vec4 tap(ivec2 t) {
  if (t.x < 0 || t.y < 0 || float(t.x) >= u_src_size.x || float(t.y) >= u_src_size.y) return vec4(0.0);
  return texelFetch(u_tex, t, 0);
}
void main() {
  vec2 c = gl_FragCoord.xy + u_origin;
  vec2 s = floor(c);
  vec4 color;
  if (u_kind == 0) {
    color = tap(ivec2(s - u_shift));
  } else if (u_kind == 3) {
    ivec2 d = ivec2(s - u_dst.xy);
    ivec3 cx = axis(u_src.x, u_src.z, u_dst.z, d.x);
    ivec3 cy = axis(u_src.y, u_src.w, u_dst.w, d.y);
    vec4 top = lerp8(texelFetch(u_tex, ivec2(cx.x, cy.x), 0), texelFetch(u_tex, ivec2(cx.y, cy.x), 0), cx.z);
    vec4 bottom = lerp8(texelFetch(u_tex, ivec2(cx.x, cy.y), 0), texelFetch(u_tex, ivec2(cx.y, cy.y), 0), cx.z);
    color = lerp8(top, bottom, cy.z);
    if (u_tint.a > 0.0) color.rgb = floor(u_tint.rgb * floor(color.a * 255.0 + 0.5) / 255.0) / 255.0;
    if (u_mask.z > 0.0) color *= rbox(c, u_mask, u_mask_radii);
  } else if (u_kind == 2) {
    // A glyph's coverage times its colour; italic shears each row right by (height - 1 - row) * 0.2,
    // the fraction split between two columns as the software path does (the 1-bit path drops it).
    ivec2 local = ivec2(s - u_shift);
    if (local.y < 0 || float(local.y) >= u_src_size.y) discard;
    float shift = u_italic != 0 ? (u_src_size.y - 1.0 - float(local.y)) * 0.2 : 0.0;
    int whole = int(shift);
    float part = u_italic == 2 ? shift - float(whole) : 0.0;
    int d = local.x - whole;
    float c = part < 0.005 ? cov(d, local.y) : cov(d, local.y) * (1.0 - part) + cov(d - 1, local.y) * part;
    color = u_color * c;
  } else {
    float fx = u_m[0] * s.x + u_m[2] * s.y + u_m[4] - u_shift.x;
    float fy = u_m[1] * s.x + u_m[3] * s.y + u_m[5] - u_shift.y;
    if (!(fx >= -1.0 && fx < u_src_size.x && fy >= -1.0 && fy < u_src_size.y)) discard;
    vec2 f0 = floor(vec2(fx, fy));
    vec2 w = floor((vec2(fx, fy) - f0) * 256.0) / 256.0;
    ivec2 t = ivec2(f0);
    vec4 top = mix(tap(t), tap(t + ivec2(1, 0)), w.x);
    vec4 bottom = mix(tap(t + ivec2(0, 1)), tap(t + ivec2(1, 1)), w.x);
    color = mix(top, bottom, w.y);
  }
  frag = color * (u_alpha * clipped(c));
}
)";

GLuint compile(GLenum type, const std::string& source) {
  GLuint shader = glCreateShader(type);
  const char* text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    std::fprintf(stderr, "[PS5-REACT] gpu mirror: shader failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint link(const char* fragment_body) {
  GLuint vertex = compile(GL_VERTEX_SHADER, kVertex);
  GLuint fragment = compile(GL_FRAGMENT_SHADER, std::string(GLSL_VERSION) + kCoverage + fragment_body);
  if (!vertex || !fragment) return 0;
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

// Straight-alpha ARGB8888 as premultiplied RGBA floats.
std::array<float, 4> premultiplied(std::uint32_t argb) {
  const float a = static_cast<float>(argb >> 24) / 255.0f;
  return {static_cast<float>((argb >> 16) & 0xFF) / 255.0f * a, static_cast<float>((argb >> 8) & 0xFF) / 255.0f * a,
          static_cast<float>(argb & 0xFF) / 255.0f * a, a};
}

bool make_target(int width, int height, unsigned int& texture, unsigned int& fbo) {
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return ok;
}

void free_target(unsigned int& texture, unsigned int& fbo) {
  if (fbo) glDeleteFramebuffers(1, &fbo);
  if (texture) glDeleteTextures(1, &texture);
  texture = fbo = 0;
}

constexpr const char* kKindNames[ER_CANVAS_OP_COUNT] = {
    "fill", "rrect", "rrect-bordered", "rrect-ring", "rrect-ring-edges", "gradient", "shadow", "border-sweep",
    "border-conic", "border-radial", "arc-sector", "vector", "vector-slot", "text", "image", "clear",
    "clip-rect", "clip-rrect", "clip-reset", "clip-pop", "alpha", "blend", "group-begin", "group-end",
    "group-draw", "group-xform", "group-keep", "unhooked", "move", "glyph", "image-region", "pixels"};

bool write_bmp(const std::string& path, const std::vector<std::uint32_t>& argb, int width, int height) {
  const int w = width / 2, h = height / 2, row = (w * 3 + 3) & ~3;
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  const std::uint32_t data = static_cast<std::uint32_t>(row) * static_cast<std::uint32_t>(h);
  std::uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int at, std::uint32_t v) { std::memcpy(header + at, &v, 4); };
  put32(2, 54 + data);
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<std::uint32_t>(w));
  put32(22, static_cast<std::uint32_t>(-h));  // top-down
  header[26] = 1;
  header[28] = 24;
  put32(34, data);
  std::fwrite(header, 1, sizeof header, file);
  std::vector<std::uint8_t> line(static_cast<std::size_t>(row));
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const std::uint32_t p = argb[static_cast<std::size_t>(y * 2) * width + x * 2];
      line[x * 3] = p & 0xFF;
      line[x * 3 + 1] = (p >> 8) & 0xFF;
      line[x * 3 + 2] = (p >> 16) & 0xFF;
    }
    std::fwrite(line.data(), 1, line.size(), file);
  }
  return std::fclose(file) == 0;
}

}  // namespace

GpuRenderer::~GpuRenderer() { stop(); }

bool GpuRenderer::start(int width, int height, Mode mode) {
  mode_ = mode;
  width_ = width;
  height_ = height;
  shape_ = link(kShapeFragment);
  image_ = link(kImageFragment);
  if (!shape_ || !image_) return false;
  glGenVertexArrays(1, &vao_);
  if (!make_target(width, height, target_.texture, target_.fbo) ||
      !make_target(width, height, copy_.texture, copy_.fbo))
    return false;
  target_.width = copy_.width = width;
  target_.height = copy_.height = height;
  ops_ = new std::vector<Op>();
  ops_->reserve(65536);
  missing_.assign(ER_CANVAS_OP_COUNT, 0);
  er_set_canvas(&GpuRenderer::record, this);
  return true;
}

void GpuRenderer::stop() {
  if (!ops_) return;
  er_set_canvas(nullptr, nullptr);
  free_target(target_.texture, target_.fbo);
  free_target(copy_.texture, copy_.fbo);
  free_target(screen_.texture, screen_.fbo);
  for (auto& [buffer, group] : groups_) free_target(group.target.texture, group.target.fbo);
  groups_.clear();
  for (auto& [bitmap, g] : glyphs_) glDeleteTextures(1, &g.texture);
  glyphs_.clear();
  for (auto& [pixels, img] : images_) glDeleteTextures(1, &img.texture);
  images_.clear();
  if (pixels_) glDeleteTextures(1, &pixels_);
  pixels_ = 0;
  if (vao_) glDeleteVertexArrays(1, &vao_);
  uniforms_.clear();
  uniform_index_.clear();
  if (shape_) glDeleteProgram(shape_);
  if (image_) glDeleteProgram(image_);
  vao_ = shape_ = image_ = 0;
  delete ops_;
  ops_ = nullptr;
}

bool GpuRenderer::takes(int kind) const {
  switch (kind) {
    case ER_CANVAS_FILL:
    case ER_CANVAS_RRECT:
    case ER_CANVAS_RRECT_BORDERED:
    case ER_CANVAS_RRECT_RING:
    case ER_CANVAS_RRECT_RING_EDGES:
    case ER_CANVAS_CLEAR:
    case ER_CANVAS_CLIP_RECT:
    case ER_CANVAS_CLIP_RRECT:
    case ER_CANVAS_CLIP_RESET:
    case ER_CANVAS_CLIP_POP:
    case ER_CANVAS_ALPHA:
    case ER_CANVAS_BLEND:
    case ER_CANVAS_GROUP_BEGIN:
    case ER_CANVAS_GROUP_END:
    case ER_CANVAS_GROUP_DRAW:
    case ER_CANVAS_GROUP_XFORM:
    case ER_CANVAS_GROUP_KEEP:
    case ER_CANVAS_MOVE:
    case ER_CANVAS_GLYPH:
    case ER_CANVAS_GRADIENT:
    case ER_CANVAS_IMAGE_REGION:
    case ER_CANVAS_BORDER_SWEEP:
      return true;
    default:
      return false;
  }
}

// What the mirror has no shader for yet: the engine draws it into a buffer, and the mirror draws the pixels.
bool GpuRenderer::captures(int kind) {
  switch (kind) {
    case ER_CANVAS_SHADOW:
    case ER_CANVAS_BORDER_CONIC:
    case ER_CANVAS_BORDER_RADIAL:
    case ER_CANVAS_ARC_SECTOR:
    case ER_CANVAS_VECTOR:
    case ER_CANVAS_VECTOR_SLOT:
      return true;
    default:
      return false;
  }
}

// Called by the engine for every op: keeps the ops a GPU renderer would be handed, and counts the ones it
// could not draw. A mirror takes none of them (the software rasterizer must still draw them).
int GpuRenderer::record(const ERCanvasOp* op, void* self_raw) {
  auto* self = static_cast<GpuRenderer*>(self_raw);
  std::lock_guard<std::mutex> guard(record_lock);
  // What a GPU renderer is handed: the ops at depth 0, and the glyphs of a text block it leaves to the
  // engine to lay out.
  if (op->kind >= ER_CANVAS_OP_COUNT ||
      (op->depth != 0 && op->kind != ER_CANVAS_GLYPH && op->kind != ER_CANVAS_IMAGE_REGION))
    return ER_CANVAS_PASS;
  // Text and images are left to the engine to lay out; their glyphs and regions come next.
  if (op->kind == ER_CANVAS_TEXT || op->kind == ER_CANVAS_IMAGE) return ER_CANVAS_PASS;
  if (captures(op->kind) && op->bounds[2] > 0) {
    self->stats_.captures[op->kind]++;
    self->stats_.capture_px[op->kind] += static_cast<std::uint64_t>(op->bounds[2]) * op->bounds[3];
    return ER_CANVAS_CAPTURE;
  }
  if (op->kind == ER_CANVAS_PIXELS) {
    const auto* px = static_cast<const std::uint32_t*>(op->buffer);
    Op kept{*op, {}};
    kept.pixels.reserve(static_cast<std::size_t>(op->w) * op->h);
    for (int y = 0; y < op->h; y++)
      kept.pixels.insert(kept.pixels.end(), px + static_cast<std::size_t>(y) * op->width,
                         px + static_cast<std::size_t>(y) * op->width + op->w);
    kept.op.buffer = nullptr;
    self->ops_->push_back(std::move(kept));
    return self->kept();
  }
  // A 3D transform carries a homography; only the 2D map is drawn so far.
  if (!self->takes(op->kind) || (op->kind == ER_CANVAS_GROUP_XFORM && op->f[0] == 3.0f)) {
    self->missing_[op->kind]++;
    return ER_CANVAS_PASS;
  }
  self->ops_->push_back(Op{*op, {}});
  return self->kept();
}

// A uniform's location and last value, looked up once per program and name (the names are literals).
GpuRenderer::Uniform& GpuRenderer::uniform(unsigned int program, const char* name) {
  const std::uint64_t key = (static_cast<std::uint64_t>(program) << 48) ^ reinterpret_cast<std::uintptr_t>(name);
  auto found = uniform_index_.find(key);
  if (found != uniform_index_.end()) return uniforms_[found->second];
  uniform_index_.emplace(key, uniforms_.size());
  uniforms_.push_back({glGetUniformLocation(program, name), -1, {}});
  return uniforms_.back();
}

// Whether `data` differs from what the uniform holds, keeping it if so: a uniform keeps its value in
// its program, and the driver charges for every call, changed or not.
bool GpuRenderer::changes(Uniform& u, const void* data, int bytes) {
  if (u.location < 0) return false;
  if (u.bytes == bytes && !std::memcmp(u.value, data, static_cast<std::size_t>(bytes))) return false;
  u.bytes = bytes;
  std::memcpy(u.value, data, static_cast<std::size_t>(bytes));
  return true;
}

void GpuRenderer::set_i(unsigned int program, const char* name, int value) {
  Uniform& u = uniform(program, name);
  if (changes(u, &value, sizeof value)) glUniform1i(u.location, value);
}

void GpuRenderer::set_f(unsigned int program, const char* name, std::initializer_list<float> values) {
  Uniform& u = uniform(program, name);
  if (!changes(u, values.begin(), static_cast<int>(values.size() * sizeof(float)))) return;
  switch (values.size()) {
    case 1: glUniform1fv(u.location, 1, values.begin()); break;
    case 2: glUniform2fv(u.location, 1, values.begin()); break;
    case 3: glUniform3fv(u.location, 1, values.begin()); break;
    default: glUniform4fv(u.location, 1, values.begin()); break;
  }
}

void GpuRenderer::set_v4(unsigned int program, const char* name, const float* values, int count) {
  Uniform& u = uniform(program, name);
  if (changes(u, values, count * 4 * static_cast<int>(sizeof(float)))) glUniform4fv(u.location, count, values);
}

void GpuRenderer::set_v1(unsigned int program, const char* name, const float* values, int count) {
  Uniform& u = uniform(program, name);
  if (changes(u, values, count * static_cast<int>(sizeof(float)))) glUniform1fv(u.location, count, values);
}

GpuRenderer::Group& GpuRenderer::group(const void* buffer, int w, int h) {
  for (auto& [key, group] : groups_) {
    if (key != buffer) continue;
    if (group.target.width != w || group.target.height != h) {
      stats_.regroups++;
      free_target(group.target.texture, group.target.fbo);
      make_target(w, h, group.target.texture, group.target.fbo);
      group.target.width = w;
      group.target.height = h;
    }
    return group;
  }
  Group fresh;
  make_target(w, h, fresh.target.texture, fresh.target.fbo);
  fresh.target.width = w;
  fresh.target.height = h;
  groups_.emplace_back(buffer, fresh);
  return groups_.back().second;
}

// A baked glyph's coverage as an 8-bit texture, unpacked once from its 1, 2, 4 or 8 bits per pixel.
const GpuRenderer::Glyph& GpuRenderer::glyph(const void* bitmap, int w, int h, int bpp) {
  auto found = glyphs_.find(bitmap);
  if (found != glyphs_.end()) return found->second;
  const auto* bits = static_cast<const std::uint8_t*>(bitmap);
  const int stride = bpp == 8 ? w : bpp == 4 ? (w + 1) / 2 : bpp == 2 ? (w + 3) / 4 : (w + 7) / 8;
  std::vector<std::uint8_t> coverage(static_cast<std::size_t>(w) * h);
  for (int y = 0; y < h; y++) {
    const std::uint8_t* row = bits + static_cast<std::size_t>(y) * stride;
    for (int x = 0; x < w; x++) {
      std::uint8_t c;
      if (bpp == 8) c = row[x];
      else if (bpp == 4) c = static_cast<std::uint8_t>(((x & 1) ? row[x >> 1] & 0x0F : row[x >> 1] >> 4) * 17);
      else if (bpp == 2) c = static_cast<std::uint8_t>(((row[x >> 2] >> (6 - (x & 3) * 2)) & 3) * 85);
      else c = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
      coverage[static_cast<std::size_t>(y) * w + x] = c;
    }
  }
  Glyph g{0, w, h};
  glGenTextures(1, &g.texture);
  glBindTexture(GL_TEXTURE_2D, g.texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  stats_.uploads++;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, coverage.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return glyphs_.emplace(bitmap, g).first->second;
}

// An image's pixels as a texture, uploaded once per buffer (premultiplied ARGB8888, or RGB565).
const GpuRenderer::Image& GpuRenderer::image(const void* pixels, const char* name, int w, int h, int format) {
  auto found = images_.find(pixels);
  if (found != images_.end()) {
    Image& kept = found->second;
    // A buffer freed and reused for another image: upload again.
    if (kept.width == w && kept.height == h && kept.format == format && kept.name == (name ? name : "")) return kept;
    glDeleteTextures(1, &kept.texture);
    images_.erase(found);
  }
  stats_.uploads++;
  Image img{0, w, h, format, name ? name : ""};
  glGenTextures(1, &img.texture);
  glBindTexture(GL_TEXTURE_2D, img.texture);
  if (format == ER_IMG_RGB565) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    gl::tex_image_rgb565(w, h, pixels);
  } else {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl::tex_image_argb(w, h, pixels);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return images_.emplace(pixels, img).first->second;
}

void GpuRenderer::replay() {
  if (!active()) return;
  std::lock_guard<std::mutex> guard(record_lock);
  const auto began = std::chrono::steady_clock::now();
  stats_.ops += ops_->size();
  if (!ops_->empty()) draw_ops();
  ops_->clear();
  const auto now = std::chrono::steady_clock::now();
  const auto took = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - began).count());
  stats_.replays++;
  stats_.replay_us += took;
  stats_.max_replay_us = std::max(stats_.max_replay_us, took);
  const std::int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  if (!stats_.window_start_us) stats_.window_start_us = now_us;
  if (now_us - stats_.window_start_us >= 2000000) {
    const double n = static_cast<double>(stats_.replays);
    std::printf("[PS5-REACT] gpu: replays=%llu avg=%.2fms max=%.2fms | per replay: ops=%.0f draws=%.0f regroups=%.1f "
                "pixels=%.1f (%.0fKB) uploads=%.1f\n",
                static_cast<unsigned long long>(stats_.replays), stats_.replay_us / n / 1000.0,
                stats_.max_replay_us / 1000.0, stats_.ops / n, stats_.draws / n, stats_.regroups / n,
                stats_.pixel_ops / n, stats_.pixel_bytes / n / 1024.0, stats_.uploads / n);
    for (int kind = 0; kind < ER_CANVAS_OP_COUNT; kind++)
      if (stats_.captures[kind])
        std::printf("[PS5-REACT] gpu:   capture %s: %.1f per replay, %.0f kpx each (bounds, before clipping)\n",
                    kKindNames[kind], stats_.captures[kind] / n,
                    stats_.capture_px[kind] / 1000.0 / static_cast<double>(stats_.captures[kind]));
    stats_ = {};
    stats_.window_start_us = now_us;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ZERO);
  glUseProgram(0);
  glBindVertexArray(0);
}

void GpuRenderer::draw_ops() {
  struct Bound { const Target* target; int ox, oy; };
  std::vector<Bound> targets{{&target_, 0, 0}};
  std::vector<Clip> clips;
  float alpha = 1.0f;
  int blend = 0;

  glBindVertexArray(vao_);
  glEnable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_SCISSOR_TEST);
  // GL state as last set here: a draw sets only what changed.
  GLuint used = 0, bound = 0;
  int blend_set = -1;
  bool scissoring = false;
  int scissor[4] = {};
  auto scissor_off = [&]() {
    if (!scissoring) return;
    glDisable(GL_SCISSOR_TEST);
    scissoring = false;
  };
  auto scissor_to = [&](int x, int y, int w, int h) {
    if (!scissoring) glEnable(GL_SCISSOR_TEST);
    if (!scissoring || x != scissor[0] || y != scissor[1] || w != scissor[2] || h != scissor[3]) {
      glScissor(x, y, w, h);
      scissor[0] = x, scissor[1] = y, scissor[2] = w, scissor[3] = h;
    }
    scissoring = true;
  };
  auto bind_texture = [&](GLuint texture) {
    if (texture == bound) return;
    glBindTexture(GL_TEXTURE_2D, texture);
    bound = texture;
  };

  auto bind = [&]() {
    const Bound& b = targets.back();
    glBindFramebuffer(GL_FRAMEBUFFER, b.target->fbo);
    glViewport(0, 0, b.target->width, b.target->height);
  };
  // Scissor = the innermost clip rect, in the bound target's pixels; the rounded clips go to the shader.
  auto apply_clip = [&](GLuint program) {
    const Bound& b = targets.back();
    if (clips.empty()) {
      scissor_off();
    } else {
      const Clip& top = clips.back();
      scissor_to(top.x - b.ox, top.y - b.oy, std::max(top.w, 0), std::max(top.h, 0));
    }
    float rects[16] = {}, radii[16] = {};
    int count = 0;
    for (auto it = clips.rbegin(); it != clips.rend() && count < 4; ++it) {
      if (it->reset) break;
      if (!it->rounded) continue;
      rects[count * 4] = static_cast<float>(it->bx);
      rects[count * 4 + 1] = static_cast<float>(it->by);
      rects[count * 4 + 2] = static_cast<float>(it->bw);
      rects[count * 4 + 3] = static_cast<float>(it->bh);
      std::copy(it->radii, it->radii + 4, radii + count * 4);
      count++;
    }
    set_i(program, "u_clips", count);
    set_v4(program, "u_clip_rect", rects, 4);
    set_v4(program, "u_clip_radii", radii, 4);
  };
  auto begin_draw = [&](GLuint program, int x, int y, int w, int h) {
    const Bound& b = targets.back();
    if (program != used) glUseProgram(program);
    used = program;
    set_f(program, "u_size", {static_cast<float>(b.target->width),
                static_cast<float>(b.target->height)});
    set_f(program, "u_origin", {static_cast<float>(b.ox), static_cast<float>(b.oy)});
    set_f(program, "u_quad", {static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(w), static_cast<float>(h)});
    set_f(program, "u_alpha", {alpha});
    apply_clip(program);
    const int wanted = blend == ER_BLEND_ADD ? ER_BLEND_ADD : 0;
    if (wanted != blend_set) glBlendFunc(GL_ONE, wanted == ER_BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    blend_set = wanted;
  };
  // Radii as the software rasterizer clamps them: no corner wider than half the shorter side.
  auto clamp_radii = [](const int in[4], int w, int h, float out[4]) {
    const float most = static_cast<float>(std::min(w, h)) / 2.0f;
    for (int i = 0; i < 4; i++) out[i] = std::clamp(static_cast<float>(in[i]), 0.0f, std::max(most, 0.0f));
  };
  auto shape = [&](const ERCanvasOp& op, int mode, std::uint32_t fill, const int edges[4],
                   const std::uint32_t colors[4]) {
    if (op.w <= 0 || op.h <= 0) return;
    begin_draw(shape_, op.x, op.y, op.w, op.h);
    float radii[4];
    clamp_radii(op.radii, op.w, op.h, radii);
    const auto f = premultiplied(fill);
    set_f(shape_, "u_rect", {static_cast<float>(op.x), static_cast<float>(op.y),
                static_cast<float>(op.w), static_cast<float>(op.h)});
    set_v4(shape_, "u_radii", radii, 1);
    set_i(shape_, "u_mode", mode);
    set_v4(shape_, "u_fill", f.data(), 1);
    if (mode != 0) {
      const float e[4] = {static_cast<float>(edges[0]), static_cast<float>(edges[1]), static_cast<float>(edges[2]),
                          static_cast<float>(edges[3])};
      const float inner[4] = {std::max(radii[0] - std::max(e[0], e[1]), 0.0f),
                              std::max(radii[1] - std::max(e[2], e[1]), 0.0f),
                              std::max(radii[2] - std::max(e[2], e[3]), 0.0f),
                              std::max(radii[3] - std::max(e[0], e[3]), 0.0f)};
      float c[16];
      for (int i = 0; i < 4; i++) {
        const auto p = premultiplied(colors[i]);
        std::copy(p.begin(), p.end(), c + i * 4);
      }
      set_v4(shape_, "u_edges", e, 1);
      set_v4(shape_, "u_inner_radii", inner, 1);
      set_v4(shape_, "u_edge_color", c, 4);
    }
    stats_.draws++;
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  };
  auto texture_draw = [&](unsigned int texture, int kind, int x, int y, int w, int h, float shift_x, float shift_y,
                          int src_w, int src_h, const float* m, float op_alpha) {
    if (w <= 0 || h <= 0) return;
    begin_draw(image_, x, y, w, h);
    set_f(image_, "u_alpha", {alpha * op_alpha});
    bind_texture(texture);
    set_i(image_, "u_tex", 0);
    set_i(image_, "u_kind", kind);
    set_f(image_, "u_shift", {shift_x, shift_y});
    set_f(image_, "u_src_size", {static_cast<float>(src_w), static_cast<float>(src_h)});
    if (m) set_v1(image_, "u_m", m, 6);
    stats_.draws++;
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  };

  bind();
  for (const Op& recorded : *ops_) {
    const ERCanvasOp& op = recorded.op;
    switch (op.kind) {
      case ER_CANVAS_FILL: {
        ERCanvasOp square = op;
        std::fill(square.radii, square.radii + 4, 0);
        shape(square, 0, op.argb, nullptr, nullptr);
        break;
      }
      case ER_CANVAS_RRECT:
        shape(op, 0, op.argb, nullptr, nullptr);
        break;
      case ER_CANVAS_RRECT_BORDERED: {
        if (op.width <= 0 || (op.argb2 >> 24) == 0) {
          shape(op, 0, op.argb, nullptr, nullptr);
          break;
        }
        const int edges[4] = {op.width, op.width, op.width, op.width};
        const std::uint32_t colors[4] = {op.argb2, op.argb2, op.argb2, op.argb2};
        shape(op, 1, op.argb, edges, colors);
        break;
      }
      case ER_CANVAS_RRECT_RING: {
        const int edges[4] = {op.width, op.width, op.width, op.width};
        const std::uint32_t colors[4] = {op.argb, op.argb, op.argb, op.argb};
        shape(op, 2, 0, edges, colors);
        break;
      }
      case ER_CANVAS_RRECT_RING_EDGES:
        shape(op, 2, 0, op.edges, op.edge_colors);
        break;
      case ER_CANVAS_CLEAR: {
        // Transparent, through the clip rect only, as the software clear.
        const Bound& b = targets.back();
        int x = op.x, y = op.y, w = op.w, h = op.h;
        if (!clips.empty()) {
          const Clip& c = clips.back();
          const int x1 = std::min(x + w, c.x + c.w), y1 = std::min(y + h, c.y + c.h);
          x = std::max(x, c.x);
          y = std::max(y, c.y);
          w = x1 - x;
          h = y1 - y;
        }
        if (w <= 0 || h <= 0) break;
        scissor_to(x - b.ox, y - b.oy, w, h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        break;
      }
      case ER_CANVAS_CLIP_RECT: {
        Clip c{op.x, op.y, op.w, op.h, false, {}, 0, 0, 0, 0, false};
        if (!clips.empty()) {
          const Clip& top = clips.back();
          const int x1 = std::min(op.x + op.w, top.x + top.w), y1 = std::min(op.y + op.h, top.y + top.h);
          c.x = std::max(op.x, top.x);
          c.y = std::max(op.y, top.y);
          c.w = std::max(x1 - c.x, 0);
          c.h = std::max(y1 - c.y, 0);
        }
        clips.push_back(c);
        break;
      }
      case ER_CANVAS_CLIP_RRECT: {
        Clip c{op.x, op.y, op.w, op.h, true, {}, op.x, op.y, op.w, op.h, false};
        if (!clips.empty()) {
          const Clip& top = clips.back();
          const int x1 = std::min(op.x + op.w, top.x + top.w), y1 = std::min(op.y + op.h, top.y + top.h);
          c.x = std::max(op.x, top.x);
          c.y = std::max(op.y, top.y);
          c.w = std::max(x1 - c.x, 0);
          c.h = std::max(y1 - c.y, 0);
        }
        clamp_radii(op.radii, op.w, op.h, c.radii);
        c.rounded = c.radii[0] > 0 || c.radii[1] > 0 || c.radii[2] > 0 || c.radii[3] > 0;
        clips.push_back(c);
        break;
      }
      case ER_CANVAS_CLIP_RESET:
        clips.push_back(Clip{-(1 << 20), -(1 << 20), 1 << 21, 1 << 21, false, {}, 0, 0, 0, 0, true});
        break;
      case ER_CANVAS_CLIP_POP:
        if (!clips.empty()) clips.pop_back();
        break;
      case ER_CANVAS_ALPHA:
        alpha = static_cast<float>(op.alpha) / 255.0f;
        break;
      case ER_CANVAS_BLEND:
        blend = op.mode;
        break;
      case ER_CANVAS_GROUP_BEGIN: {
        Group& g = group(op.buffer, op.w, op.h);
        g.ox = op.x;
        g.oy = op.y;
        bound = 0; // a new or resized target binds its texture
        targets.push_back({&g.target, op.x, op.y});
        bind();
        scissor_off();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        break;
      }
      case ER_CANVAS_GROUP_END:
        if (targets.size() > 1) targets.pop_back();
        bind();
        break;
      case ER_CANVAS_GROUP_DRAW: {
        for (const auto& [key, g] : groups_) {
          if (key != op.buffer) continue;
          // Screen pixel (x, y) reads the group's pixel pos[]: the shift between them.
          texture_draw(g.target.texture, 0, op.x, op.y, op.w, op.h, static_cast<float>(op.x - op.pos[0]),
                       static_cast<float>(op.y - op.pos[1]), g.target.width, g.target.height, nullptr,
                       static_cast<float>(op.alpha) / 255.0f);
          break;
        }
        break;
      }
      case ER_CANVAS_GROUP_XFORM: {
        for (const auto& [key, g] : groups_) {
          if (key != op.buffer) continue;
          // m holds ia ib ic id itx ity; the source's pixel (0, 0) is the capture's (pos[0], pos[1]).
          texture_draw(g.target.texture, 1, op.x, op.y, op.w, op.h, static_cast<float>(op.pos[0]),
                       static_cast<float>(op.pos[1]), op.slot, op.width, op.m, 1.0f);
          break;
        }
        break;
      }
      case ER_CANVAS_GRADIENT: {
        if (op.stop_count < 2 || op.w <= 0 || op.h <= 0) break;
        if (op.mode != ER_GRADIENT_LINEAR && op.mode != ER_GRADIENT_RADIAL) break;
        begin_draw(shape_, op.x, op.y, op.w, op.h);
        float radii[4];
        clamp_radii(op.radii, op.w, op.h, radii);
        set_f(shape_, "u_rect", {static_cast<float>(op.x), static_cast<float>(op.y),
                    static_cast<float>(op.w), static_cast<float>(op.h)});
        set_v4(shape_, "u_radii", radii, 1);
        set_i(shape_, "u_mode", op.mode == ER_GRADIENT_LINEAR ? 3 : 4);
        const int count = std::min(op.stop_count, 4);
        float colors[16] = {}, positions[4] = {};
        for (int i = 0; i < count; i++) {
          const std::uint32_t c = op.stops[i].color;
          colors[i * 4] = static_cast<float>(c >> 24);
          colors[i * 4 + 1] = static_cast<float>((c >> 16) & 0xFF);
          colors[i * 4 + 2] = static_cast<float>((c >> 8) & 0xFF);
          colors[i * 4 + 3] = static_cast<float>(c & 0xFF);
          positions[i] = op.stops[i].position;
        }
        set_v4(shape_, "u_stop_color", colors, 4);
        set_v1(shape_, "u_stop_pos", positions, 4);
        set_i(shape_, "u_stop_count", count);
        // The table's size and the index scale, as the software ramp works them out.
        float extent, to_idx, min_p = 0.0f;
        int step = 0;
        if (op.mode == ER_GRADIENT_LINEAR) {
          const float ddx = op.f[0], ddy = op.f[1], wf = static_cast<float>(op.w - 1), hf = static_cast<float>(op.h - 1);
          const float p10 = wf * ddx, p01 = hf * ddy, p11 = wf * ddx + hf * ddy;
          min_p = std::min(std::min(0.0f, p10), std::min(p01, p11));
          extent = std::max(std::max(0.0f, p10), std::max(p01, p11)) - min_p;
        } else {
          const float cx = static_cast<float>(op.w - 1) * 0.5f, cy = static_cast<float>(op.h - 1) * 0.5f;
          extent = std::sqrt(cx * cx + cy * cy);
        }
        const int n = std::clamp(static_cast<int>(extent) + 1, 2, 1024);
        to_idx = extent > 1e-6f ? static_cast<float>(n - 1) / extent : 0.0f;
        if (op.mode == ER_GRADIENT_LINEAR) step = static_cast<int>(std::lrint(op.f[0] * to_idx * 65536.0f));
        set_f(shape_, "u_dir", {op.f[0], op.f[1]});
        set_f(shape_, "u_min_p", {min_p});
        set_f(shape_, "u_to_idx", {to_idx});
        set_i(shape_, "u_step", step);
        set_i(shape_, "u_lut_n", n);
        stats_.draws++;
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        break;
      }
      case ER_CANVAS_IMAGE_REGION: {
        const std::uint64_t uploads = stats_.uploads;
        const Image& img = image(op.buffer, op.name, op.slot, op.width, op.mode);
        if (stats_.uploads != uploads) bound = img.texture;
        begin_draw(image_, op.x, op.y, op.w, op.h);
        bind_texture(img.texture);
        set_i(image_, "u_tex", 0);
        set_i(image_, "u_kind", 3);
        set_f(image_, "u_dst", {static_cast<float>(op.x), static_cast<float>(op.y),
                    static_cast<float>(op.w), static_cast<float>(op.h)});
        set_f(image_, "u_src", {static_cast<float>(op.src[0]), static_cast<float>(op.src[1]),
                    static_cast<float>(op.src[2]), static_cast<float>(op.src[3])});
        set_f(image_, "u_tint", {static_cast<float>((op.argb >> 16) & 0xFF),
                    static_cast<float>((op.argb >> 8) & 0xFF), static_cast<float>(op.argb & 0xFF), op.f[0]});
        float radii[4];
        clamp_radii(op.radii, op.area[2], op.area[3], radii);
        set_f(image_, "u_mask", {static_cast<float>(op.area[0]),
                    static_cast<float>(op.area[1]), static_cast<float>(op.area[2]), static_cast<float>(op.area[3])});
        set_v4(image_, "u_mask_radii", radii, 1);
        stats_.draws++;
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        break;
      }
      case ER_CANVAS_GLYPH: {
        const std::uint64_t uploads = stats_.uploads;
        const Glyph& g = glyph(op.buffer, op.w, op.h, op.mode);
        if (stats_.uploads != uploads) bound = g.texture;
        // The glyph's box (wider by the italic shear), cut to the text's clip.
        const int reach = op.f[0] != 0.0f ? static_cast<int>(static_cast<float>(op.h - 1) * 0.2f) + 2 : 0;
        int x = std::max(op.x, op.area[0]), y = std::max(op.y, op.area[1]);
        const int x1 = std::min(op.x + op.w + reach, op.area[0] + op.area[2]);
        const int y1 = std::min(op.y + op.h, op.area[1] + op.area[3]);
        if (x1 <= x || y1 <= y) break;
        begin_draw(image_, x, y, x1 - x, y1 - y);
        bind_texture(g.texture);
        set_i(image_, "u_tex", 0);
        set_i(image_, "u_kind", 2);
        set_f(image_, "u_shift", {static_cast<float>(op.x), static_cast<float>(op.y)});
        set_f(image_, "u_src_size", {static_cast<float>(g.width), static_cast<float>(g.height)});
        set_i(image_, "u_italic", op.f[0] == 0.0f ? 0 : op.mode == 1 ? 1 : 2);
        const auto c = premultiplied(op.argb);
        set_v4(image_, "u_color", c.data(), 1);
        stats_.draws++;
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        break;
      }
      case ER_CANVAS_GROUP_KEEP: {
        const Group* from = nullptr;
        for (const auto& [key, g] : groups_)
          if (key == op.source) from = &g;
        if (!from || op.w <= 0 || op.h <= 0) break;
        const Target source = from->target;
        Group& kept = group(op.buffer, source.width, source.height);
        bound = 0;
        scissor_off();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, kept.target.fbo);
        glBlitFramebuffer(0, 0, op.w, op.h, 0, 0, op.w, op.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        bind();
        break;
      }
      case ER_CANVAS_BORDER_SWEEP: {
        // As border_sweep_render_sw sets up the ring and its centre line.
        if (op.w <= 0 || op.h <= 0 || op.width <= 0 || (op.argb >> 24) == 0 || op.f[1] <= 0.0f) break;
        const int r = std::clamp(op.radii[0], 0, std::min(op.w, op.h) / 2);
        const float bw = static_cast<float>(op.width), mid = bw * 0.5f;
        const float cr = std::max(static_cast<float>(r) - mid, 0.0f);
        begin_draw(shape_, op.x, op.y, op.w, op.h);
        const auto f = premultiplied(op.argb);
        set_f(shape_, "u_rect", {static_cast<float>(op.x), static_cast<float>(op.y),
                    static_cast<float>(op.w), static_cast<float>(op.h)});
        set_i(shape_, "u_mode", 5);
        set_v4(shape_, "u_fill", f.data(), 1);
        set_f(shape_, "u_ring", {static_cast<float>(r), bw, std::max(static_cast<float>(r) - bw, 0.0f)});
        set_f(shape_, "u_line", {static_cast<float>(op.w) * 0.5f - mid - cr,
                    static_cast<float>(op.h) * 0.5f - mid - cr, cr});
        set_f(shape_, "u_sweep", {op.f[0], op.f[1]});
        stats_.draws++;
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        break;
      }
      case ER_CANVAS_PIXELS: {
        if (op.w <= 0 || op.h <= 0) break;
        if (!pixels_) {
          glGenTextures(1, &pixels_);
          bind_texture(pixels_);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        bind_texture(pixels_);
        stats_.pixel_ops++;
        stats_.pixel_bytes += static_cast<std::uint64_t>(op.w) * op.h * 4;
        gl::tex_image_argb(op.w, op.h, recorded.pixels.data());
        texture_draw(pixels_, 0, op.x, op.y, op.w, op.h, static_cast<float>(op.x), static_cast<float>(op.y), op.w,
                     op.h, nullptr, 1.0f);
        break;
      }
      case ER_CANVAS_MOVE: {
        // Through the copy target: a framebuffer cannot blit onto itself.
        scissor_off();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target_.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_.fbo);
        glBlitFramebuffer(op.x, op.y, op.x + op.w, op.y + op.h, op.x, op.y, op.x + op.w, op.y + op.h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, copy_.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_.fbo);
        glBlitFramebuffer(op.x, op.y, op.x + op.w, op.y + op.h, op.pos[0], op.pos[1], op.pos[0] + op.w,
                          op.pos[1] + op.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        bind();
        break;
      }
      default:
        break;
    }
  }
}

namespace {

struct Diff {
  std::size_t over[3] = {};
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
  std::vector<std::uint32_t> picture;  // red by how far apart, over the second picture's green
};

// How far apart two pictures are: pixels whose channels differ by more than 2, 16 and 64 levels, and the
// box around the last.
Diff difference(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, int width, int height,
                bool picture) {
  Diff d;
  d.x0 = width;
  d.y0 = height;
  if (picture) d.picture.resize(a.size());
  for (std::size_t i = 0; i < a.size(); i++) {
    int most = 0;
    for (int shift = 0; shift < 32; shift += 8)
      most = std::max(most, std::abs(static_cast<int>((a[i] >> shift) & 0xFF) - static_cast<int>((b[i] >> shift) & 0xFF)));
    if (most > 2) d.over[0]++;
    if (most > 16) d.over[1]++;
    if (most > 64) {
      d.over[2]++;
      const int x = static_cast<int>(i % width), y = static_cast<int>(i / width);
      d.x0 = std::min(d.x0, x);
      d.y0 = std::min(d.y0, y);
      d.x1 = std::max(d.x1, x);
      d.y1 = std::max(d.y1, y);
    }
    if (picture) {
      const std::uint32_t level = static_cast<std::uint32_t>(std::min(255, most * 4));
      d.picture[i] = 0xFF000000u | (level << 16) | ((b[i] & 0xFF00u) >> 2 & 0xFF00u);
    }
  }
  return d;
}

// A target's pixels as ARGB8888, rows top first; `flip` for one drawn like the screen, bottom first.
void read_target(unsigned int fbo, int width, int height, bool flip, std::vector<std::uint32_t>& argb) {
  argb.resize(static_cast<std::size_t>(width) * height);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  gl::read_argb(0, 0, width, height, argb.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (flip)
    for (int y = 0; y < height / 2; y++)
      std::swap_ranges(argb.begin() + static_cast<std::ptrdiff_t>(y) * width,
                       argb.begin() + static_cast<std::ptrdiff_t>(y + 1) * width,
                       argb.begin() + static_cast<std::ptrdiff_t>(height - 1 - y) * width);
}

}  // namespace

bool GpuRenderer::screen_target() {
  if (screen_.fbo) return true;
  if (!make_target(width_, height_, screen_.texture, screen_.fbo)) return false;
  screen_.width = width_;
  screen_.height = height_;
  return true;
}

std::string GpuRenderer::compare(const std::uint32_t* cpu, GlPresenter& presenter, const std::string& path) {
  if (!active() || !cpu) return "gpu mirror: nothing to compare";
  if (!screen_target()) return "gpu mirror: no screen target";
  // The engine's two pictures, then each as the screen shows it: over the presenter's layers.
  std::vector<std::uint32_t> gpu, cpu_screen, gpu_screen;
  read_target(target_.fbo, width_, height_, false, gpu);
  const std::vector<std::uint32_t> cpu_frame(cpu, cpu + static_cast<std::size_t>(width_) * height_);
  const Diff frame = difference(cpu_frame, gpu, width_, height_, false);
  presenter.composite_to(screen_.fbo, 0);
  read_target(screen_.fbo, width_, height_, true, cpu_screen);
  presenter.composite_to(screen_.fbo, target_.texture);
  read_target(screen_.fbo, width_, height_, true, gpu_screen);
  const Diff screen = difference(cpu_screen, gpu_screen, width_, height_, true);
  write_bmp(path + "-cpu.bmp", cpu_screen, width_, height_);
  write_bmp(path + "-gpu.bmp", gpu_screen, width_, height_);
  write_bmp(path + "-diff.bmp", screen.picture, width_, height_);

  char line[640];
  const double total = static_cast<double>(cpu_frame.size()) / 100.0;
  int n = std::snprintf(line, sizeof line, "gpu mirror %s: screen differ >2 %.3f%%, >16 %.3f%%, >64 %.3f%%",
                        path.c_str(), screen.over[0] / total, screen.over[1] / total, screen.over[2] / total);
  if (screen.x1 >= screen.x0)
    n += std::snprintf(line + n, sizeof line - n, " (box %d,%d-%d,%d)", screen.x0, screen.y0, screen.x1, screen.y1);
  n += std::snprintf(line + n, sizeof line - n, "; framebuffer >2 %.3f%%, >64 %.3f%%; missing:",
                     frame.over[0] / total, frame.over[2] / total);
  for (int kind = 0; kind < ER_CANVAS_OP_COUNT && n > 0 && n < static_cast<int>(sizeof line); kind++)
    if (missing_[kind]) n += std::snprintf(line + n, sizeof line - n, " %s=%llu", kKindNames[kind],
                                           static_cast<unsigned long long>(missing_[kind]));
  return line;
}

bool GpuRenderer::save_screen(GlPresenter& presenter, const std::string& path) {
  if (!active()) return false;
  if (!screen_target()) return false;
  std::vector<std::uint32_t> screen;
  presenter.composite_to(screen_.fbo, target_.texture);
  read_target(screen_.fbo, width_, height_, true, screen);
  return write_bmp(path, screen, width_, height_);
}
