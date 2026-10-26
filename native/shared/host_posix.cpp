// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_posix.hpp"
#include "host_api.hpp"
#include "storage_stats.hpp"

#include <dirent.h>
#include <sys/statvfs.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace host {
bool resolve_under(const char* root, const char* path, char* out, std::size_t size) {
  if (!valid_app_path(path)) return false;
  const int written = std::snprintf(out, size, "%s%s", root, path[1] ? path : "");
  return written >= 0 && static_cast<std::size_t>(written) < size;
}

bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user) {
  DIR* dir = opendir(real_path);
  if (!dir) return false;
  while (const dirent* item = readdir(dir))
    if (std::strcmp(item->d_name, ".") && std::strcmp(item->d_name, "..")) visit(item->d_name, user);
  closedir(dir);
  return true;
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
} // namespace host
