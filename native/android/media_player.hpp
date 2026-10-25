// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// video::Player on Android: the system's MediaPlayer fetches, demuxes, decodes in hardware and plays the
// sound itself, and draws the picture into a SurfaceTexture, an external texture GlVideo::draw samples.
// Its Frame has no planes: Frame::owner is the texture. Everything but prepare() is called on the render
// thread, which owns the GL context; MediaPlayer reports nothing back without Java code, so update()
// polls it.
#pragma once

#include "video_player.hpp"

#include <jni.h>

namespace video {

class Player {
public:
  Player(std::string url, const Options& options);
  ~Player();
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  void set_paused(bool paused, std::int64_t now_us);
  void seek(double seconds);
  void set_volume(float volume);
  void set_muted(bool muted);
  void set_loop(bool loop);
  // The TV's delay is the system's to correct; MediaPlayer has no offset.
  void set_audio_delay(double) {}
  void select_audio_track(int index);
  // Subtitles are not drawn yet: the app gets no tracks to pick.
  void select_subtitle_track(int) {}
  int add_subtitle_file(const std::string&, const std::string&, const std::string&) { return -1; }
  void set_audio_output(bool on);
  // MediaPlayer decodes or passes the sound through as the system's output settings say.
  void set_audio_mode(AudioOutput) {}
  std::vector<std::shared_ptr<void>> detach_audio() { return {}; }

  void update(std::int64_t now_us, std::int64_t frame_us);
  const Frame* frame() const { return started_ ? &shown_ : nullptr; }
  Status status() const;

  // The picture's texture, shared with the frames handed out; released on the GL thread.
  struct Picture;

private:
  void prepare_main(std::string url, Options options);
  void apply_volume();
  // A MediaPlayer method without arguments; false (and the state failed) when it threw.
  bool call(const char* name);

  jobject player_ = nullptr;
  std::shared_ptr<Picture> picture_;
  std::thread preparer_;
  std::atomic<bool> prepared_{false};

  mutable std::mutex mutex_;
  Status status_;

  // Render thread.
  bool started_ = false, paused_ = false, muted_ = false, loop_ = false, audio_output_ = true;
  float volume_ = 1;
  double pending_seek_ = -1;
  int last_position_ms_ = -1;
  std::int64_t stalled_since_us_ = 0;
  Frame shown_;
};

} // namespace video
