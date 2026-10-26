// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "recorder.hpp"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

#include <csignal>

namespace {
// `text` as one single-quoted shell word.
std::string quoted(const std::string& text) {
  std::string out = "'";
  for (char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}
} // namespace

void Recorder::start(const char* path, int fps) {
  path_ = path ? path : "";
  fps_ = fps > 0 && fps <= 120 ? fps : 30;
}

bool Recorder::add(int width, int height) {
  if (!recording() || width <= 0 || height <= 0) return false;
  if (!ffmpeg_) {
    if (frames_) return false;  // ffmpeg did not start
    width_ = width;
    height_ = height;
    const std::string command =
        "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(width) + "x" +
        std::to_string(height) + " -r " + std::to_string(fps_) +
        " -i - -vf \"vflip,scale='min(1920,iw)':-2:flags=lanczos\" -c:v libx264 -preset slow -crf 16"
        " -pix_fmt yuv420p -movflags +faststart " + quoted(path_);
    std::signal(SIGPIPE, SIG_IGN);
    ffmpeg_ = popen(command.c_str(), "w");
    if (!ffmpeg_) {
      std::fprintf(stderr, "ROA_RECORD: cannot run ffmpeg\n");
      frames_ = -1;
      return false;
    }
    pixels_.resize(static_cast<std::size_t>(width) * height * 4);
  }
  if (width != width_ || height != height_) return false;
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
  if (std::fwrite(pixels_.data(), 1, pixels_.size(), ffmpeg_) != pixels_.size()) {
    std::fprintf(stderr, "ROA_RECORD: ffmpeg stopped taking frames\n");
    pclose(ffmpeg_);
    ffmpeg_ = nullptr;
    return false;
  }
  frames_++;
  return true;
}

long Recorder::finish() {
  if (ffmpeg_ && pclose(ffmpeg_) != 0) std::fprintf(stderr, "ROA_RECORD: ffmpeg failed writing %s\n", path_.c_str());
  ffmpeg_ = nullptr;
  path_.clear();
  return frames_;
}
