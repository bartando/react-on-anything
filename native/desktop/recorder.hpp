// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The preview's ROA_RECORD: each frame the window draws, read back and piped into ffmpeg (from PATH) as
// a video of `fps` frames a second, at most 1920 wide. The host steps its clock one video frame per
// drawn frame, so the video plays smoothly however slowly the frames were drawn and encoded.
class Recorder {
public:
  ~Recorder() { finish(); }
  void start(const char* path, int fps);
  bool recording() const { return !path_.empty(); }
  std::int64_t frame_us() const { return 1000000 / fps_; }
  // The current back buffer, w x h; the first frame fixes the video's size, and a frame of another size
  // (the window resized) is left out.
  bool add(int width, int height);
  // Waits for ffmpeg to write the file. Returns the frames it got.
  long finish();

private:
  std::string path_;
  int fps_ = 30, width_ = 0, height_ = 0;
  long frames_ = 0;
  std::FILE* ffmpeg_ = nullptr;
  std::vector<unsigned char> pixels_;
};
