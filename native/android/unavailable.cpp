// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// What the Android host does not have yet: archive extraction (libarchive, xz) and video playback (FFmpeg).
// Their native APIs exist so an app loads; a request fails as a refused one would.
#include "archives.hpp"
#include "gl_video.hpp"
#include "host_api.hpp"
#include "video_api.hpp"

namespace archives {
void stop() {}
} // namespace archives

namespace video_layers {
bool find(const char*, Layer&) { return false; }
bool sample(const char*, int, int, int, int, int, int, unsigned[3]) { return false; }
void update(std::int64_t, std::int64_t) {}
void stop() {}
std::string summary() { return {}; }
} // namespace video_layers

bool GlVideo::init(bool) { return false; }
void GlVideo::release() {}
void GlVideo::draw(std::uint64_t, const video::Frame&, Fit, const Target&, int, int, int, int, int, int, int,
                   const Compare&) {}
void GlVideo::end_present() {}
std::string GlVideo::take_upscale_log() { return {}; }
void GlVideo::dump_upscale_at(const std::string&, std::uint64_t) {}

namespace {
JSValue video_unavailable(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_ThrowInternalError(ctx, "video.open: video playback is not supported on Android yet");
}
JSValue video_nothing(JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }
JSValue video_poll(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewArray(ctx); }
JSValue archives_unavailable(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_ThrowInternalError(ctx, "archives: archives are not supported on Android yet");
}
} // namespace

JSValue ps5_react_archive_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "extract", JS_NewCFunction(ctx, archives_unavailable, "extract", 3));
  JS_SetPropertyStr(ctx, api, "inspect", JS_NewCFunction(ctx, archives_unavailable, "inspect", 1));
  JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, video_nothing, "cancel", 1));
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, video_poll, "poll", 0));
  return api;
}

JSValue ps5_react_video_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "open", JS_NewCFunction(ctx, video_unavailable, "open", 2));
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, video_poll, "poll", 0));
  for (const char* name : {"close", "setPaused", "seek", "setVolume", "setMuted", "setLoop", "setResizeMode",
                           "selectAudioTrack", "selectSubtitleTrack", "setAudioDelay", "setUpscale",
                           "setAudioOutput", "setCompare", "addSubtitle"})
    JS_SetPropertyStr(ctx, api, name, JS_NewCFunction(ctx, video_nothing, name, 1));
  return api;
}
