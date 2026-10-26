// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstddef>
#include <cstring>

// The host:: functions a POSIX filesystem answers the same on the desktop, the Switch and Android
// (host_posix.cpp: read_dir, disk_usage), and the app-path rule every host applies.
namespace host {
// Whether an app path is absolute and names no parent directory: no "/.." component, anywhere.
inline bool valid_app_path(const char* path) {
  if (path[0] != '/') return false;
  for (const char* part = path; part; part = std::strchr(part + 1, '/'))
    if (part[1] == '.' && part[2] == '.' && (part[3] == '/' || part[3] == '\0')) return false;
  return true;
}

// A valid app path under the real directory `root`, into `out`: "/" is the root itself.
bool resolve_under(const char* root, const char* path, char* out, std::size_t size);
} // namespace host
