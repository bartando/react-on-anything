// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "canvas_trace.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

extern "C" {
#include "er_canvas.h"
}

namespace canvas_trace {
namespace {

constexpr const char* kNames[ER_CANVAS_OP_COUNT] = {
    "fill", "rrect", "rrect-bordered", "rrect-ring", "rrect-ring-edges", "gradient", "shadow", "border-sweep",
    "border-conic", "border-radial", "arc-sector", "vector", "vector-slot", "text", "image", "clear",
    "clip-rect", "clip-rrect", "clip-reset", "clip-pop", "alpha", "blend", "group-begin", "group-end",
    "group-draw", "group-xform", "group-keep", "unhooked", "move", "glyph", "image-region", "pixels"};

std::mutex lock;
FILE* out = nullptr;
std::string pending;
long commit_index = 0;
std::array<long, ER_CANVAS_OP_COUNT> counts{};
std::array<long, ER_CANVAS_OP_COUNT> nested{};
std::string unhooked_samples;
int unhooked_sampled = 0;

void append(const ERCanvasOp& op) {
  char line[256];
  int n = std::snprintf(line, sizeof line, "%*s%s %d,%d %dx%d", op.depth * 2, "", kNames[op.kind], op.x, op.y,
                        op.w, op.h);
  auto more = [&](const char* format, auto... values) {
    if (n > 0 && n < static_cast<int>(sizeof line)) n += std::snprintf(line + n, sizeof line - n, format, values...);
  };
  switch (op.kind) {
    case ER_CANVAS_FILL:
    case ER_CANVAS_TEXT:
      more(" #%08x", op.argb);
      break;
    case ER_CANVAS_RRECT:
    case ER_CANVAS_RRECT_RING:
      more(" #%08x r=%d,%d,%d,%d w=%d", op.argb, op.radii[0], op.radii[1], op.radii[2], op.radii[3], op.width);
      break;
    case ER_CANVAS_RRECT_BORDERED:
      more(" #%08x border=#%08x w=%d r=%d", op.argb, op.argb2, op.width, op.radii[0]);
      break;
    case ER_CANVAS_CLIP_RRECT:
    case ER_CANVAS_RRECT_RING_EDGES:
      more(" r=%d,%d,%d,%d", op.radii[0], op.radii[1], op.radii[2], op.radii[3]);
      break;
    case ER_CANVAS_BORDER_SWEEP:
      more(" #%08x w=%d r=%d phase=%.2f", op.argb, op.width, op.radii[0], op.f[0]);
      break;
    case ER_CANVAS_ALPHA:
    case ER_CANVAS_GROUP_DRAW:
    case ER_CANVAS_UNHOOKED:
      more(" a=%u", op.alpha);
      break;
    case ER_CANVAS_BLEND:
      more(" mode=%u", op.mode);
      break;
    case ER_CANVAS_GROUP_BEGIN:
    case ER_CANVAS_GROUP_END:
      more(" buf=%p", op.buffer);
      break;
    case ER_CANVAS_GROUP_XFORM:
      more(" src=%d,%d %dx%d", op.pos[0], op.pos[1], op.slot, op.width);
      break;
    case ER_CANVAS_MOVE:
      more(" to=%d,%d", op.pos[0], op.pos[1]);
      break;
    default:
      break;
  }
  pending.append(line, n > 0 && n < static_cast<int>(sizeof line) ? n : sizeof line - 1);
  pending.push_back('\n');
}

int watch(const ERCanvasOp* op, void*) {
  std::lock_guard<std::mutex> guard(lock);
  if (op->kind >= ER_CANVAS_OP_COUNT) return ER_CANVAS_PASS;
  (op->depth == 0 ? counts : nested)[op->kind]++;
  if (op->kind == ER_CANVAS_UNHOOKED && unhooked_sampled < 20) {
    char sample[96];
    std::snprintf(sample, sizeof sample, "  commit %ld: %d,%d %dx%d\n", commit_index, op->x, op->y, op->w, op->h);
    unhooked_samples += sample;
    unhooked_sampled++;
  }
  append(*op);
  return ER_CANVAS_PASS;
}

}  // namespace

bool start(const char* path, int render_workers) {
  out = std::fopen(path, "w");
  if (!out) return false;
  if (render_workers > 1)
    std::fprintf(out, "# %d render workers: ops of one commit interleave (PS5_REACT_RENDER_WORKERS=1 orders them)\n",
                 render_workers);
  er_set_canvas(watch, nullptr);
  return true;
}

void commit_begin() {
  std::lock_guard<std::mutex> guard(lock);
  commit_index++;
}

void commit_end() {
  std::lock_guard<std::mutex> guard(lock);
  if (!out || pending.empty()) return;
  std::fprintf(out, "commit %ld\n%s", commit_index, pending.c_str());
  pending.clear();
}

void stop() {
  if (!out) return;
  er_set_canvas(nullptr, nullptr);
  std::lock_guard<std::mutex> guard(lock);
  std::fprintf(out, "summary: %ld commits\n", commit_index);
  for (int kind = 0; kind < ER_CANVAS_OP_COUNT; kind++)
    if (counts[kind] || nested[kind])
      std::fprintf(out, "  %-16s %8ld top  %8ld nested\n", kNames[kind], counts[kind], nested[kind]);
  if (counts[ER_CANVAS_UNHOOKED]) std::fprintf(out, "unhooked pixels (first %d):\n%s", unhooked_sampled, unhooked_samples.c_str());
  std::fclose(out);
  out = nullptr;
}

}  // namespace canvas_trace
