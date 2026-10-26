// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "network.hpp"

#include <cstdio>

namespace network {
// The host starts the socket service before anything else, so nxlink can carry stdout.
bool platform_start(std::string&) { return true; }
void platform_stop() {}
// curl's TLS here is the system's ssl service, which has its own certificates.
const char* ca_path() { return nullptr; }
void platform_log(const char* line) { std::printf("%s\n", line); }
}
