// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The Android video decoder: MediaCodec decodes in hardware straight into a SurfaceTexture, so a Frame
// it hands out has no planes. Its owner is a SurfaceFrame, which queues the picture to the texture when
// drawn (gl_video.cpp here) or gives the buffer back to the codec when dropped.
#pragma once

#include <android/surface_texture.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace video {

// A codec and the SurfaceTexture it renders into, shared by the decoder and the frames still out.
struct CodecOutput {
  CodecOutput() = default;
  CodecOutput(const CodecOutput&) = delete;
  CodecOutput& operator=(const CodecOutput&) = delete;
  ~CodecOutput();

  // Gives buffer `index` back, queued to the texture as `render_ns` or dropped (0); a buffer from
  // before the last flush is the codec's again already.
  void release(int index, std::uint64_t generation, std::int64_t render_ns);

  std::mutex lock;
  AMediaCodec* codec = nullptr;
  std::uint64_t generation = 0; // bumped by every flush

  jobject surface_texture = nullptr, surface = nullptr;
  ASurfaceTexture* native = nullptr;

  // GL thread.
  unsigned int texture = 0;    // attached on the first draw
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::int64_t latched_ns = 0; // the timestamp of the picture in the texture
};

// Deletes a texture on the GL thread, at the next present.
void retire_texture(unsigned int texture);

struct SurfaceFrame {
  std::shared_ptr<CodecOutput> output;
  int index = -1;
  std::uint64_t generation = 0;
  std::int64_t render_ns = 0; // when queued to the texture; -1 if it could not be

  ~SurfaceFrame() {
    if (!render_ns) output->release(index, generation, 0);
  }
  // Queues the picture to the texture, once.
  void render();
};

} // namespace video
