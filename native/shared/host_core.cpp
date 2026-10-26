// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_core.hpp"

#include "archives.hpp"
#include "browser_capture.hpp"
#include "canvas_trace.hpp"
#include "damage_tracker.hpp"
#include "gl_presenter.hpp"
#include "image_loader.hpp"
#include "lan.hpp"
#include "network.hpp"
#include "packages.hpp"
#include "removals.hpp"
#include "render_workers.hpp"
#include "screenshot.hpp"
#include "text_shaper.hpp"
#include "video_api.hpp"

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {
// The core's sink, for the callbacks that take a plain function (text shaper, GPU renderer). One core
// runs per process: the engine's state is process-wide.
void (*core_log)(const char*) = nullptr;
void log_line(const char* line) { core_log(line); }

int option_int(const std::string& value, int fallback) {
  return value.empty() ? fallback : std::atoi(value.c_str());
}

std::string default_shot_path(const char* name) {
  char path[PATH_MAX];
  const std::string logical = std::string("/temp0/") + name + ".bmp";
  return host::resolve_path(logical.c_str(), path, sizeof path) ? path : "";
}
} // namespace

HostCore::HostCore(const Config& config)
    : gc_scheduler(config.clock_us), profiler(config.clock_us), config_(config) {
  core_log = config_.log;
  previous_step_us_ = now();
}

HostCore::~HostCore() { stop(); }

std::string HostCore::env_option(const char* name) {
  std::string variable = "ROA_";
  for (const char* c = name; *c; ++c) variable += *c == '-' ? '_' : static_cast<char>(std::toupper(*c));
  const char* value = std::getenv(variable.c_str());
  return value ? value : "";
}

// stdio rather than iostreams: those pull in locale support the PS5's libc does not have.
void HostCore::load_env_file(const char* path, void (*log)(const char* line)) {
  FILE* file = std::fopen(path, "rb");
  if (!file) return;
  for (char text[1024]; std::fgets(text, sizeof text, file);) {
    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
    log(("[ROA] option " + line).c_str());
  }
  std::fclose(file);
}

void HostCore::logf(const char* format, ...) {
  char line[1024];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(line, sizeof line, format, arguments);
  va_end(arguments);
  config_.log(line);
}

bool HostCore::start_engine(GlPresenter& presenter) {
  presenter_ = &presenter;
  const int width = config_.width, height = config_.height;
  backend_started_ = er_software_backend_init(width, height);
  if (!backend_started_ || !damage_tracker_install(width, height)) return false;
  // The presenter draws qualifying <Image layer> images itself; the option "layers" at 0 rasterizes them all.
  er_set_layers_enabled(option_int(option("layers"), 1) != 0 && presenter.layers_supported());
  // "gpu": 0 draws in software, 1 with the GPU renderer, 2 also waits for the GPU each frame to time it.
  // "gpu-mirror" draws with both and compares the two at each shot. The GPU renderer needs the drawing
  // ops in order, from one worker.
  const int gpu_flag = option_int(option("gpu"), config_.gpu_by_default ? 1 : 0);
  const bool mirroring = !gpu_flag && !option("gpu-mirror").empty();
  GpuRenderer::set_log(log_line);
  if (gpu_flag || mirroring) {
    gpu.set_sync(gpu_flag == 2);
    if (!gpu.start(width, height, mirroring ? GpuRenderer::Mode::mirror : GpuRenderer::Mode::draw)) {
      logf("[ROA] gpu renderer could not start: drawing in software");
      gpu.stop();
    }
  }
  const int wanted = option_int(option("render-workers"), 0);
  render_worker_count_ = render_workers::start(gpu.active() ? 1 : wanted > 0 ? wanted : render_workers::available_cpus());
  // A list of every drawing primitive per commit (canvas_trace.hpp), from the software rasterizer.
  if (const std::string trace = option("canvas-trace"); !trace.empty() && !gpu.active()) {
    tracing_ = canvas_trace::start(trace.c_str(), render_worker_count_);
    if (!tracing_) logf("[ROA] canvas trace: cannot write %s", trace.c_str());
  }
  logf("[ROA] render workers=%d of %d, cpus=%d, %s", render_worker_count_, render_workers::max_workers(),
       render_workers::available_cpus(), gpu.drawing() ? "gpu" : gpu.active() ? "gpu mirror" : "software");
  if (const int ms = option_int(option("slow-frame-ms"), 0); ms > 0 && ms <= 60000) slow_frame_us = ms * 1000u;
  if (const int lines = option_int(option("slow-frame-lines"), 0); lines > 0) stats.set_slow_line_limit(lines);
  // At 0, idle collections may run during motion.
  if (const std::string still = option("gc-still"); !still.empty()) gc_scheduler.wait_for_still(std::atoi(still.c_str()) != 0);
  if (const std::string text = option("input-script"); !text.empty()) {
    char error[160];
    if (script.parse(text.c_str(), error, sizeof error)) logf("[ROA] input script loaded");
    else logf("[ROA] input script ignored: %s", error);
  }
  return true;
}

