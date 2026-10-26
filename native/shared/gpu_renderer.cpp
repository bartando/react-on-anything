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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

struct GpuRenderer::Op {
  ERCanvasOp op;
  std::vector<std::uint32_t> pixels;  // ER_CANVAS_PIXELS: a copy, unless its texture is kept already
  std::uint64_t hash = 0;             // ER_CANVAS_PIXELS: the picture's fingerprint, its key in pixels_
};

namespace {
// Where the renderer's lines go: stdout, unless the host gives its own log (GpuRenderer::set_log).
void print_line(const char* line) { std::printf("%s\n", line); }
void (*log_sink)(const char*) = print_line;

__attribute__((format(printf, 1, 2))) void log_line(const char* format, ...) {
  char line[512];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(line, sizeof line, format, arguments);
  va_end(arguments);
  log_sink(line);
}


// Kept pictures not drawn for this many replays go, and the oldest go past the byte budget.
constexpr std::uint64_t kPixelsKeepReplays = 120;
constexpr std::size_t kPixelsBudgetBytes = 24u << 20;

// A 64-bit fingerprint of a picture's w x h pixels in rows `stride` apart.
std::uint64_t picture_hash(const std::uint32_t* px, int stride, int w, int h) {
  std::uint64_t hash = 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(w) << 32 | static_cast<std::uint32_t>(h));
  const auto mix = [&hash](std::uint64_t v) {
    hash = (hash ^ v) * 0xff51afd7ed558ccdull;
    hash ^= hash >> 32;
  };
  for (int y = 0; y < h; y++) {
    const std::uint32_t* row = px + static_cast<std::size_t>(y) * stride;
    int x = 0;
    for (std::uint64_t v; x + 2 <= w; x += 2) {
      std::memcpy(&v, row + x, sizeof v);
      mix(v);
    }
    if (x < w) mix(row[x]);
  }
  return hash;
}

std::mutex record_lock;

// A draw's parameters are a record of kRecord vec4s in a buffer texture, so draws in a row go to the GPU as
// instances of one call: the PS5's GL charges about 80us a call, whatever it draws. Record 0 is the quad
// to cover, already cut to the clip rect; 1 holds the kind, the alpha, the rounded clips (an offset into
// the batch's clip sets, or -1) and the texture unit; the rest are the kind's own (see draw_ops).
constexpr int kRecord = 11;
// Why a batch ended, for the stats.
enum Break { kBreakBlend, kBreakTextures, kBreakFull, kBreakGroup, kBreakClear, kBreakCopy, kBreakEnd, kBreakKinds };
constexpr const char* kBreakNames[kBreakKinds] = {"blend", "textures", "full", "group", "clear", "copy", "end"};
// Texture units: a batch's textures on 0 to kUnits - 1, the records on the next, an image being copied in
// on the one after. GL promises 16.
constexpr int kUnits = 14;
constexpr int kRecordsUnit = kUnits;
constexpr int kStagingUnit = kUnits + 1;
constexpr int kStagingTexels = 1 << 20;  // 4MB, at most what a buffer texture holds on the PS5

