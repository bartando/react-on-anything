// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "er_canvas.h"

#include <cstdint>
#include <string>
#include <initializer_list>
#include <unordered_map>
#include <vector>

// Draws the engine's canvas ops (er_canvas.h) with OpenGL into a texture of its own, replaying each
// commit's ops after it. It runs one of two ways:
// - Mirror: beside the software rasterizer rather than instead of it. The engine still rasterizes every
//   pixel into its framebuffer, and compare() shows how far apart the two pictures are.
// - Draw: instead of it. The engine skips what the renderer draws, and the presenter shows its texture.
//
// It takes the ops a GPU renderer would take: one it can draw stands for everything nested in it (a
// bordered rect for its two fills), one it cannot (text, for now) leaves its parts to it. Shapes it has no
// shader for yet (shadows, focus sweeps, arcs, vector icons) it has the engine capture: rasterized into a
// buffer of their own and handed over as pixels. What it cannot draw at all is missing from its picture,
// and listed by compare().
//
// Ops are recorded on the render thread during er_commit and drawn by replay() on the GL thread. The
// engine must run one render worker, so the ops arrive in drawing order.
class GlPresenter;

class GpuRenderer {
public:
  GpuRenderer() = default;
  GpuRenderer(const GpuRenderer&) = delete;
  GpuRenderer& operator=(const GpuRenderer&) = delete;
  ~GpuRenderer();

  enum class Mode { mirror, draw };

  // Where its lines go (the stats every 2 s, shader errors): stdout, unless a host whose stdout goes
  // nowhere (the PS5) gives its own log.
  static void set_log(void (*log)(const char* line));
  // Waits for the GPU after each replay and reports how long it took: a diagnostic, since it costs the
  // pipelining between frames.
  void set_sync(bool sync) { sync_ = sync; }
  // With the GL context current. Installs the canvas.
  bool start(int width, int height, Mode mode);
  void stop();
  bool active() const { return target_.fbo != 0; }
  bool drawing() const { return active() && mode_ == Mode::draw; }
  // The picture, laid out like the software framebuffer (row 0 at the top), premultiplied.
  unsigned int texture() const { return target_.texture; }

  // Draws the ops recorded since the last replay into the mirror's texture: a commit's, and any drawn
  // outside one (the first frame is drawn while the app boots). Leaves the framebuffer binding,
  // viewport, scissor and blending as the presenter expects them (default framebuffer, off). Returns
  // whether the picture changed.
  bool replay();

  // Compares the mirror with the software framebuffer `cpu`, both on their own and as the screen shows
  // them, over the presenter's layers (a backdrop the engine leaves a hole for). Returns a summary line
  // and writes the two screens and their difference next to `path` (path-cpu.bmp, path-gpu.bmp,
  // path-diff.bmp), half size. With the GL context current, after the frame was presented.
  std::string compare(const std::uint32_t* cpu, GlPresenter& presenter, const std::string& path);
  // Writes the screen as the presenter shows the renderer's picture to `path` (a BMP, half size).
  bool save_screen(GlPresenter& presenter, const std::string& path);

private:
  struct Target { unsigned int texture = 0, fbo = 0; int width = 0, height = 0; };
  struct Group { Target target; int ox = 0, oy = 0; };
  struct Clip { int x, y, w, h; bool rounded; float radii[4]; int bx, by, bw, bh; bool reset; };
  struct Op;

  Target target_;
  Target copy_;    // scratch for scroll moves
  Target screen_;  // where compare() composites a screen
  int width_ = 0, height_ = 0;
  Mode mode_ = Mode::mirror;
  bool sync_ = false;
  // What replay() did over the current stats window, reported every 2s.
  struct Stats {
    std::uint64_t replays = 0, ops = 0, draws = 0, regroups = 0, pixel_ops = 0, pixel_bytes = 0, uploads = 0;
    std::uint64_t replay_us = 0, max_replay_us = 0;
    // Of replay: issuing the draws, and with sync_ on, waiting for the GPU to finish them after. And the
    // time the engine's commits spent handing ops over (record()).
    std::uint64_t submit_us = 0, finish_us = 0, max_finish_us = 0, record_us = 0;
    std::uint64_t batches = 0;  // the instanced calls the draws went out in
    // Time spent in the driver creating and filling textures, by kind (images, glyphs, CPU-drawn pixels).
    std::uint64_t upload_us[3] = {}, max_upload_us[3] = {}, upload_count[3] = {}, upload_bytes[3] = {};
    std::uint64_t capture_px[ER_CANVAS_OP_COUNT] = {}, captures[ER_CANVAS_OP_COUNT] = {};
    std::int64_t window_start_us = 0;
  } stats_;
  unsigned int vao_ = 0, program_ = 0;
  // The draws' records (see kVertex), written on after the last batch's until full, then orphaned.
  unsigned int records_buffer_ = 0, records_texture_ = 0;
  int buffer_texels_ = 0, buffer_at_ = 0;
  static constexpr int kBufferTexels = 1 << 18;
  static constexpr int kBatchDraws = 2048;
  std::vector<float> staged_, clip_sets_;
  // Captured ops' pixels by content: what the engine draws the same way each frame (an icon) is uploaded
  // once, and a new picture gets a texture of its own instead of overwriting one a queued draw still
  // reads, which stalled the Switch's driver for tens of milliseconds.
  struct Pixels { unsigned int texture = 0; int width = 0, height = 0; std::uint64_t used = 0; };
  std::unordered_map<std::uint64_t, Pixels> pixels_;
  std::uint64_t replay_serial_ = 0;
  std::size_t pixels_bytes_ = 0;
  void evict_pixels();
  std::vector<Op>* ops_ = nullptr;
  std::vector<std::pair<const void*, Group>> groups_;
  std::vector<std::uint64_t> missing_;
  struct Glyph { unsigned int texture = 0; int width = 0, height = 0; };
  std::unordered_map<const void*, Glyph> glyphs_;
  struct Image { unsigned int texture = 0; int width = 0, height = 0, format = 0; std::string name; };
  std::unordered_map<const void*, Image> images_;

  static int record(const ERCanvasOp* op, void* self);
  int keep(const ERCanvasOp* op);
  // The answer to an op it keeps.
  int kept() const { return mode_ == Mode::draw ? ER_CANVAS_TAKEN : ER_CANVAS_PASS; }
  static bool captures(int kind);
  bool takes(int kind) const;
  Group& group(const void* buffer, int w, int h);
  const Glyph& glyph(const void* bitmap, int w, int h, int bpp);
  const Image& image(const void* pixels, const char* name, int w, int h, int format);
  void note_upload(int kind, std::uint64_t us, std::uint64_t bytes);
  void draw_ops();
  struct Uniform { int location; int bytes; std::uint8_t value[64]; };
  std::vector<Uniform> uniforms_;
  std::unordered_map<std::uint64_t, std::size_t> uniform_index_; // program and name pointer → uniforms_
  Uniform& uniform(unsigned int program, const char* name);
  bool changes(Uniform& u, const void* data, int bytes);
  void set_i(unsigned int program, const char* name, int value);
  void set_f(unsigned int program, const char* name, std::initializer_list<float> values);
  bool screen_target();
};
