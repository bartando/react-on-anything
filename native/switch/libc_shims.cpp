// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// POSIX calls the shared code uses that devkitA64's newlib leaves out.
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <mutex>

namespace {
// pread and pwrite are a seek and a transfer here: one at a time, so threads sharing a descriptor (the
// network's parallel downloads) cannot move each other's offset in between. Each restores the offset.
std::mutex positioned;

template <typename Transfer>
ssize_t at_offset(int fd, off_t offset, Transfer transfer) {
  std::lock_guard lock(positioned);
  const off_t before = lseek(fd, 0, SEEK_CUR);
  if (before < 0 || lseek(fd, offset, SEEK_SET) < 0) return -1;
  const ssize_t count = transfer();
  const int error = errno;
  lseek(fd, before, SEEK_SET);
  errno = error;
  return count;
}
} // namespace

extern "C" {
ssize_t pread(int fd, void* buffer, size_t size, off_t offset) {
  return at_offset(fd, offset, [&] { return read(fd, buffer, size); });
}

ssize_t pwrite(int fd, const void* buffer, size_t size, off_t offset) {
  return at_offset(fd, offset, [&] { return write(fd, buffer, size); });
}

long sysconf(int name) {
  switch (name) {
    // An application gets three of the four cores; the fourth is the system's.
    case _SC_NPROCESSORS_ONLN: case _SC_NPROCESSORS_CONF: return 3;
    case _SC_PAGESIZE: return 0x1000;
    default: errno = EINVAL; return -1;
  }
}

int getrusage(int, struct rusage*) {
  errno = ENOSYS;
  return -1;
}
}