constexpr const char* kVertex = R"(
uniform vec2 u_size;
uniform vec2 u_origin;
uniform samplerBuffer u_records;
uniform int u_first;
flat out int v_record;
void main() {
  v_record = u_first + gl_InstanceID * kRecord;
  vec4 quad = texelFetch(u_records, v_record);
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 local = quad.xy + corner * quad.zw - u_origin;
  gl_Position = vec4(local / u_size * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Rounded rects: mode 0 fills, 1 fills inside a border, 2 strokes the border only; 3 and 4 are linear and
// radial gradients; 5 is the animated focus ring (border_sweep.c), a ring inside the rect, bright at its
// head and fading behind it. Images: 10 a group's texture shifted, 11 transformed, 12 a glyph, 13 an
// image region scaled.
constexpr const char* kFragment = R"(
uniform vec2 u_origin;
uniform samplerBuffer u_records;
uniform int u_clip_first;
uniform sampler2D u_tex0;
uniform sampler2D u_tex1;
uniform sampler2D u_tex2;
uniform sampler2D u_tex3;
uniform sampler2D u_tex4;
uniform sampler2D u_tex5;
uniform sampler2D u_tex6;
uniform sampler2D u_tex7;
uniform sampler2D u_tex8;
uniform sampler2D u_tex9;
uniform sampler2D u_tex10;
uniform sampler2D u_tex11;
uniform sampler2D u_tex12;
uniform sampler2D u_tex13;
flat in int v_record;
out vec4 frag;
const float kPi = 3.14159265;
vec4 rec(int i) { return texelFetch(u_records, v_record + i); }

// The pixel's coverage by a rounded rect (x, y, w, h; radii tl, tr, br, bl), from its signed distance at
// the pixel centre: the software rasterizer's analytic fringe, within a level or two.
float rbox(vec2 c, vec4 rect, vec4 radii) {
  vec2 half_size = rect.zw * 0.5;
  vec2 p = c - (rect.xy + half_size);
  float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
  vec2 q = abs(p) - half_size + r;
  float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
  return clamp(0.5 - d, 0.0, 1.0);
}
// Rounded clips, a count then rect and radii pairs: rect clips cut the quad.
float clipped(vec2 c, int at) {
  if (at < 0) return 1.0;
  int base = u_clip_first + at;
  int count = int(texelFetch(u_records, base).x);
  float cov = 1.0;
  for (int i = 0; i < count; i++)
    cov *= rbox(c, texelFetch(u_records, base + 1 + i * 2), texelFetch(u_records, base + 2 + i * 2));
  return cov;
}

int g_unit;
vec4 fetch(ivec2 p) {
  switch (g_unit) {
    case 0: return texelFetch(u_tex0, p, 0);
    case 1: return texelFetch(u_tex1, p, 0);
    case 2: return texelFetch(u_tex2, p, 0);
    case 3: return texelFetch(u_tex3, p, 0);
    case 4: return texelFetch(u_tex4, p, 0);
    case 5: return texelFetch(u_tex5, p, 0);
    case 6: return texelFetch(u_tex6, p, 0);
    case 7: return texelFetch(u_tex7, p, 0);
    case 8: return texelFetch(u_tex8, p, 0);
    case 9: return texelFetch(u_tex9, p, 0);
    case 10: return texelFetch(u_tex10, p, 0);
    case 11: return texelFetch(u_tex11, p, 0);
    case 12: return texelFetch(u_tex12, p, 0);
    default: return texelFetch(u_tex13, p, 0);
  }
}

// Shapes. Record 2 is the rect; then for modes 0-2: radii, fill, edges, inner radii and the four edge
// colours; gradients: radii, four stop colours (straight a, r, g, b in 0..255), their positions, (count,
// direction, smallest projection), (index scale, index step as int bits, table size); the sweep: fill,
// ring (outer radius, width, inner radius), centre line (straight half-lengths a, b and corner radius),
// and the head's place on the perimeter and the tail's length, as fractions of it.
vec4 g_rect, g_radii, g_fill, g_edges, g_inner_radii;
vec4 g_edge_color[4];
vec4 g_stop0, g_stop1, g_stop2, g_stop3, g_stop_pos;
int g_mode, g_stop_count, g_step, g_lut_n;
vec2 g_dir;
float g_min_p, g_to_idx;
vec3 g_ring, g_line;
vec2 g_sweep;
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
  float d = g_sweep.x - t;
  d -= floor(d);
  if (d > 1.0 - lead) return (d - (1.0 - lead)) / lead;
  if (d >= g_sweep.y) return 0.0;
  float f = 1.0 - d / g_sweep.y;
  return f * f;
}
vec4 sweep(vec2 c) {
  vec2 half_size = g_rect.zw * 0.5;
  vec2 p = c - (g_rect.xy + half_size);
  float cov = clamp(0.5 - sd_rrect(p, half_size, g_ring.x), 0.0, 1.0);
  cov *= clamp(0.5 + sd_rrect(p, half_size - g_ring.y, g_ring.z), 0.0, 1.0);
  return g_fill * (cov * sweep_alpha(perimeter_at(p, g_line.x, g_line.y, g_line.z)));
}
// Gradients as the software ramp: a table of n premultiplied 8.8 colours, read at a fixed-point index and
// dithered with a 4x4 Bayer threshold per screen pixel. No array is indexed at run time: on the Shield's
// Tegra one made every draw slower, gradient or not, and Home took 26ms of GPU a frame, not 11.
vec4 premul_f(vec4 s) { return vec4(s.x, s.y * s.x / 255.0, s.z * s.x / 255.0, s.w * s.x / 255.0); }
vec4 stop_color(int i) { return i == 0 ? g_stop0 : i == 1 ? g_stop1 : i == 2 ? g_stop2 : g_stop3; }
float stop_pos(int i) { return i == 0 ? g_stop_pos.x : i == 1 ? g_stop_pos.y : i == 2 ? g_stop_pos.z : g_stop_pos.w; }
// The 4x4 Bayer matrix (0, 8, 2, 10 / 12, 4, 14, 6 / 3, 11, 1, 9 / 15, 7, 13, 5): x ^ y and y, bits
// interleaved and reversed.
int bayer(ivec2 p) {
  int x = p.x & 3, y = p.y & 3, d = x ^ y;
  return (d & 1) << 3 | (y & 1) << 2 | (d & 2) | (y >> 1);
}
vec4 lut(int idx) {
  float t = float(idx) / float(g_lut_n - 1);
  int last = g_stop_count - 1;
  vec4 c;
  if (t <= g_stop_pos.x) c = premul_f(g_stop0);
  else if (t >= stop_pos(last)) c = premul_f(stop_color(last));
  else {
    int seg = 0;
    if (last > 0 && t > g_stop_pos.y) seg = 1;
    if (last > 1 && t > g_stop_pos.z) seg = 2;
    if (last > 2 && t > g_stop_pos.w) seg = 3;
    vec4 c0 = premul_f(stop_color(seg)), c1 = premul_f(stop_color(seg + 1));
    float p0 = stop_pos(seg), span = stop_pos(seg + 1) - p0;
    float lt = span > 0.0 ? (t - p0) / span : 1.0;
    c = c0 + (c1 - c0) * lt;
  }
  vec4 v = floor(c * 256.0 + 0.5);
  return vec4(v.x, min(v.yzw, vec3(v.x)));
}
vec4 gradient(vec2 c) {
  ivec2 screen = ivec2(floor(c));
  ivec2 local = screen - ivec2(g_rect.xy);
  int idx;
  if (g_mode == 3) {
    int f = int(round((float(local.y) * g_dir.y - g_min_p) * g_to_idx * 65536.0)) + local.x * g_step;
    idx = clamp((f + 32768) >> 16, 0, g_lut_n - 1);
  } else {
    vec2 centre = (g_rect.zw - 1.0) * 0.5;
    vec2 d = vec2(local) - centre;
    idx = min(int(sqrt(d.x * d.x + d.y * d.y) * g_to_idx + 0.5), g_lut_n - 1);
  }
  float threshold = float(bayer(screen) * 16 + 8);
  vec4 argb = floor((lut(idx) + threshold) / 256.0);
  return vec4(argb.yzw, argb.x) / 255.0;
}
vec4 edge_color(vec2 c, vec4 inner) {
  float dl = (inner.x - c.x) / max(g_edges.x, 0.001);
  float dt = (inner.y - c.y) / max(g_edges.y, 0.001);
  float dr = (c.x - inner.x - inner.z) / max(g_edges.z, 0.001);
  float db = (c.y - inner.y - inner.w) / max(g_edges.w, 0.001);
  float best = dl; vec4 color = g_edge_color[0];
  if (dt > best) { best = dt; color = g_edge_color[1]; }
  if (dr > best) { best = dr; color = g_edge_color[2]; }
  if (db > best) { best = db; color = g_edge_color[3]; }
  return color;
}
vec4 shape(vec2 c, int mode) {
  g_mode = mode;
  g_rect = rec(2);
  if (mode == 5) {
    g_fill = rec(3);
    g_ring = rec(4).xyz;
    g_line = rec(5).xyz;
    g_sweep = rec(6).xy;
    return sweep(c);
  }
  g_radii = rec(3);
  float outer = rbox(c, g_rect, g_radii);
  if (mode == 0) return rec(4) * outer;
  if (mode >= 3) {
    g_stop0 = rec(4);
    g_stop1 = rec(5);
    g_stop2 = rec(6);
    g_stop3 = rec(7);
    g_stop_pos = rec(8);
    vec4 a = rec(9), b = rec(10);
    g_stop_count = int(a.x);
    g_dir = a.yz;
    g_min_p = a.w;
    g_to_idx = b.x;
    g_step = floatBitsToInt(b.y);
    g_lut_n = int(b.z);
    return gradient(c) * outer;
  }
  g_fill = rec(4);
  g_edges = rec(5);
  g_inner_radii = rec(6);
  for (int i = 0; i < 4; i++) g_edge_color[i] = rec(7 + i);
  vec4 inner = vec4(g_rect.xy + g_edges.xy, g_rect.zw - g_edges.xy - g_edges.zw);
  float in_cov = (inner.z > 0.0 && inner.w > 0.0) ? rbox(c, inner, g_inner_radii) : 0.0;
  vec4 fill = mode == 2 ? vec4(0.0) : g_fill;
  return edge_color(c, inner) * max(outer - in_cov, 0.0) + fill * min(in_cov, outer);
}

// Images. Record 2 is (shift, source size); then a transform's matrix (ia ib ic id, itx ity); a glyph's
// colour, and its italic mode and place in its atlas page; an image region's destination, source part,
// tint (rgb 0..255, a = 1 when tinted), and the box a rounded image is cut to with its radii.
vec2 g_shift, g_src_size;
ivec2 g_atlas;
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
  if (d < 0 || float(d) >= g_src_size.x) return 0.0;
  return fetch(ivec2(d, row) + g_atlas).r;
}
vec4 tap(ivec2 t) {
  if (t.x < 0 || t.y < 0 || float(t.x) >= g_src_size.x || float(t.y) >= g_src_size.y) return vec4(0.0);
  return fetch(t);
}
vec4 image(vec2 c, int kind) {
  vec4 at = rec(2);
  g_shift = at.xy;
  g_src_size = at.zw;
  vec2 s = floor(c);
  if (kind == 0) return tap(ivec2(s - g_shift));
  if (kind == 3) {
    vec4 dst = rec(3), src = rec(4), tint = rec(5), mask = rec(6);
    ivec2 d = ivec2(s - dst.xy);
    ivec3 cx = axis(src.x, src.z, dst.z, d.x);
    ivec3 cy = axis(src.y, src.w, dst.w, d.y);
    vec4 top = lerp8(fetch(ivec2(cx.x, cy.x)), fetch(ivec2(cx.y, cy.x)), cx.z);
    vec4 bottom = lerp8(fetch(ivec2(cx.x, cy.y)), fetch(ivec2(cx.y, cy.y)), cx.z);
    vec4 color = lerp8(top, bottom, cy.z);
    if (tint.a > 0.0) color.rgb = floor(tint.rgb * floor(color.a * 255.0 + 0.5) / 255.0) / 255.0;
    if (mask.z > 0.0) color *= rbox(c, mask, rec(7));
    return color;
  }
  if (kind == 2) {
    // A glyph's coverage times its colour; italic shears each row right by (height - 1 - row) * 0.2,
    // the fraction split between two columns as the software path does (the 1-bit path drops it).
    vec4 glyph = rec(4);
    int italic = int(glyph.x);
    g_atlas = ivec2(glyph.yz);
    ivec2 local = ivec2(s - g_shift);
    if (local.y < 0 || float(local.y) >= g_src_size.y) discard;
    float shift = italic != 0 ? (g_src_size.y - 1.0 - float(local.y)) * 0.2 : 0.0;
    int whole = int(shift);
    float part = italic == 2 ? shift - float(whole) : 0.0;
    int d = local.x - whole;
    float k = part < 0.005 ? cov(d, local.y) : cov(d, local.y) * (1.0 - part) + cov(d - 1, local.y) * part;
    return rec(3) * k;
  }
  vec4 m = rec(3), t = rec(4);
  float fx = m.x * s.x + m.z * s.y + t.x - g_shift.x;
  float fy = m.y * s.x + m.w * s.y + t.y - g_shift.y;
  if (!(fx >= -1.0 && fx < g_src_size.x && fy >= -1.0 && fy < g_src_size.y)) discard;
  vec2 f0 = floor(vec2(fx, fy));
  vec2 w = floor((vec2(fx, fy) - f0) * 256.0) / 256.0;
  ivec2 i = ivec2(f0);
  vec4 top = mix(tap(i), tap(i + ivec2(1, 0)), w.x);
  vec4 bottom = mix(tap(i + ivec2(0, 1)), tap(i + ivec2(1, 1)), w.x);
  return mix(top, bottom, w.y);
}