bool HostCore::start_runtime() {
  if (network::start()) roa_start_images();
  if (!roa_start_sound()) logf("[ROA] sound could not start");
  ErRuntimeConfig config = {};
  config.screen_width = config_.width;
  config.screen_height = config_.height;
  config.screen_scale = 2;
  config.log = config_.react_log;
  config.max_stack_size = 1024 * 1024;
  config.memory_limit = kJsMemoryLimit;
  config.malloc_functions = js_heap_functions();
  config.install_host_globals = roa_install_host_api;
  runtime_started_ = er_runtime_init(&config);
  return runtime_started_;
}

bool HostCore::load(void (*register_assets)(), const char* font_dir, const char* source, std::size_t length,
                    const char* name) {
  register_assets();
  if (font_dir && *font_dir)
    text_shaper::install(font_dir, [](const char* line) {
      char tagged[512];
      std::snprintf(tagged, sizeof tagged, "[ROA] %s", line);
      log_line(tagged);
    });
  const bool ok = er_runtime_load_source(source, length, name);
  previous_step_us_ = now();
  return ok;
}

bool HostCore::load_file(void (*register_assets)(), const char* font_dir, const char* path) {
  FILE* file = std::fopen(path, "rb");
  if (!file) {
    logf("[ROA] no bundle at %s: %s", path, std::strerror(errno));
    return false;
  }
  bool ok = std::fseek(file, 0, SEEK_END) == 0;
  const long size = ok ? std::ftell(file) : -1;
  ok = size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
  const std::size_t length = ok ? static_cast<std::size_t>(size) : 0;
  // QuickJS's source parser requires a trailing NUL beyond the supplied length.
  std::vector<char> source(length + 1, '\0');
  if (ok) ok = std::fread(source.data(), 1, length, file) == length;
  std::fclose(file);
  return ok && load(register_assets, font_dir, source.data(), length, path);
}

void HostCore::stop() {
  if (stopped_) return;
  stopped_ = true;
  if (tracing_) canvas_trace::stop();
  // The players and the native services first, then the runtime that holds their handles, then the
  // renderers it drew with.
  video_layers::stop();
  roa_stop_images();
  archives::stop();
  browser_capture::stop();
  lan::stop();
  packages::stop();
  removals::stop();
  network::stop();
  if (runtime_started_) er_runtime_shutdown();
  text_shaper::shutdown();
  roa_stop_sound();
  gpu.stop();
  render_workers::stop();
  if (backend_started_) er_software_backend_destroy();
}

bool HostCore::call_input(const char* name, int argc, JSValue* argv) {
  gc_scheduler.input(now());
  er_perf_phase_begin(ER_PERF_PHASE_JS);
  JSContext* ctx = er_runtime_context();
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, global, name);
  JSValue result = JS_Call(ctx, fn, global, argc, argv);
  const bool ok = !JS_IsException(result);
  if (!ok) {
    JSValue error = JS_GetException(ctx);
    const char* message = JS_ToCString(ctx, error);
    logf("[ROA] input exception: %s", message ? message : "unknown");
    if (message) JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, error);
  }
  JS_FreeValue(ctx, result);
  for (int i = 0; i < argc; ++i) JS_FreeValue(ctx, argv[i]);
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, global);
  er_perf_phase_end(ER_PERF_PHASE_JS);
  return ok;
}

