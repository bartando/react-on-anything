// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_platform.hpp"
#include "host_api.hpp"
#include "audio_sink.hpp"
#include "storage_stats.hpp"
#include "audio/mixer.hpp"

#include <aaudio/AAudio.h>
#include <atomic>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::string root = "/sdcard";
std::string model = "Android", release;
AAudioStream* audio = nullptr;

aaudio_data_callback_result_t render_audio(AAudioStream*, void* mixer, void* data, int32_t frames) {
  static_cast<hui::audio::Mixer*>(mixer)->render(static_cast<std::int16_t*>(data), frames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
} // namespace

void android_set_root(const char* path) { root = path; }
namespace {
std::atomic<ANativeWindow*> window{nullptr};
}
void android_set_window(ANativeWindow* to) { window = to; }
ANativeWindow* android_window() { return window; }
const char* android_root() { return root.c_str(); }
void android_set_device(const char* device_model, const char* device_release) {
  model = device_model;
  release = device_release;
}

namespace host {
const char* platform_name() {
  return "android";
}

bool arrange_relaunch(char* error, std::size_t size) {
  std::snprintf(error, size, "Relaunching is not supported on Android.");
  return false;
}

int list_mounts(MountEntry*, int) {
  return 0;
}

bool disk_usage(const char* real_path, double& total, double& free) {
  struct statvfs fs;
  if (statvfs(real_path, &fs) != 0) return false;
  if (!storage::disk_bytes(fs.f_frsize, fs.f_blocks, static_cast<std::int64_t>(fs.f_bavail), total, free)) {
    errno = EIO;
    return false;
  }
  return true;
}

void trace(const char*) {}

bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user) {
  DIR* dir = opendir(real_path);
  if (!dir) return false;
  while (const dirent* item = readdir(dir))
    if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) visit(item->d_name, user);
  closedir(dir);
  return true;
}

bool resolve_path(const char* path, char* out, std::size_t size) {
  if (path[0] != '/' || std::strstr(path, "/../")) return false;
  const std::size_t length = std::strlen(path);
  if (length >= 3 && !std::strcmp(path + length - 3, "/..")) return false;
  const int written = std::snprintf(out, size, "%s%s", root.c_str(), length == 1 ? "" : path);
  return written >= 0 && static_cast<std::size_t>(written) < size;
}

void device_info(DeviceInfo& info) {
  std::snprintf(info.model, sizeof info.model, "%s", model.c_str());
  if (!release.empty()) std::snprintf(info.firmware, sizeof info.firmware, "Android %s", release.c_str());
  struct sysinfo memory = {};
  if (sysinfo(&memory) == 0)
    info.free_memory = static_cast<std::int64_t>(memory.freeram + memory.bufferram) * memory.mem_unit;
  rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    info.process_time = (static_cast<std::int64_t>(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1000000 +
                        usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}

bool foreground_user(User& user) {
  user.id = 1;
  std::snprintf(user.name, sizeof user.name, "player");
  return true;
}

int logged_in_users(User* users, int max) {
  return max > 0 && foreground_user(users[0]) ? 1 : 0;
}

bool notify(const char* message, const char* sub_message) {
  std::printf("[notify] %s%s%s\n", message, *sub_message ? ": " : "", sub_message);
  return true;
}

bool open_url(const char*) {
  return false;
}

// No light bar, rumble or adaptive triggers on an Android TV controller through NativeActivity.
void set_light_bar(std::uint8_t, std::uint8_t, std::uint8_t) {}
void reset_light_bar() {}
void vibrate(float, float) {}
void set_trigger_effect(unsigned, int, const std::uint8_t*, std::size_t) {}
// The activity keeps the screen on (AWINDOW_FLAG_KEEP_SCREEN_ON) while it is in front.
void keep_awake(bool) {}

bool start_audio(hui::audio::Mixer& mixer) {
  AAudioStreamBuilder* builder = nullptr;
  if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(builder, 2);
  AAudioStreamBuilder_setSampleRate(builder, hui::audio::kSampleRate);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setDataCallback(builder, render_audio, &mixer);
  const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &audio);
  AAudioStreamBuilder_delete(builder);
  if (opened != AAUDIO_OK || AAudioStream_requestStart(audio) != AAUDIO_OK) {
    std::printf("[sound] no audio output: %s\n", AAudio_convertResultToText(opened));
    if (audio) AAudioStream_close(audio);
    audio = nullptr;
    return false;
  }
  return true;
}

void stop_audio() {
  if (!audio) return;
  AAudioStream_requestStop(audio);
  AAudioStream_close(audio);
  audio = nullptr;
}

// HDMI bitstream output is the system's business on Android: video audio is always decoded.
bool has_bitstream() { return false; }
bool open_bitstream(iec61937::Codec, BitstreamFill) { return false; }
void close_bitstream() {}
bool sink_edid(std::vector<std::uint8_t>&) { return false; }
} // namespace host