void main() {
  vec4 head = rec(1);
  int kind = int(head.x);
  g_unit = int(head.w);
  vec2 c = gl_FragCoord.xy + u_origin;
  vec4 color = kind < 10 ? shape(c, kind) : image(c, kind - 10);
  frag = color * (head.y * clipped(c, int(head.z)));
}
)";

// Copies an image into its texture: rows [u_rows.x, u_rows.y) of a w-wide target, read from a buffer of
// premultiplied ARGB words (B, G, R, A bytes) that starts at row u_rows.x. The PS5's GL lays a texture out
// on the CPU as it uploads, 5.7ms for a poster; into a buffer the same bytes take 0.1ms, and the GPU makes
// the texture of them in a draw.
constexpr const char* kCopyVertex = R"(
uniform vec2 u_rows;
uniform vec2 u_size;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  float y = mix(u_rows.x, u_rows.y, corner.y);
  gl_Position = vec4(corner.x * 2.0 - 1.0, y / u_size.y * 2.0 - 1.0, 0.0, 1.0);
}
)";
constexpr const char* kCopyFragment = R"(
uniform samplerBuffer u_staging;
uniform vec2 u_rows;
uniform int u_width;
out vec4 frag;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  frag = texelFetch(u_staging, (p.y - int(u_rows.x)) * u_width + p.x).bgra;
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
    log_line("[ROA] gpu: shader failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint link_program(const char* vertex_body, const char* fragment_body) {
  const std::string prelude = std::string(GLSL_VERSION) + "const int kRecord = " + std::to_string(kRecord) + ";\n";
  GLuint vertex = compile(GL_VERTEX_SHADER, prelude + vertex_body);
  GLuint fragment = compile(GL_FRAGMENT_SHADER, prelude + fragment_body);
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
    char log[2048];
    glGetProgramInfoLog(program, sizeof log, nullptr, log);
    log_line("[ROA] gpu: program failed: %s", log);
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

void GpuRenderer::set_log(void (*log)(const char* line)) { log_sink = log ? log : print_line; }

bool GpuRenderer::start(int width, int height, Mode mode) {
  mode_ = mode;
  width_ = width;
  height_ = height;
  program_ = link_program(kVertex, kFragment);
  copy_program_ = link_program(kCopyVertex, kCopyFragment);
  if (!program_ || !copy_program_) return false;
  glGenVertexArrays(1, &vao_);
  GLint most = 0;
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &most);
  buffer_texels_ = std::min(most, kBufferTexels);
  if (buffer_texels_ < kBatchDraws * (kRecord + 9)) return false;
  glGenBuffers(1, &records_buffer_);
  glBindBuffer(GL_TEXTURE_BUFFER, records_buffer_);
  glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(buffer_texels_) * 16, nullptr, GL_STREAM_DRAW);
  glGenTextures(1, &records_texture_);
  glBindTexture(GL_TEXTURE_BUFFER, records_texture_);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, records_buffer_);
  glBindTexture(GL_TEXTURE_BUFFER, 0);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
  buffer_at_ = 0;
  staging_texels_ = std::min(static_cast<int>(most), kStagingTexels);
  glGenBuffers(1, &staging_buffer_);
  glGenTextures(1, &staging_texture_);
  glGenFramebuffers(1, &copy_fbo_);
#if !GL_ES_HOST && defined(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT)
  // GL 4.3 binds the records a batch reads, not the whole buffer: the PS5's GL charges a draw ~35us for
  // each MB of buffers bound.
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (major > 4 || (major == 4 && minor >= 3)) glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &range_align_);
#endif
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
  glyphs_.clear();
  if (!glyph_pages_.empty()) glDeleteTextures(static_cast<GLsizei>(glyph_pages_.size()), glyph_pages_.data());
  glyph_pages_.clear();
  page_ = 0;
  shelf_x_ = shelf_y_ = shelf_h_ = 0;
  for (auto& [pixels, img] : images_)
    if (img.texture) glDeleteTextures(1, &img.texture);
  images_.clear();
  if (staging_texture_) glDeleteTextures(1, &staging_texture_);
  if (staging_buffer_) glDeleteBuffers(1, &staging_buffer_);
  if (copy_fbo_) glDeleteFramebuffers(1, &copy_fbo_);
  if (copy_program_) glDeleteProgram(copy_program_);
  staging_texture_ = staging_buffer_ = copy_fbo_ = copy_program_ = 0;
  range_align_ = 0;
  for (auto& [hash, kept] : pixels_) glDeleteTextures(1, &kept.texture);
  pixels_.clear();
  pixels_bytes_ = 0;
  if (vao_) glDeleteVertexArrays(1, &vao_);
  uniforms_.clear();
  uniform_index_.clear();
  if (program_) glDeleteProgram(program_);
  if (records_texture_) glDeleteTextures(1, &records_texture_);
  if (records_buffer_) glDeleteBuffers(1, &records_buffer_);
  vao_ = program_ = records_texture_ = records_buffer_ = 0;
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
  const auto began = std::chrono::steady_clock::now();
  const int answer = self->keep(op);
  self->stats_.record_us += static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
  return answer;
}

