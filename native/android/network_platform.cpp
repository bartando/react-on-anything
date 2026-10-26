// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "network.hpp"
#include "host_platform.hpp"

#include <android/log.h>
#include <string>

namespace {
std::string ca_bundle;
}

void android_set_ca_bundle(const char* path) { ca_bundle = path ? path : ""; }

namespace network {
bool platform_start(std::string&) { return true; }
void platform_stop() {}
// The system's certificates, gathered by the host into one file at startup (curl's CA path wants
// OpenSSL's hash names, which Android's store does not use).
const char* ca_path() { return ca_bundle.empty() ? nullptr : ca_bundle.c_str(); }
void platform_log(const char* line) { __android_log_write(ANDROID_LOG_INFO, "react-on-anything", line); }
}
