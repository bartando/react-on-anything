// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_platform.hpp"
#include "host_api.hpp"
#include "host_posix.hpp"
#include "audio_sink.hpp"
#include "audio/mixer.hpp"

#include <switch.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string root = "sdmc:/switch";

// audout plays buffers it was handed; a thread keeps a few queued, each a grain the mixer renders.
constexpr int kAudioBuffers = 3;
constexpr int kAudioFrames = 1024; // 21ms at 48kHz
struct AudioBuffer {
  AudioOutBuffer out{};
  void* memory = nullptr;
};
AudioBuffer audio_buffers[kAudioBuffers];
std::atomic<bool> audio_running{false};
std::thread audio_thread;

void audio_main(hui::audio::Mixer* mixer) {
  for (AudioBuffer& buffer : audio_buffers) {
    mixer->render(static_cast<std::int16_t*>(buffer.memory), kAudioFrames);
    audoutAppendAudioOutBuffer(&buffer.out);
  }
  while (audio_running) {
    AudioOutBuffer* released = nullptr;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000)) || !released) continue;
    mixer->render(static_cast<std::int16_t*>(released->buffer), kAudioFrames);
    audoutAppendAudioOutBuffer(released);
  }
}
} // namespace

void switch_set_root(const char* path) { root = path; }
const char* switch_root() { return root.c_str(); }

namespace host {
const char* platform_name() {
  return "switch";
}

bool resolve_path(const char* path, char* out, std::size_t size) {
  return resolve_under(root.c_str(), path, out, size);
}

void device_info(DeviceInfo& info) {
  std::snprintf(info.model, sizeof info.model, "Nintendo Switch");
  SetSysFirmwareVersion firmware;
  if (R_SUCCEEDED(setsysInitialize())) {
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware)))
      std::snprintf(info.firmware, sizeof info.firmware, "%s", firmware.display_version);
    setsysExit();
  }
  u64 total = 0, used = 0;
  if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
      R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))
    info.free_memory = static_cast<std::int64_t>(total - used);
  u32 percent = 0;
  PsmChargerType charger = PsmChargerType_Unconnected;
  if (R_SUCCEEDED(psmInitialize())) {
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&percent)) && R_SUCCEEDED(psmGetChargerType(&charger))) {
      info.battery = static_cast<int>(percent);
      info.charging = charger != PsmChargerType_Unconnected;
    }
    psmExit();
  }
  NifmInternetConnectionType type{};
  NifmInternetConnectionStatus status{};
  u32 strength = 0;
  if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
        status == NifmInternetConnectionStatus_Connected) {
      const bool wifi = type == NifmInternetConnectionType_WiFi;
      std::snprintf(info.network, sizeof info.network, "%s", wifi ? "wifi" : "ethernet");
      if (wifi) info.wifi_signal = static_cast<int>(strength);
    }
    nifmExit();
  }
}

// Media playback state keeps the console from dimming and sleeping while it is set.
void keep_awake(bool enabled) {
  appletSetMediaPlaybackState(enabled);
}

bool start_audio(hui::audio::Mixer& mixer) {
  if (R_FAILED(audoutInitialize())) {
    std::printf("[sound] no audio output\n");
    return false;
  }
  const std::size_t bytes = kAudioFrames * 2 * sizeof(std::int16_t);
  const std::size_t aligned = (bytes + 0xfff) & ~std::size_t{0xfff};
  for (AudioBuffer& buffer : audio_buffers) {
    buffer.memory = std::aligned_alloc(0x1000, aligned);
    std::memset(buffer.memory, 0, aligned);
    buffer.out.buffer = buffer.memory;
    buffer.out.buffer_size = aligned;
    buffer.out.data_size = bytes;
  }
  if (R_FAILED(audoutStartAudioOut())) {
    audoutExit();
    return false;
  }
  audio_running = true;
  audio_thread = std::thread(audio_main, &mixer);
  return true;
}

void stop_audio() {
  if (!audio_running) return;
  audio_running = false;
  audio_thread.join();
  audoutStopAudioOut();
  audoutExit();
  for (AudioBuffer& buffer : audio_buffers) std::free(buffer.memory);
}
} // namespace host
