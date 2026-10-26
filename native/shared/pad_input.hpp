// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "actions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Controller handling the hosts share, so a direction repeats and a stick or trigger feels the same
// on every platform.

// A stick axis (-1..1) with the deadzone cut out and the rest stretched back to full travel.
inline float stick_deadzone(float value) {
  constexpr float deadzone = 0.16f;
  value = std::clamp(value, -1.0f, 1.0f);
  if (std::abs(value) < deadzone) return 0;
  return std::copysign((std::abs(value) - deadzone) / (1 - deadzone), value);
}

// An analog trigger acts on crossing half travel, and re-arms below a quarter.
struct TriggerEdge {
  bool down = false;
  // Whether `travel` (0..1) just crossed into a press.
  bool crossed(float travel) {
    if (!down && travel > 0.5f) return down = true;
    if (down && travel < 0.25f) down = false;
    return false;
  }
};

// A held direction repeats after 350 ms, every 110 ms; the system's own key repeats are ignored.
class Repeater {
public:
  // A press of `action` by `source`, whatever number the host tells its keys and buttons apart by.
  void press(HostAction action, int source, std::uint64_t now_ms) {
    if (!repeats(action)) return;
    action_ = action_name(action);
    source_ = source;
    next_ms_ = now_ms + 350;
  }
  void release(int source) {
    if (source == source_) action_ = nullptr;
  }
  bool holding() const { return action_ != nullptr; }
  int source() const { return source_; }
  // The action to dispatch again at `now_ms`, or nullptr; `down` is whether the source is still held.
  const char* due(bool down, std::uint64_t now_ms) {
    if (!action_) return nullptr;
    if (!down) {
      action_ = nullptr;
      return nullptr;
    }
    if (now_ms < next_ms_) return nullptr;
    next_ms_ = now_ms + 110;
    return action_;
  }

private:
  const char* action_ = nullptr;
  int source_ = -1;
  std::uint64_t next_ms_ = 0;
};