int GpuRenderer::keep(const ERCanvasOp* op) {
  GpuRenderer* self = this;
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
    Op kept{*op, {}, picture_hash(px, op->width, op->w, op->h)};
    // A picture whose texture is kept needs no copy; marking it used keeps it through the next replay.
    const auto found = self->pixels_.find(kept.hash);
    if (found != self->pixels_.end() && found->second.width == op->w && found->second.height == op->h) {
      found->second.used = self->replay_serial_ + 1;
    } else {
      kept.pixels.reserve(static_cast<std::size_t>(op->w) * op->h);
      for (int y = 0; y < op->h; y++)
        kept.pixels.insert(kept.pixels.end(), px + static_cast<std::size_t>(y) * op->width,
                           px + static_cast<std::size_t>(y) * op->width + op->w);
    }
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

namespace {
enum UploadKind { kUploadImage, kUploadGlyph, kUploadPixels };
std::uint64_t elapsed_us(std::chrono::steady_clock::time_point since) {
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
}
} // namespace

// Room for a w x h glyph in an atlas page, rows of glyphs filled left to right, a new page when one is
// full: text reads a few textures instead of one per glyph, so it does not end batches. A glyph past a
// page's size gets one of its own.
GpuRenderer::Glyph GpuRenderer::place_glyph(int w, int h) {
  auto page = [](int width, int height) {
    unsigned int texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
  };
  if (w > kGlyphPage || h > kGlyphPage) {
    glyph_pages_.push_back(page(w, h));
    return {glyph_pages_.back(), 0, 0, w, h};
  }
  if (shelf_x_ + w > kGlyphPage) {
    shelf_y_ += shelf_h_;
    shelf_x_ = shelf_h_ = 0;
  }
  if (!page_ || shelf_y_ + h > kGlyphPage) {
    page_ = page(kGlyphPage, kGlyphPage);
    glyph_pages_.push_back(page_);
    shelf_x_ = shelf_y_ = shelf_h_ = 0;
  }
  const Glyph g{page_, shelf_x_, shelf_y_, w, h};
  shelf_x_ += w + 1;
  shelf_h_ = std::max(shelf_h_, h + 1);
  return g;
}

// A baked glyph's coverage as 8 bits a pixel in an atlas page, unpacked once from its 1, 2, 4 or 8 bits.
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
  const auto upload_began = std::chrono::steady_clock::now();
  Glyph g = place_glyph(w, h);
  glBindTexture(GL_TEXTURE_2D, g.texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  stats_.uploads++;
  glTexSubImage2D(GL_TEXTURE_2D, 0, g.x, g.y, w, h, GL_RED, GL_UNSIGNED_BYTE, coverage.data());
  note_upload(kUploadGlyph, elapsed_us(upload_began), static_cast<std::uint64_t>(w) * h);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  return glyphs_.emplace(bitmap, g).first->second;
}

void GpuRenderer::note_upload(int kind, std::uint64_t us, std::uint64_t bytes) {
  stats_.upload_us[kind] += us;
  stats_.max_upload_us[kind] = std::max(stats_.max_upload_us[kind], us);
  stats_.upload_count[kind]++;
  stats_.upload_bytes[kind] += bytes;
}

// Premultiplied ARGB8888 pixels into the bound texture: through the staging buffer, a band of rows at a
// time, drawn into the texture by the GPU. Leaves the framebuffer, program and viewport changed.
void GpuRenderer::copy_in(unsigned int texture, int w, int h, const void* pixels) {
  const int rows = staging_texels_ / w;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  glViewport(0, 0, w, h);
  glDisable(GL_BLEND);
  glUseProgram(copy_program_);
  set_i(copy_program_, "u_staging", kStagingUnit);
  set_i(copy_program_, "u_width", w);
  set_f(copy_program_, "u_size", {static_cast<float>(w), static_cast<float>(h)});
  glActiveTexture(GL_TEXTURE0 + kStagingUnit);
  glBindTexture(GL_TEXTURE_BUFFER, staging_texture_);
  for (int first = 0; first < h; first += rows) {
    const int count = std::min(rows, h - first);
    // Fresh storage each band: the driver keeps the old while a queued copy reads it.
    glBindBuffer(GL_COPY_WRITE_BUFFER, staging_buffer_);
    glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(count) * w * 4,
                 static_cast<const std::uint32_t*>(pixels) + static_cast<std::size_t>(first) * w, GL_STREAM_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, staging_buffer_);
    set_f(copy_program_, "u_rows", {static_cast<float>(first), static_cast<float>(first + count)});
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_BLEND);
}

// An image's pixels as a texture, copied once per buffer: premultiplied ARGB8888 by copy_in(), RGB565
// uploaded as GL takes it. Leaves the framebuffer, program and viewport changed when it copies
// (`stats_.uploads` grows).
const GpuRenderer::Image& GpuRenderer::image(const void* pixels, const char* name, int w, int h, int format) {
  auto found = images_.find(pixels);
  if (found != images_.end()) {
    Image& kept = found->second;
    // A buffer freed and reused for another image: upload again.
    if (kept.width == w && kept.height == h && kept.format == format && kept.name == (name ? name : "")) return kept;
    doomed_.push_back(kept.texture);  // a queued draw may still read it
    images_.erase(found);
  }
  stats_.uploads++;
  const auto upload_began = std::chrono::steady_clock::now();
  Image img{0, w, h, format, name ? name : ""};
  glGenTextures(1, &img.texture);
  glBindTexture(GL_TEXTURE_2D, img.texture);
  if (format == ER_IMG_RGB565 || w <= 0 || w > staging_texels_) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, format == ER_IMG_RGB565 ? 2 : 4);
    if (format == ER_IMG_RGB565) gl::tex_image_rgb565(w, h, pixels);
    else gl::tex_image_argb(w, h, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  } else {
    copy_in(img.texture, w, h, pixels);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  note_upload(kUploadImage, elapsed_us(upload_began),
              static_cast<std::uint64_t>(w) * h * (format == ER_IMG_RGB565 ? 2 : 4));
  return images_.emplace(pixels, img).first->second;
}