bool HostCore::dispatch(const char* action) {
  JSValue value = JS_NewString(er_runtime_context(), action);
  return call_input("__roaDispatch", 1, &value);
}

bool HostCore::touch(bool down, float x, float y) {
  JSContext* ctx = er_runtime_context();
  JSValue args[2] = {down ? JS_NewFloat64(ctx, x) : JS_NULL, down ? JS_NewFloat64(ctx, y) : JS_NULL};
  return call_input("__roaTouch", 2, args);
}

bool HostCore::screen_touch(bool down, float x, float y) {
  JSContext* ctx = er_runtime_context();
  JSValue args[2] = {down ? JS_NewFloat64(ctx, x) : JS_NULL, down ? JS_NewFloat64(ctx, y) : JS_NULL};
  return call_input("__roaScreenTouch", 2, args);
}

std::string HostCore::call_js(const char* name, const char* arg) {
  JSContext* ctx = er_runtime_context();
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, global, name);
  JSValue argument = arg ? JS_NewString(ctx, arg) : JS_UNDEFINED;
  JSValue result = JS_Call(ctx, fn, global, arg ? 1 : 0, &argument);
  std::string text;
  if (JS_IsException(result)) {
    JSValue error = JS_GetException(ctx);
    const char* message = JS_ToCString(ctx, error);
    text = std::string("error: ") + (message ? message : "JS exception");
    if (message) JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, error);
  } else {
    JSValue json = JS_IsString(result) ? JS_DupValue(ctx, result) : JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    if (const char* value = JS_ToCString(ctx, json)) {
      text = value;
      JS_FreeCString(ctx, value);
    }
    JS_FreeValue(ctx, json);
  }
  JS_FreeValue(ctx, result);
  JS_FreeValue(ctx, argument);
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, global);
  return text;
}

bool HostCore::begin_frame() {
  enter("input");
  const char* line = stats.start_frame(now());
  if (line) logf("[ROA] %s", line);
  // Input and JavaScript commit layout only; the commit after them paints the frame once.
  er_set_raster_deferred(true);
  return line != nullptr;
}

bool HostCore::save_shot(const char* path) {
  // With the GPU drawing, the software framebuffer holds only what it left to the CPU.
  return gpu.drawing() ? gpu.save_screen(*presenter_, path) : save_screenshot(path);
}

bool HostCore::play_script(std::uint64_t now_ms) {
  const char* action = script.next(now_ms);
  if (!action) return true;
  logf("[ROA] script: %s", action);
  if (!std::strcmp(action, "quit")) {
    quit_requested_ = true;
  } else if (VirtualPad::is_step(action)) {
    char error[160];
    if (!virtual_pad.apply(action, now_ms, error, sizeof error)) logf("[ROA] script: %s", error);
  } else if (!std::strncmp(action, "shot:", 5)) {
    const std::string path = config_.shot_path ? config_.shot_path(action + 5) : default_shot_path(action + 5);
    const bool saved = !path.empty() && save_shot(path.c_str());
    logf("[ROA] shot %s: %s", path.c_str(), saved ? "saved" : std::strerror(errno));
    if (saved && gpu.active() && !gpu.drawing())
      logf("[ROA] %s", gpu.compare(er_software_framebuffer(), *presenter_, path.substr(0, path.size() - 4)).c_str());
  } else if (!std::strncmp(action, "touch:", 6)) {
    float x = 0, y = 0;
    return touch(std::sscanf(action + 6, "%f,%f", &x, &y) == 2, x, y);
  } else if (!std::strncmp(action, "screen:", 7)) {
    float x = 0, y = 0;
    return screen_touch(std::sscanf(action + 7, "%f,%f", &x, &y) == 2, x, y);
  } else {
    return dispatch(action);
  }
  return true;
}

