// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// What the Android host does not have yet: archive extraction (libarchive, xz). Its native API exists so an
// app loads; a request fails as a refused one would.
#include "archives.hpp"
#include "host_api.hpp"

namespace archives {
void stop() {}
} // namespace archives

namespace {
JSValue nothing(JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }
JSValue none(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewArray(ctx); }
JSValue archives_unavailable(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  return JS_ThrowInternalError(ctx, "archives: archives are not supported on Android yet");
}
} // namespace

JSValue roa_archive_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "extract", JS_NewCFunction(ctx, archives_unavailable, "extract", 3));
  JS_SetPropertyStr(ctx, api, "inspect", JS_NewCFunction(ctx, archives_unavailable, "inspect", 1));
  JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, nothing, "cancel", 1));
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, none, "poll", 0));
  return api;
}
