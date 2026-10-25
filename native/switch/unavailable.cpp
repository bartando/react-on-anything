// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// What the Switch host does not have yet: archive extraction, and a hardware video decoder (the
// software one plays up to 1080p). Their native APIs exist so an app loads.
#include "archives.hpp"
#include "host_api.hpp"
#include "image_loader.hpp"
#include "network.hpp"
#include "video_frame.hpp"

#include <cstdint>

namespace archives {
void stop() {}
void trace(const char*, const char*) {}
std::size_t pipeline_bytes() { return 32 * 1024 * 1024; }
bool hold_memory(bool hold) { images::hold_memory(hold); return true; }
bool downloading() { return network::downloading(); }
bool list_directory(const char* path, void (*visit)(const char*, void*), void* user) {
  return host::read_dir(path, visit, user);
}
} // namespace archives

namespace {
JSValue nothing(JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }
JSValue none(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewArray(ctx); }
JSValue archives_unavailable(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_ThrowInternalError(ctx, "archives: archives are not supported on the Switch yet");
}
} // namespace

JSValue ps5_react_archive_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "extract", JS_NewCFunction(ctx, archives_unavailable, "extract", 3));
  JS_SetPropertyStr(ctx, api, "inspect", JS_NewCFunction(ctx, archives_unavailable, "inspect", 1));
  JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, nothing, "cancel", 1));
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, none, "poll", 0));
  return api;
}

void video::preload_platform_decoder() {}
std::int64_t video::software_pixel_limit() { return 1920 * 1088; }
std::unique_ptr<video::Decoder> video::make_platform_decoder(const AVCodecParameters*, int, int) { return nullptr; }