// Drops kept pictures that went unused, then the least recently used past the budget. Under record_lock:
// a picture recorded without a copy is marked used for the coming replay, so it is never one of them.
void GpuRenderer::evict_pixels() {
  for (auto it = pixels_.begin(); it != pixels_.end();) {
    if (it->second.used + kPixelsKeepReplays >= replay_serial_) { ++it; continue; }
    glDeleteTextures(1, &it->second.texture);
    pixels_bytes_ -= static_cast<std::size_t>(it->second.width) * it->second.height * 4;
    it = pixels_.erase(it);
  }
  while (pixels_bytes_ > kPixelsBudgetBytes) {
    auto oldest = std::min_element(pixels_.begin(), pixels_.end(),
                                   [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    if (oldest == pixels_.end() || oldest->second.used >= replay_serial_) break;
    glDeleteTextures(1, &oldest->second.texture);
    pixels_bytes_ -= static_cast<std::size_t>(oldest->second.width) * oldest->second.height * 4;
    pixels_.erase(oldest);
  }
}

bool GpuRenderer::replay() {
  if (!active()) return false;
  std::lock_guard<std::mutex> guard(record_lock);
  const auto began = std::chrono::steady_clock::now();
  stats_.ops += ops_->size();
  const bool drew = !ops_->empty();
  if (drew) {
    draw_ops();
    if (!doomed_.empty()) glDeleteTextures(static_cast<GLsizei>(doomed_.size()), doomed_.data());
    doomed_.clear();
    const auto submitted = std::chrono::steady_clock::now();
    stats_.submit_us += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(submitted - began).count());
    if (sync_) {
      glFinish();
      const auto finish = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - submitted).count());
      stats_.finish_us += finish;
      stats_.max_finish_us = std::max(stats_.max_finish_us, finish);
    }
  }
  ops_->clear();
  ++replay_serial_;
  evict_pixels();
  const auto now = std::chrono::steady_clock::now();
  const auto took = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - began).count());
  stats_.replays++;
  stats_.replay_us += took;
  stats_.max_replay_us = std::max(stats_.max_replay_us, took);
  const std::int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  if (!stats_.window_start_us) stats_.window_start_us = now_us;
  if (now_us - stats_.window_start_us >= 2000000) {
    const double n = static_cast<double>(stats_.replays);
    log_line("[ROA] gpu: replays=%llu avg=%.2fms max=%.2fms (submit %.2f%s) record=%.2fms | per replay: ops=%.0f "
             "draws=%.0f in %.1f calls (%.0f hidden), regroups=%.1f pixels=%.1f (%.0fKB) uploads=%.1f",
             static_cast<unsigned long long>(stats_.replays), stats_.replay_us / n / 1000.0,
             stats_.max_replay_us / 1000.0, stats_.submit_us / n / 1000.0,
             sync_ ? (", finish " + std::to_string(stats_.finish_us / n / 1000.0).substr(0, 5) + " max " +
                      std::to_string(stats_.max_finish_us / 1000.0).substr(0, 5)).c_str() : "",
             stats_.record_us / n / 1000.0, stats_.ops / n, stats_.draws / n, stats_.batches / n, stats_.hidden / n, stats_.regroups / n,
             stats_.pixel_ops / n, stats_.pixel_bytes / n / 1024.0, stats_.uploads / n);
    if (stats_.batches) {
      char line[256];
      int at = 0;
      for (int reason = 0; reason < kBreakKinds; reason++)
        if (stats_.breaks[reason])
          at += std::snprintf(line + at, sizeof line - at, " %s %.1f", kBreakNames[reason], stats_.breaks[reason] / n);
      log_line("[ROA] gpu:   calls end per replay at:%s; calls %.2fms, groups %.2fms a replay", line,
               stats_.flush_us / n / 1000.0, stats_.group_us / n / 1000.0);
    }
    constexpr const char* kUploadNames[] = {"images", "glyphs", "pixels"};
    for (int kind = 0; kind < 3; kind++)
      if (stats_.upload_count[kind])
        log_line("[ROA] gpu:   upload %s: %llu, %.0fKB, %.1fms total, %.1fms max", kUploadNames[kind],
                 static_cast<unsigned long long>(stats_.upload_count[kind]),
                 stats_.upload_bytes[kind] / 1024.0,
                    stats_.upload_us[kind] / 1000.0, stats_.max_upload_us[kind] / 1000.0);
    for (int kind = 0; kind < ER_CANVAS_OP_COUNT; kind++)
      if (stats_.captures[kind])
        log_line("[ROA] gpu:   capture %s: %.1f per replay, %.0f kpx each (bounds, before clipping)",
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
  return drew;
}

// The draws a later op paints over entirely: a clear, or an opaque square fill at full alpha, covering
// the clip it was drawn in. Every draw since that clip was pushed lies inside it, so on the Shield,
// where a full-screen draw costs about a millisecond, the views' backgrounds under a host layer's hole
// (seven of them on Home, erased by the hole's clear) no longer cost a frame. Not across what reaches
// outside the clips (a clip reset, a scroll move) or groups, whose pixels later commits may reuse.
std::vector<bool> GpuRenderer::hidden_ops() const {
  const std::vector<Op>& ops = *ops_;
  const auto draws_pixels = [](int kind) {
    switch (kind) {
      case ER_CANVAS_FILL: case ER_CANVAS_RRECT: case ER_CANVAS_RRECT_BORDERED: case ER_CANVAS_RRECT_RING:
      case ER_CANVAS_RRECT_RING_EDGES: case ER_CANVAS_GRADIENT: case ER_CANVAS_BORDER_SWEEP: case ER_CANVAS_CLEAR:
      case ER_CANVAS_IMAGE_REGION: case ER_CANVAS_GLYPH: case ER_CANVAS_PIXELS:
        return true;
      default:
        return false;
    }
  };
  std::vector<bool> hidden(ops.size(), false);
  struct Level { int x0, y0, x1, y1; bool plain; std::size_t pushed; };
  std::vector<Level> levels{{0, 0, width_, height_, true, 0}};  // [0]: the target
  std::size_t barrier = 0;  // nothing before this is hidden: a group, reset or move came after it
  int group_depth = 0, alpha = 255, blend = 0;
  for (std::size_t i = 0; i < ops.size(); i++) {
    const ERCanvasOp& op = ops[i].op;
    if (op.depth) continue;
    switch (op.kind) {
      case ER_CANVAS_CLIP_RECT:
      case ER_CANVAS_CLIP_RRECT: {
        const Level& top = levels.back();
        const bool rounded = op.kind == ER_CANVAS_CLIP_RRECT &&
                             (op.radii[0] > 0 || op.radii[1] > 0 || op.radii[2] > 0 || op.radii[3] > 0);
        levels.push_back({std::max(op.x, top.x0), std::max(op.y, top.y0), std::min(op.x + op.w, top.x1),
                          std::min(op.y + op.h, top.y1), top.plain && !rounded, i});
        continue;
      }
      case ER_CANVAS_CLIP_RESET:
        levels.push_back({0, 0, width_, height_, false, i});
        barrier = i + 1;
        continue;
      case ER_CANVAS_CLIP_POP:
        if (levels.size() > 1) levels.pop_back();
        continue;
      case ER_CANVAS_ALPHA: alpha = op.alpha; continue;
      case ER_CANVAS_BLEND: blend = op.mode; continue;
      case ER_CANVAS_GROUP_BEGIN: group_depth++; barrier = i + 1; continue;
      case ER_CANVAS_GROUP_END: group_depth--; barrier = i + 1; continue;
      case ER_CANVAS_GROUP_DRAW: case ER_CANVAS_GROUP_XFORM: case ER_CANVAS_GROUP_KEEP: case ER_CANVAS_MOVE:
        barrier = i + 1;
        continue;
      default:
        break;
    }
    const bool square = op.radii[0] == 0 && op.radii[1] == 0 && op.radii[2] == 0 && op.radii[3] == 0;
    const bool opaque = (op.argb >> 24) == 0xFF && alpha == 255 && blend != ER_BLEND_ADD &&
                        (op.kind == ER_CANVAS_FILL || (square && op.kind == ER_CANVAS_RRECT) ||
                         (square && op.kind == ER_CANVAS_RRECT_BORDERED &&
                          (op.width <= 0 || (op.argb2 >> 24) == 0 || (op.argb2 >> 24) == 0xFF)));
    if (group_depth || !levels.back().plain || !(op.kind == ER_CANVAS_CLEAR || opaque)) continue;
    // What it covers: its rect inside the clips; the outermost clip it fills is what it hides.
    const Level& top = levels.back();
    const int x0 = std::max(op.x, top.x0), y0 = std::max(op.y, top.y0);
    const int x1 = std::min(op.x + op.w, top.x1), y1 = std::min(op.y + op.h, top.y1);
    for (std::size_t k = 0; k < levels.size(); k++) {
      const Level& level = levels[k];
      if (level.x0 < x0 || level.y0 < y0 || level.x1 > x1 || level.y1 > y1) continue;
      for (std::size_t j = std::max(k ? level.pushed + 1 : 0, barrier); j < i; j++)
        if (draws_pixels(ops[j].op.kind)) hidden[j] = true;
      break;
    }
  }
  return hidden;
}

void GpuRenderer::draw_ops() {
  struct Bound { const Target* target; int ox, oy; };
  std::vector<Bound> targets{{&target_, 0, 0}};
  std::vector<Clip> clips;
  float alpha = 1.0f;
  int blend = 0;

  glBindVertexArray(vao_);
  glEnable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(program_);
  constexpr const char* kTextureNames[kUnits] = {"u_tex0", "u_tex1", "u_tex2",  "u_tex3",  "u_tex4",
                                                 "u_tex5", "u_tex6", "u_tex7",  "u_tex8",  "u_tex9",
                                                 "u_tex10", "u_tex11", "u_tex12", "u_tex13"};
  for (int unit = 0; unit < kUnits; unit++) set_i(program_, kTextureNames[unit], unit);
  set_i(program_, "u_records", kRecordsUnit);
  glActiveTexture(GL_TEXTURE0 + kRecordsUnit);
  glBindTexture(GL_TEXTURE_BUFFER, records_texture_);
  glActiveTexture(GL_TEXTURE0);
  glBindBuffer(GL_TEXTURE_BUFFER, records_buffer_);

  // The batch: the draws since the last flush, their records and rounded clips, and the textures they read.
  // A blend change, a target change, a clear or a copy ends it; so does a texture past kUnits.
  std::vector<float>& staged = staged_;
  std::vector<float>& clip_sets = clip_sets_;
  staged.clear();
  clip_sets.clear();
  GLuint units[kUnits] = {};
  int unit_count = 0, draws = 0, batch_blend = 0, blend_set = -1;
  int clip_set = -1;
  bool clip_set_current = false;
  auto bind = [&]() {
    const Bound& b = targets.back();
    glBindFramebuffer(GL_FRAMEBUFFER, b.target->fbo);
    glViewport(0, 0, b.target->width, b.target->height);
  };
  auto flush = [&](int reason) {
    if (!draws) return;
    stats_.breaks[reason]++;
    const auto flush_began = std::chrono::steady_clock::now();
    const int record_texels = draws * kRecord;
    const int texels = record_texels + static_cast<int>(clip_sets.size() / 4);
    // A full buffer is orphaned: the driver gives fresh storage while the GPU reads the old.
    if (buffer_at_ + texels > buffer_texels_) {
      glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(buffer_texels_) * 16, nullptr, GL_STREAM_DRAW);
      buffer_at_ = 0;
    }
    staged.insert(staged.end(), clip_sets.begin(), clip_sets.end());
    glBufferSubData(GL_TEXTURE_BUFFER, static_cast<GLintptr>(buffer_at_) * 16,
                    static_cast<GLsizeiptr>(staged.size() * sizeof(float)), staged.data());
    int first = buffer_at_;
#if !GL_ES_HOST && defined(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT)
    if (range_align_ > 0) {
      // The view starts on an aligned texel at or before the batch's first.
      const int align = std::max(1, range_align_ / 16);
      const int start = buffer_at_ / align * align;
      first = buffer_at_ - start;
      glActiveTexture(GL_TEXTURE0 + kRecordsUnit);
      glTexBufferRange(GL_TEXTURE_BUFFER, GL_RGBA32F, records_buffer_, static_cast<GLintptr>(start) * 16,
                       static_cast<GLsizeiptr>(first + texels) * 16);
      glActiveTexture(GL_TEXTURE0);
    }
#endif
    const Bound& b = targets.back();
    set_f(program_, "u_size", {static_cast<float>(b.target->width), static_cast<float>(b.target->height)});
    set_f(program_, "u_origin", {static_cast<float>(b.ox), static_cast<float>(b.oy)});
    set_i(program_, "u_first", first);
    set_i(program_, "u_clip_first", first + record_texels);
    for (int unit = unit_count - 1; unit >= 0; unit--) {
      glActiveTexture(GL_TEXTURE0 + unit);
      glBindTexture(GL_TEXTURE_2D, units[unit]);
    }
    if (batch_blend != blend_set)
      glBlendFunc(GL_ONE, batch_blend == ER_BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    blend_set = batch_blend;
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, draws);
    stats_.batches++;
    stats_.flush_us += elapsed_us(flush_began);
    buffer_at_ += texels;
    staged.clear();
    clip_sets.clear();
    unit_count = draws = 0;
    clip_set_current = false;
  };
  // The rounded clips in effect, innermost first, as the batch's clip set: written once per clip stack.
  auto rounded_clips = [&]() -> int {
    if (clip_set_current) return clip_set;
    clip_set_current = true;
    clip_set = -1;
    int count = 0;
    for (auto it = clips.rbegin(); it != clips.rend() && count < 4 && !it->reset; ++it) count += it->rounded;
    if (!count) return clip_set;
    clip_set = static_cast<int>(clip_sets.size() / 4);
    clip_sets.insert(clip_sets.end(), {static_cast<float>(count), 0, 0, 0});
    count = 0;
    for (auto it = clips.rbegin(); it != clips.rend() && count < 4 && !it->reset; ++it) {
      if (!it->rounded) continue;
      clip_sets.insert(clip_sets.end(), {static_cast<float>(it->bx), static_cast<float>(it->by),
                                         static_cast<float>(it->bw), static_cast<float>(it->bh)});
      clip_sets.insert(clip_sets.end(), it->radii, it->radii + 4);
      count++;
    }
    return clip_set;
  };
  // A draw's record, its quad cut to the clip rect as the scissor would; nullptr when nothing shows. The
  // caller fills records 2 onward.
  auto draw = [&](int kind, int x, int y, int w, int h, GLuint texture, float draw_alpha) -> float* {
    if (!clips.empty()) {
      const Clip& top = clips.back();
      const int x1 = std::min(x + w, top.x + top.w), y1 = std::min(y + h, top.y + top.h);
      x = std::max(x, top.x);
      y = std::max(y, top.y);
      w = x1 - x;
      h = y1 - y;
    }
    if (w <= 0 || h <= 0) return nullptr;
    const int wanted = blend == ER_BLEND_ADD ? ER_BLEND_ADD : 0;
    if (draws && wanted != batch_blend) flush(kBreakBlend);
    if (draws == kBatchDraws) flush(kBreakFull);
    batch_blend = wanted;
    int unit = 0;
    if (texture) {
      while (unit < unit_count && units[unit] != texture) unit++;
      if (unit == kUnits) {
        flush(kBreakTextures);
        unit = 0;
      }
      if (unit == unit_count) units[unit_count++] = texture;
    }
    const int clip = rounded_clips();
    const std::size_t at = staged.size();
    staged.resize(at + kRecord * 4, 0.0f);
    float* r = staged.data() + at;
    r[0] = static_cast<float>(x), r[1] = static_cast<float>(y), r[2] = static_cast<float>(w), r[3] = static_cast<float>(h);
    r[4] = static_cast<float>(kind), r[5] = draw_alpha, r[6] = static_cast<float>(clip), r[7] = static_cast<float>(unit);
    draws++;
    stats_.draws++;
    return r + 8;
  };
  auto put = [](float*& at, std::initializer_list<float> values) {
    std::copy(values.begin(), values.end(), at);
    at += 4;
  };
  auto put_color = [](float*& at, std::uint32_t argb) {
    const auto p = premultiplied(argb);
    std::copy(p.begin(), p.end(), at);
    at += 4;
  };
  // Radii as the software rasterizer clamps them: no corner wider than half the shorter side.
  auto clamp_radii = [](const int in[4], int w, int h, float out[4]) {
    const float most = static_cast<float>(std::min(w, h)) / 2.0f;
    for (int i = 0; i < 4; i++) out[i] = std::clamp(static_cast<float>(in[i]), 0.0f, std::max(most, 0.0f));
  };
  auto shape = [&](const ERCanvasOp& op, int mode, std::uint32_t fill, const int edges[4],
                   const std::uint32_t colors[4]) {
    if (op.w <= 0 || op.h <= 0) return;
    // Nothing to see: a transparent fill with no edge showing (an empty view's background).
    bool shows = mode != 2 && (fill >> 24) != 0;
    for (int i = 0; mode != 0 && i < 4 && !shows; i++) shows = edges[i] > 0 && (colors[i] >> 24) != 0;
    if (!shows) return;
    float* r = draw(mode, op.x, op.y, op.w, op.h, 0, alpha);
    if (!r) return;
    float radii[4];
    clamp_radii(op.radii, op.w, op.h, radii);
    put(r, {static_cast<float>(op.x), static_cast<float>(op.y), static_cast<float>(op.w), static_cast<float>(op.h)});
    put(r, {radii[0], radii[1], radii[2], radii[3]});
    put_color(r, fill);
    if (mode == 0) return;
    const float e[4] = {static_cast<float>(edges[0]), static_cast<float>(edges[1]), static_cast<float>(edges[2]),
                        static_cast<float>(edges[3])};
    put(r, {e[0], e[1], e[2], e[3]});
    put(r, {std::max(radii[0] - std::max(e[0], e[1]), 0.0f), std::max(radii[1] - std::max(e[2], e[1]), 0.0f),
            std::max(radii[2] - std::max(e[2], e[3]), 0.0f), std::max(radii[3] - std::max(e[0], e[3]), 0.0f)});
    for (int i = 0; i < 4; i++) put_color(r, colors[i]);
  };
  // kind 10 shifts the texture by (shift_x, shift_y); 11 maps through the matrix m.
  auto texture_draw = [&](unsigned int texture, int kind, int x, int y, int w, int h, float shift_x, float shift_y,
                          int src_w, int src_h, const float* m, float op_alpha) {
    if (w <= 0 || h <= 0) return;
    float* r = draw(kind, x, y, w, h, texture, alpha * op_alpha);
    if (!r) return;
    put(r, {shift_x, shift_y, static_cast<float>(src_w), static_cast<float>(src_h)});
    if (m) {
      put(r, {m[0], m[1], m[2], m[3]});
      put(r, {m[4], m[5], 0, 0});
    }
  };
  auto clip_to_top = [&](Clip& c) {
    if (clips.empty()) return;
    const Clip& top = clips.back();
    const int x1 = std::min(c.x + c.w, top.x + top.w), y1 = std::min(c.y + c.h, top.y + top.h);
    c.x = std::max(c.x, top.x);
    c.y = std::max(c.y, top.y);
    c.w = std::max(x1 - c.x, 0);
    c.h = std::max(y1 - c.y, 0);
  };

  bind();
  const std::vector<bool> hidden = hidden_ops();
  for (std::size_t index = 0; index < ops_->size(); index++) {
    const Op& recorded = (*ops_)[index];
    const ERCanvasOp& op = recorded.op;
    if (hidden[index]) {
      stats_.hidden++;
      continue;
    }
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
        Clip c{op.x, op.y, op.w, op.h, false, {}, 0, 0, 0, 0, false};
        clip_to_top(c);
        if (c.w <= 0 || c.h <= 0) break;
        flush(kBreakClear);
        const Bound& b = targets.back();
        glEnable(GL_SCISSOR_TEST);
        glScissor(c.x - b.ox, c.y - b.oy, c.w, c.h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        break;
      }
      case ER_CANVAS_CLIP_RECT: {
        Clip c{op.x, op.y, op.w, op.h, false, {}, 0, 0, 0, 0, false};
        clip_to_top(c);
        clips.push_back(c);
        clip_set_current = false;
        break;
      }
      case ER_CANVAS_CLIP_RRECT: {
        Clip c{op.x, op.y, op.w, op.h, true, {}, op.x, op.y, op.w, op.h, false};
        clip_to_top(c);
        clamp_radii(op.radii, op.w, op.h, c.radii);
        c.rounded = c.radii[0] > 0 || c.radii[1] > 0 || c.radii[2] > 0 || c.radii[3] > 0;
        clips.push_back(c);
        clip_set_current = false;
        break;
      }
      case ER_CANVAS_CLIP_RESET:
        clips.push_back(Clip{-(1 << 20), -(1 << 20), 1 << 21, 1 << 21, false, {}, 0, 0, 0, 0, true});
        clip_set_current = false;
        break;
      case ER_CANVAS_CLIP_POP:
        if (!clips.empty()) clips.pop_back();
        clip_set_current = false;
        break;
      case ER_CANVAS_ALPHA:
        alpha = static_cast<float>(op.alpha) / 255.0f;
        break;
      case ER_CANVAS_BLEND:
        blend = op.mode;
        break;
      case ER_CANVAS_GROUP_BEGIN: {
        flush(kBreakGroup);
        const auto began = std::chrono::steady_clock::now();
        Group& g = group(op.buffer, op.w, op.h);
        g.ox = op.x;
        g.oy = op.y;
        targets.push_back({&g.target, op.x, op.y});
        bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        stats_.group_us += elapsed_us(began);
        break;
      }
      case ER_CANVAS_GROUP_END:
        flush(kBreakGroup);
        if (targets.size() > 1) targets.pop_back();
        bind();
        break;
      case ER_CANVAS_GROUP_DRAW: {
        for (const auto& [key, g] : groups_) {
          if (key != op.buffer) continue;
          // Screen pixel (x, y) reads the group's pixel pos[]: the shift between them.
          texture_draw(g.target.texture, 10, op.x, op.y, op.w, op.h, static_cast<float>(op.x - op.pos[0]),
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
          texture_draw(g.target.texture, 11, op.x, op.y, op.w, op.h, static_cast<float>(op.pos[0]),
                       static_cast<float>(op.pos[1]), op.slot, op.width, op.m, 1.0f);
          break;
        }
        break;
      }
      case ER_CANVAS_GRADIENT: {
        if (op.stop_count < 2 || op.w <= 0 || op.h <= 0) break;
        if (op.mode != ER_GRADIENT_LINEAR && op.mode != ER_GRADIENT_RADIAL) break;
        float* r = draw(op.mode == ER_GRADIENT_LINEAR ? 3 : 4, op.x, op.y, op.w, op.h, 0, alpha);
        if (!r) break;
        float radii[4];
        clamp_radii(op.radii, op.w, op.h, radii);
        put(r, {static_cast<float>(op.x), static_cast<float>(op.y), static_cast<float>(op.w), static_cast<float>(op.h)});
        put(r, {radii[0], radii[1], radii[2], radii[3]});
        const int count = std::min(op.stop_count, 4);
        float positions[4] = {};
        for (int i = 0; i < 4; i++) {
          const std::uint32_t c = i < count ? op.stops[i].color : 0;
          put(r, {static_cast<float>(c >> 24), static_cast<float>((c >> 16) & 0xFF), static_cast<float>((c >> 8) & 0xFF),
                  static_cast<float>(c & 0xFF)});
          if (i < count) positions[i] = op.stops[i].position;
        }
        put(r, {positions[0], positions[1], positions[2], positions[3]});
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
        float step_bits;
        std::memcpy(&step_bits, &step, sizeof step_bits);
        put(r, {static_cast<float>(count), op.f[0], op.f[1], min_p});
        put(r, {to_idx, step_bits, static_cast<float>(n), 0});
        break;
      }
      case ER_CANVAS_IMAGE_REGION: {
        const std::uint64_t uploads = stats_.uploads;
        const Image& img = image(op.buffer, op.name, op.slot, op.width, op.mode);
        if (stats_.uploads != uploads) {
          // The copy drew elsewhere: back to the target and the program.
          bind();
          glUseProgram(program_);
        }
        float* r = draw(13, op.x, op.y, op.w, op.h, img.texture, alpha);
        if (!r) break;
        put(r, {0, 0, static_cast<float>(img.width), static_cast<float>(img.height)});
        put(r, {static_cast<float>(op.x), static_cast<float>(op.y), static_cast<float>(op.w), static_cast<float>(op.h)});
        put(r, {static_cast<float>(op.src[0]), static_cast<float>(op.src[1]), static_cast<float>(op.src[2]),
                static_cast<float>(op.src[3])});
        put(r, {static_cast<float>((op.argb >> 16) & 0xFF), static_cast<float>((op.argb >> 8) & 0xFF),
                static_cast<float>(op.argb & 0xFF), op.f[0]});
        put(r, {static_cast<float>(op.area[0]), static_cast<float>(op.area[1]), static_cast<float>(op.area[2]),
                static_cast<float>(op.area[3])});
        float radii[4];
        clamp_radii(op.radii, op.area[2], op.area[3], radii);
        put(r, {radii[0], radii[1], radii[2], radii[3]});
        break;
      }
      case ER_CANVAS_GLYPH: {
        const Glyph& g = glyph(op.buffer, op.w, op.h, op.mode);
        // The glyph's box (wider by the italic shear), cut to the text's clip.
        const int reach = op.f[0] != 0.0f ? static_cast<int>(static_cast<float>(op.h - 1) * 0.2f) + 2 : 0;
        const int x = std::max(op.x, op.area[0]), y = std::max(op.y, op.area[1]);
        const int x1 = std::min(op.x + op.w + reach, op.area[0] + op.area[2]);
        const int y1 = std::min(op.y + op.h, op.area[1] + op.area[3]);
        if (x1 <= x || y1 <= y) break;
        float* r = draw(12, x, y, x1 - x, y1 - y, g.texture, alpha);
        if (!r) break;
        put(r, {static_cast<float>(op.x), static_cast<float>(op.y), static_cast<float>(g.width),
                static_cast<float>(g.height)});
        put_color(r, op.argb);
        put(r, {op.f[0] == 0.0f ? 0.0f : op.mode == 1 ? 1.0f : 2.0f, static_cast<float>(g.x), static_cast<float>(g.y), 0});
        break;
      }
      case ER_CANVAS_GROUP_KEEP: {
        const Group* from = nullptr;
        for (const auto& [key, g] : groups_)
          if (key == op.source) from = &g;
        if (!from || op.w <= 0 || op.h <= 0) break;
        flush(kBreakCopy);
        const Target source = from->target;
        Group& kept = group(op.buffer, source.width, source.height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, kept.target.fbo);
        glBlitFramebuffer(0, 0, op.w, op.h, 0, 0, op.w, op.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        bind();
        break;
      }
      case ER_CANVAS_BORDER_SWEEP: {
        // As border_sweep_render_sw sets up the ring and its centre line.
        if (op.w <= 0 || op.h <= 0 || op.width <= 0 || (op.argb >> 24) == 0 || op.f[1] <= 0.0f) break;
        const int radius = std::clamp(op.radii[0], 0, std::min(op.w, op.h) / 2);
        const float bw = static_cast<float>(op.width), mid = bw * 0.5f;
        const float cr = std::max(static_cast<float>(radius) - mid, 0.0f);
        float* r = draw(5, op.x, op.y, op.w, op.h, 0, alpha);
        if (!r) break;
        put(r, {static_cast<float>(op.x), static_cast<float>(op.y), static_cast<float>(op.w), static_cast<float>(op.h)});
        put_color(r, op.argb);
        put(r, {static_cast<float>(radius), bw, std::max(static_cast<float>(radius) - bw, 0.0f), 0});
        put(r, {static_cast<float>(op.w) * 0.5f - mid - cr, static_cast<float>(op.h) * 0.5f - mid - cr, cr, 0});
        put(r, {op.f[0], op.f[1], 0, 0});
        break;
      }
      case ER_CANVAS_PIXELS: {
        if (op.w <= 0 || op.h <= 0) break;
        stats_.pixel_ops++;
        auto found = pixels_.find(recorded.hash);
        if (found == pixels_.end() || found->second.width != op.w || found->second.height != op.h) {
          // Recorded without a copy only when kept, and kept ones stay until they are replayed.
          if (recorded.pixels.empty()) break;
          if (found != pixels_.end()) {
            flush(kBreakCopy);  // a queued draw may read the texture going
            glDeleteTextures(1, &found->second.texture);
            pixels_bytes_ -= static_cast<std::size_t>(found->second.width) * found->second.height * 4;
            pixels_.erase(found);
          }
          const auto bytes = static_cast<std::size_t>(op.w) * op.h * 4;
          stats_.pixel_bytes += bytes;
          const auto upload_began = std::chrono::steady_clock::now();
          Pixels made{0, op.w, op.h, replay_serial_};
          glGenTextures(1, &made.texture);
          glBindTexture(GL_TEXTURE_2D, made.texture);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
          if (op.w <= staging_texels_) {
            copy_in(made.texture, op.w, op.h, recorded.pixels.data());
            bind();
            glUseProgram(program_);
          } else {
            gl::tex_image_argb(op.w, op.h, recorded.pixels.data());
          }
          note_upload(kUploadPixels, elapsed_us(upload_began), bytes);
          pixels_bytes_ += bytes;
          found = pixels_.emplace(recorded.hash, made).first;
        }
        found->second.used = std::max(found->second.used, replay_serial_);
        texture_draw(found->second.texture, 10, op.x, op.y, op.w, op.h, static_cast<float>(op.x),
                     static_cast<float>(op.y), op.w, op.h, nullptr, 1.0f);
        break;
      }
      case ER_CANVAS_MOVE: {
        // Through the copy target: a framebuffer cannot blit onto itself.
        flush(kBreakCopy);
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
  flush(kBreakEnd);
  glActiveTexture(GL_TEXTURE0);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
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
