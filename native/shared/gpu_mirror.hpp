// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "er_canvas.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Draws the engine's canvas ops (er_canvas.h) with OpenGL, beside the software rasterizer rather than
// instead of it: the engine still rasterizes every pixel into its framebuffer, and the mirror replays the
// same ops into a texture of its own after each commit. Comparing the two shows how far the GPU renderer
// has come, op kind by op kind, before it is trusted to draw alone.
//
// The mirror takes the ops a GPU renderer would take: one it can draw stands for everything nested in it
// (a bordered rect for its two fills), one it cannot (text, for now) leaves its parts to it. Shapes it has
// no shader for yet (shadows, focus sweeps, arcs, vector icons) it has the engine capture: rasterized into a
// buffer of their own and handed over as pixels. What it cannot draw at all is missing from its picture,
// and listed by compare().
//
// Ops are recorded on the render thread during er_commit and drawn by replay() on the GL thread. The
// engine must run one render worker, so the ops arrive in drawing order.
class GlPresenter;

class GpuMirror {
public:
  GpuMirror() = default;
  GpuMirror(const GpuMirror&) = delete;
  GpuMirror& operator=(const GpuMirror&) = delete;
  ~GpuMirror();

  // With the GL context current. Installs the canvas.
  bool start(int width, int height);
  void stop();
  bool active() const { return target_.fbo != 0; }

  // Draws the ops recorded since the last replay into the mirror's texture: a commit's, and any drawn
  // outside one (the first frame is drawn while the app boots). Leaves the framebuffer binding,
  // viewport, scissor and blending as the presenter expects them (default framebuffer, off).
  void replay();

  // Compares the mirror with the software framebuffer `cpu`, both on their own and as the screen shows
  // them, over the presenter's layers (a backdrop the engine leaves a hole for). Returns a summary line
  // and writes the two screens and their difference next to `path` (path-cpu.bmp, path-gpu.bmp,
  // path-diff.bmp), half size. With the GL context current, after the frame was presented.
  std::string compare(const std::uint32_t* cpu, GlPresenter& presenter, const std::string& path);

private:
  struct Target { unsigned int texture = 0, fbo = 0; int width = 0, height = 0; };
  struct Group { Target target; int ox = 0, oy = 0; };
  struct Clip { int x, y, w, h; bool rounded; float radii[4]; int bx, by, bw, bh; bool reset; };
  struct Op;

  Target target_;
  Target copy_;    // scratch for scroll moves
  Target screen_;  // where compare() composites a screen
  int width_ = 0, height_ = 0;
  unsigned int vao_ = 0, shape_ = 0, image_ = 0;
  unsigned int pixels_ = 0;  // the texture captured ops' pixels are drawn from
  std::vector<Op>* ops_ = nullptr;
  std::vector<std::pair<const void*, Group>> groups_;
  std::vector<std::uint64_t> missing_;
  struct Glyph { unsigned int texture = 0; int width = 0, height = 0; };
  std::unordered_map<const void*, Glyph> glyphs_;
  struct Image { unsigned int texture = 0; int width = 0, height = 0, format = 0; std::string name; };
  std::unordered_map<const void*, Image> images_;

  static int record(const ERCanvasOp* op, void* self);
  static bool captures(int kind);
  bool takes(int kind) const;
  Group& group(const void* buffer, int w, int h);
  const Glyph& glyph(const void* bitmap, int w, int h, int bpp);
  const Image& image(const void* pixels, const char* name, int w, int h, int format);
  void draw_ops();
};