bool HostCore::deliver_pad(GamepadState state, std::uint64_t now_ms) {
  // What a tool holds presses like the controller: the buttons' actions, once (directions do not repeat).
  for (std::uint32_t pressed = virtual_pad.merge(state, now_ms), i = 0; pressed; pressed >>= 1, ++i) {
    const char* held = pressed & 1 ? VirtualPad::action(i) : nullptr;
    if (held && !dispatch(held)) return false;
  }
  roa_set_gamepad(state);
  return true;
}

double HostCore::wall_elapsed_ms() {
  const std::int64_t step_us = now();
  const double elapsed = std::min(50.0, (step_us - previous_step_us_) / 1000.0);
  previous_step_us_ = step_us;
  return elapsed;
}

bool HostCore::advance(double elapsed_ms) {
  stats.lap(FrameStats::input, now());
  enter("js");
  er_perf_phase_begin(ER_PERF_PHASE_JS);
  er_runtime_pump();
  const bool stepped = roa_frame(er_runtime_context(), elapsed_ms);
  er_perf_phase_end(ER_PERF_PHASE_JS);
  if (!stepped) {
    logf("[ROA] frame callback exception");
    return false;
  }
  elapsed_ms_ = elapsed_ms;
  er_set_raster_deferred(false);
  enter("commit");
  if (tracing_) canvas_trace::commit_begin();
  er_commit();
  if (tracing_) canvas_trace::commit_end();
  if (*er_runtime_last_error()) return false;
  redrawn_ = gpu.active() && gpu.replay();
  stats.lap(FrameStats::update, now());
  return true;
}

HostCore::Presented HostCore::present(int surface_width, int surface_height, std::int64_t frame_us, bool allow_still) {
  enter("present");
  Presented result;
  repainted_ = damage_tracker_area();
  if (surface_width <= 0 || surface_height <= 0) {
    // A minimized window: the damage waits for a surface to draw it to.
    result.still = true;
    stats.lap(FrameStats::present, now());
    return result;
  }
  er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
  // The layers go to the presenter before it compares them with the ones on screen.
  int layer_count = 0;
  const ERLayer* layers = er_get_layers(&layer_count);
  presenter_->set_layers({layers, static_cast<std::size_t>(layer_count)});
  video_layers::update(now(), frame_us);
  // After draw_texture() the software damage says nothing: the GPU renderer reports its own redraws.
  result.still = allow_still && !redrawn_ &&
                 presenter_->unchanged(damage_tracker_rects(), damage_tracker_moves(), surface_width, surface_height);
  if (!result.still) {
    result.ok = gpu.drawing() ? presenter_->draw_texture(gpu.texture(), surface_width, surface_height)
                              : presenter_->draw(er_software_framebuffer(), damage_tracker_rects(),
                                                 damage_tracker_moves(), surface_width, surface_height);
    const GlPresenter::Timing& timing = presenter_->last_timing();
    stats.note_present(timing.upload_us, timing.draw_us, timing.upload_bytes);
  }
  er_perf_phase_end(ER_PERF_PHASE_PRESENT);
  damage_tracker_clear();
  stats.lap(FrameStats::present, now());
  enter("swap");
  return result;
}

void HostCore::end_frame() {
  stats.lap(FrameStats::swap, now());
  enter("gc");
  if (const char* line = gc_scheduler.frame(JS_GetRuntime(er_runtime_context()), now(), repainted_,
                                            static_cast<std::size_t>(config_.width) * config_.height))
    logf("[ROA] %s", line);
  enter("embedded");
  tick_carry_ms_ += std::min(elapsed_ms_, 50.0);
  const auto tick_ms = static_cast<std::uint32_t>(tick_carry_ms_);
  tick_carry_ms_ -= tick_ms;
  embedded_renderer_tick(tick_ms);
  if (const char* line = stats.end_frame(slow_frame_us)) logf("[ROA] %s", line);
  if (config_.log_image_stats && now() >= next_image_stats_us_) {
    next_image_stats_us_ = now() + 2000000;
    if (std::string line = images::stats(); line != image_stats_) {
      logf("[ROA] images: %s", line.c_str());
      image_stats_ = std::move(line);
    }
  }
}
