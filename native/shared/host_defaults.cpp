// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The host:: functions of a platform without the PS5's system services: one player, no light bar, rumble
// or adaptive triggers, no HDMI bitstream, no mounts to list, no relaunch. The Switch and Android link it;
// a host that gains one of these implements it in its own host_platform.cpp and stops linking this.
#include "host_api.hpp"
#include "audio_sink.hpp"

#include <cstdio>

namespace host {
bool arrange_relaunch(char* error, std::size_t size) {
  std::snprintf(error, size, "Relaunching is not supported on %s.", platform_name());
  return false;
}

int list_mounts(MountEntry*, int) { return 0; }

void trace(const char*) {}

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

bool open_url(const char*) { return false; }

void set_light_bar(std::uint8_t, std::uint8_t, std::uint8_t) {}
void reset_light_bar() {}
void vibrate(float, float) {}
void set_trigger_effect(unsigned, int, const std::uint8_t*, std::size_t) {}

// HDMI bitstream output is the system's business here: video audio is always decoded.
bool has_bitstream() { return false; }
bool open_bitstream(iec61937::Codec, BitstreamFill) { return false; }
void close_bitstream() {}
bool sink_edid(std::vector<std::uint8_t>&) { return false; }
} // namespace host
