// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Independent React proof: PS5 lifecycle/display/input, software UI and GL texture.

#include "network.hpp"
#include "archives.hpp"
#include "thread_name.hpp"
#include "app_config.hpp"
#include "async_log.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "core/input.hpp"
#include "gl_presenter.hpp"
#include "image_loader.hpp"
#include "host_api.hpp"
#include "host_core.hpp"
#include "host_platform.hpp"
#include "actions.hpp"
#include "input_script.hpp"
#include "payload_loader.hpp"
#include "video_api.hpp"
#include "layouts.h"
#include "video_output.hpp"
#include "video_frame.hpp"
#include "filesystem_access.hpp"
#include <unistd.h>
#include <EGL/egl.h>
#include <ps5_opengl_display_modes.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
std::uint64_t sceKernelReadTsc(void);
std::uint64_t sceKernelGetTscFrequency(void);
void hui_heap_capacity(std::size_t* size, std::size_t* flexible_before, std::size_t* flexible_after);
void hui_heap_stats(std::size_t* live_bytes, std::size_t* peak_bytes, std::size_t* blocks, std::size_t* failures);
}

// Extraction steps go to the console log, so the crash log keeps the last one before a fault.
bool archives::list_directory(const char* path, void (*visit)(const char*, void*), void* user) { return host::read_dir(path, visit, user); }
void archives::trace(const char* step, const char* detail) { async_log::write("[ROA] archive %s %s", step, detail); }
// The extraction's write ring: larger on larger heaps, and none (inline writes, as before) unless the
// heap still has room for the 64 MiB decoder budget the preflight admits, the ring, and a margin.
std::size_t archives::pipeline_bytes() {
  constexpr std::size_t MiB = 1024 * 1024;
  std::size_t size = 0, live = 0, peak = 0, blocks = 0, failures = 0;
  hui_heap_capacity(&size, nullptr, nullptr);
  hui_heap_stats(&live, &peak, &blocks, &failures);
  const std::size_t ring = size >= 256 * MiB ? 32 * MiB : size >= 192 * MiB ? 16 * MiB : 8 * MiB;
  return live + 64 * MiB + ring + 16 * MiB <= size ? ring : 0;
}
// The image caches drop to their low budgets for an in-process extraction (the render thread trims
// within a frame or two; images on screen stay), then the heap must still hold the decoder budget and
// the margin. pipeline_bytes() sizes the ring from what is left after the trim.
bool archives::hold_memory(bool hold) {
  images::hold_memory(hold);
  if (!hold) return true;
  for (int i = 0; i < 25 && images::memory().decoded > images::kLowCacheBytes; ++i) usleep(10'000);
  constexpr std::size_t MiB = 1024 * 1024;
  std::size_t size = 0, live = 0, peak = 0, blocks = 0, failures = 0;
  hui_heap_capacity(&size, nullptr, nullptr);
  hui_heap_stats(&live, &peak, &blocks, &failures);
  const bool room = !size || live + 64 * MiB + 16 * MiB <= size;
  async_log::write("[ROA] extraction memory: heap=%zu/%zuMiB img decoded=%zuKiB room=%d", live >> 20, size >> 20,
                   images::memory().decoded >> 10, room);
  return room;
}
bool archives::downloading() { return network::downloading(); }
// rar-extract.elf goes to the payload loader as the package installer does, and reports on the socket.
int archives::start_rar_worker(const std::string& request, std::string& error) {
  const int socket = payload_loader::send("rar-extract.elf", request, 5'000'000, "Archives.extract", error);
  if (socket >= 0) payload_loader::receive_timeout(socket, 250'000);
  return socket;
}
long archives::read_rar_worker(int stream, char* data, std::size_t size) {
  const int count = payload_loader::receive(stream, data, size);
  return count < 0 ? -1 : count;
}
void archives::cancel_rar_worker(int stream) { payload_loader::write(stream, "cancel\n", 7); }
void archives::close_rar_worker(int stream) { payload_loader::close(stream); }

namespace {
// Startup steps log their time since main(), to show where launch time goes.
std::int64_t launch_us = 0;
long long since_launch_ms() { return (hui::sys::monotonic_us() - launch_us) / 1000; }

void log_memory() {
  std::size_t live = 0, peak = 0, blocks = 0, failures = 0, size = 0;
  hui_heap_stats(&live, &peak, &blocks, &failures);
  hui_heap_capacity(&size, nullptr, nullptr);
  const images::Memory image = images::memory();
  async_log::write("[ROA] memory: heap=%zu/%zuMiB peak=%zuMiB failed=%zu img encoded=%zuKiB decoded=%zuKiB",
                   live >> 20, size >> 20, peak >> 20, failures, image.encoded >> 10, image.decoded >> 10);
  async_log::write("[ROA] images: %s", images::stats().c_str());
}

// The video layers' lines with a frame summary: upload, upscaling, and the process's CPU.
void log_videos(GlPresenter& presenter, const std::string& videos) {
  const std::uint64_t bytes = presenter.video().take_upload_bytes();
  const std::uint32_t us = presenter.video().take_upload_us();
  // The whole process's CPU time since the last line, as cores busy: decoding threads included.
  static std::int64_t cpu_before = 0, wall_before = 0;
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const std::int64_t cpu_us = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * std::int64_t{1000000} +
                              usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
  const std::int64_t wall_us = hui::sys::monotonic_us();
  const double cores = wall_before && wall_us > wall_before ? double(cpu_us - cpu_before) / (wall_us - wall_before) : 0;
  cpu_before = cpu_us;
  wall_before = wall_us;
  async_log::write("[ROA] video upload %.1f MiB in %.1f ms, process cpu %.2f cores", bytes / 1048576.0, us / 1000.0, cores);
  if (const std::string upscale = presenter.video().take_upscale_log(); !upscale.empty())
    async_log::write("[ROA] video upscale %s", upscale.c_str());
  std::size_t start = 0;
  while (start < videos.size()) {
    const std::size_t end = videos.find('\n', start);
    async_log::write("[ROA] %s", videos.substr(start, end - start).c_str());
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

// A frozen screen on a tester's console says where the render loop hangs: past three seconds without
// a turn, the log names the loop's phase, the native call it is in and how far script got meanwhile
// (QuickJS polls its interrupt handler while it runs, so rising js_ticks with no call is a script loop).
std::atomic<std::int64_t> loop_beat{0};
std::atomic<const char*> loop_phase{"start"};
std::atomic<std::uint64_t> js_ticks{0};
std::atomic<bool> watching{true};

void beat(const char* phase) {
  loop_phase.store(phase, std::memory_order_relaxed);
  loop_beat.store(hui::sys::monotonic_us(), std::memory_order_relaxed);
}

void* watch_loop(void*) {
  name_thread("stall-watch");
  std::int64_t stalled_at = 0, next_report = 0;
  std::uint64_t ticks = 0;
  while (watching) {
    usleep(500'000);
    const std::int64_t now = hui::sys::monotonic_us(), last = loop_beat.load(std::memory_order_relaxed);
    if (stalled_at && last != stalled_at) {
      async_log::write("[ROA] stall: loop turned again after %lldms", static_cast<long long>((last-stalled_at)/1000));
      stalled_at = 0;
    }
    if (now - last < 3'000'000) continue;
    if (!stalled_at) { stalled_at = last; next_report = now; ticks = js_ticks; }
    if (now < next_report) continue;
    const char* call = host::current_call.load();
    const std::uint64_t seen = js_ticks;
    async_log::write("[ROA] stall: loop stuck %llds phase=%s call=%s js_ticks=+%llu",
                     static_cast<long long>((now-last)/1000000), loop_phase.load(std::memory_order_relaxed),
                     call ? call : "none", static_cast<unsigned long long>(seen-ticks));
    ticks = seen; next_report = now + 5'000'000;
  }
  return nullptr;
}

// Unused decoded images kept for scrolling back. Browsing peaks near 110 MiB live with 24 MiB of them
// and a 64-connection download, so a 128 MiB heap keeps 24 MiB; a 256 MiB heap holds 64 MiB (about 160
// grid covers, or a hero and 140 covers) and still leaves over 80 MiB free at that peak.
std::size_t image_cache_bytes() {
  std::size_t heap = 0;
  hui_heap_capacity(&heap, nullptr, nullptr);
  constexpr std::size_t MiB = 1024 * 1024;
  return heap >= 256 * MiB ? 64 * MiB : heap >= 192 * MiB ? 40 * MiB : 24 * MiB;
}

// A file in the app folder's dev/ directory. After console filesystem elevation /app0 is only a
// logical path, so it is resolved like the native API's paths.
std::string dev_path(const char* name) {
  char path[PATH_MAX];
  return host::resolve_path((std::string("/app0/dev/") + name).c_str(), path, sizeof path) ? path : "";
}

// A development option: the text of dev/<name>.txt, without its trailing line break, or empty. A test
// deploy adds the files; a build never has a dev folder.
std::string dev_option(const char* name) {
  FILE* file = std::fopen(dev_path((std::string(name) + ".txt").c_str()).c_str(), "rb");
  if (!file) return "";
  char text[4096] = {};
  text[std::fread(text, 1, sizeof text - 1, file)] = '\0';
  std::fclose(file);
  std::string value = text;
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}

std::string dev_shot_path(const char* name) { return dev_path((std::string(name) + ".bmp").c_str()); }

constexpr std::int64_t duration_us = static_cast<std::int64_t>(ROA_TIMEOUT) * 1000000;

void react_log(const char* line) { async_log::write("[REACT] %s", line); }

// The smallest layout at least as tall as the console's output, else the largest. An unreadable
// output picks the smallest.
const RoaLayout& choose_layout() {
  if (roa_layout_count == 1) return roa_layouts[0];
  const int lines = video_output::output_lines();
  for (unsigned i = 0; i < roa_layout_count; ++i)
    if (roa_layouts[i].height >= lines) return roa_layouts[i];
  return roa_layouts[roa_layout_count - 1];
}

// Live commands for a running title: a test machine overwrites dev/commands.txt with lines
// "<sequence> <steps>" (InputScript syntax); about once a second the host runs the steps of every line
// whose sequence number is new. Active only when the app folder has a dev directory, which a build never
// creates. Lines present at launch are skipped.
class LiveCommands {
public:
  void start() {
    struct stat info;
    enabled_ = stat(dev_path("").c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    if (enabled_) read(false);
  }

  // Appends new commands to `script` once it has finished its previous steps.
  void poll(std::int64_t now_us, InputScript& script) {
    if (!enabled_ || now_us < next_poll_us_) return;
    next_poll_us_ = now_us + 1000000;
    read(true);
    if (pending_.empty() || !script.done()) return;
    char error[160];
    if (!script.parse(pending_.c_str(), error, sizeof error)) async_log::write("[ROA] command ignored: %s", error);
    pending_.clear();
  }

private:
  void read(bool queue) {
    FILE* file = std::fopen(dev_path("commands.txt").c_str(), "rb");
    if (!file) return;
    char text[4096] = {};
    text[std::fread(text, 1, sizeof text - 1, file)] = '\0';
    std::fclose(file);
    for (char* line = std::strtok(text, "\n"); line; line = std::strtok(nullptr, "\n")) {
      char* steps = nullptr;
      const unsigned long long sequence = std::strtoull(line, &steps, 10);
      if (steps == line || sequence <= last_sequence_) continue;
      steps += std::strspn(steps, " \t");
      last_sequence_ = sequence;
      if (!queue) continue;
      async_log::write("[ROA] command: %s", steps);
      pending_ += steps;
      pending_ += ' ';
    }
  }

  bool enabled_ = false;
  unsigned long long last_sequence_ = 0;
  std::int64_t next_poll_us_ = 0;
  std::string pending_;
};

// The engine reads this clock around every blit, thousands of times in a full-screen frame:
// clock_gettime made the frame log cost about 10 ms there, the time-stamp counter costs nothing.
std::uint64_t tsc_per_us = 1;
std::uint32_t perf_clock() { return static_cast<std::uint32_t>(sceKernelReadTsc() / tsc_per_us); }

// Indexed like kButtonNames.
constexpr hui::Action kButtonActions[] = {
  hui::Action::up, hui::Action::down, hui::Action::left, hui::Action::right,
  hui::Action::confirm, hui::Action::back, hui::Action::north, hui::Action::west,
  hui::Action::page_prev, hui::Action::page_next, hui::Action::jump_prev, hui::Action::jump_next,
  hui::Action::l3, hui::Action::r3, hui::Action::menu, hui::Action::touch,
};
static_assert(std::size(kButtonActions) == std::size(kButtonNames));

// Buttons delivered to the app as actions; the D-pad and left stick come through input.nav.
constexpr std::pair<hui::Action, HostAction> kExtraActions[] = {
  {hui::Action::confirm, HostAction::confirm}, {hui::Action::back, HostAction::back},
  {hui::Action::page_prev, HostAction::l1}, {hui::Action::page_next, HostAction::r1},
  {hui::Action::jump_prev, HostAction::l2}, {hui::Action::jump_next, HostAction::r2},
  {hui::Action::north, HostAction::triangle}, {hui::Action::west, HostAction::square},
  {hui::Action::menu, HostAction::options},
};

GamepadState gamepad_state(const hui::InputFrame& input) {
  GamepadState state;
  state.connected = input.connected;
  state.left_x = input.stick_x; state.left_y = input.stick_y;
  state.right_x = input.stick2_x; state.right_y = input.stick2_y;
  state.l2 = input.trigger_l; state.r2 = input.trigger_r;
  for (std::size_t i = 0; i < std::size(kButtonActions); ++i)
    if (input.is_held(kButtonActions[i])) state.buttons |= 1u << i;
  return state;
}

const char* nav_action(hui::Direction direction) {
  switch (direction) {
    case hui::Direction::up: return action_name(HostAction::up);
    case hui::Direction::down: return action_name(HostAction::down);
    case hui::Direction::left: return action_name(HostAction::left);
    case hui::Direction::right: return action_name(HostAction::right);
    default: return nullptr;
  }
}

void log_line(const char* line) { async_log::write("%s", line); }

HostCore::Config core_config(const RoaLayout& layout) {
  HostCore::Config config;
  config.width = layout.width;
  config.height = layout.height;
  config.clock_us = hui::sys::monotonic_us;
  config.log = log_line;
  config.option = dev_option;
  config.shot_path = dev_shot_path;
  config.react_log = react_log;
  config.phase = beat;
  config.gpu_by_default = true;
  // With the memory line instead (log_memory).
  config.log_image_stats = false;
  return config;
}

// Display.setRefreshRate: the rate can change only while EGL is terminated (ps5_opengl_display_modes.h),
// so the presenter's GL objects go with the context and are made again. The engine framebuffer is CPU
// memory and survives; the presenter's first draw uploads all of it.
bool switch_refresh_rate(hui::ps5::Display& display, GlPresenter& presenter, const RoaLayout& layout, int hz) {
  const std::int64_t start = hui::sys::monotonic_us();
  presenter.release();
  display.close();
  if (!eglSetDisplayRefreshPS5(eglGetDisplay(EGL_DEFAULT_DISPLAY), hz))
    async_log::write("[ROA] refresh %d Hz refused: 0x%x", hz, eglGetError());
  const bool ok = display.open(layout.surface_width, layout.surface_height) && presenter.init(layout.width, layout.height);
  async_log::write("[ROA] refresh switch to %d Hz ok=%d in %lldms", hz, ok,
                   static_cast<long long>((hui::sys::monotonic_us() - start) / 1000));
  return ok;
}

// Owned resources are released while the context is valid; closing the title
// happens after this scope, through the platform's lifecycle API.
bool run_proof() {
  hui::ps5::Display display;
  hui::ps5::Pad pad;
  GlPresenter presenter;
  hui::InputTracker tracker;
  hui::PadSample samples[64];
  unsigned summaries = 0;
  bool runtime = false;
  // Accepted only before Display::open initializes EGL. A display without 120 Hz stays at 60.
  if (ROA_REFRESH_RATE != 60 && !eglSetDisplayRefreshPS5(eglGetDisplay(EGL_DEFAULT_DISPLAY), ROA_REFRESH_RATE))
    async_log::write("[ROA] refresh %d Hz refused: 0x%x", ROA_REFRESH_RATE, eglGetError());
  const RoaLayout& layout = choose_layout();
  const int width = layout.width, height = layout.height;
  // The engine, its services and the frame steps every host shares; the GPU renderer draws the UI unless
  // a test deploy's dev/gpu.txt holds 0 (2 also times the GPU's work).
  HostCore core(core_config(layout));
  bool ok = display.open(layout.surface_width, layout.surface_height);
  async_log::write("[ROA] display=%d at %lldms", ok, since_launch_ms());
  if (ok) {
    ok = presenter.init(width, height);
    async_log::write("[ROA] presenter=%d", ok);
  }
  if (ok) {
    // A test deploy may upload video as textures instead of storage buffers with dev/video-upload.txt "textures".
    if (dev_option("video-upload") == "textures") presenter.set_video_upload(false);
    // And dump the upscaler's input and output at its Nth frame with dev/upscale-dump.txt "N".
    if (const int frame = std::atoi(dev_option("upscale-dump").c_str()); frame > 0)
      presenter.video().dump_upscale_at(dev_path("upscale"), std::uint64_t(frame));
    async_log::write("[ROA] video upload=%s",
                     presenter.video().upload_mode() == GlVideo::Upload::storage ? "storage" : "textures");
    ok = core.start_engine(presenter);
    async_log::write("[ROA] software framebuffer=%d %dx%d", ok, width, height);
  }
  // The pad opens before the bundle runs: it also initializes the user service
  // that the users, notification and browser calls need.
  if (ok) {
    async_log::write("[ROA] pad=%d at %lldms", pad.open(), since_launch_ms());
    host_platform_set_pad(&pad);
  }
  if (ok) {
    images::set_cache_bytes(image_cache_bytes());
    images::set_heap_probe([] {
      images::Heap heap;
      std::size_t peak = 0, blocks = 0, failures = 0;
      hui_heap_stats(&heap.live, &peak, &blocks, &failures);
      hui_heap_capacity(&heap.size, nullptr, nullptr);
      return heap;
    });
    ok = runtime = core.start_runtime();
    async_log::write("[ROA] runtime=%d at %lldms", ok, since_launch_ms());
    // A test deploy samples JavaScript with dev/profile.txt holding the interval in microseconds.
    if (const std::string profile = ok ? dev_option("profile") : ""; !profile.empty()) {
      const int interval = std::atoi(profile.c_str());
      core.profiler.start(JS_GetRuntime(er_runtime_context()), interval > 0 ? interval : 1000);
      async_log::write("[ROA] profiling JavaScript every %d us", interval > 0 ? interval : 1000);
    }
  }
  if (ok) {
    char fonts[256];
    if (!host::resolve_path("/app0/fonts", fonts, sizeof fonts)) fonts[0] = '\0';
    ok = core.load(layout.register_assets, fonts, layout.bundle, layout.bundle_length, "app.jsx.bundle");
    async_log::write("[ROA] bundle=%d gc_accounting=%d at %lldms", ok, er_runtime_gc_accounting_ok(), since_launch_ms());
  }
  if (ok) {
    // Input failure is logged; timeout still allows the display-only proof to end.
    async_log::write("[ROA] timeout=%ds after first frame", ROA_TIMEOUT);
    std::int64_t first_present = 0, previous = hui::sys::monotonic_us(), last_vblank_us = previous;
    // Replaced by the accepted mode after the first frame at a rate: a display without 120 Hz stays at 60.
    int refresh_hz = ROA_REFRESH_RATE;
    bool mode_known = false;
    std::uint64_t frames = 0;
    bool touched = false;
    LiveCommands commands;
    commands.start();
    beat("start");
    JS_SetInterruptHandler(JS_GetRuntime(er_runtime_context()),
                           [](JSRuntime*, void*) { js_ticks.fetch_add(1, std::memory_order_relaxed); return 0; }, nullptr);
    pthread_t watcher;
    const bool watcher_started = pthread_create(&watcher, nullptr, watch_loop, nullptr) == 0;
    tsc_per_us = std::max<std::uint64_t>(sceKernelGetTscFrequency() / 1000000, 1);
    er_perf_set_clock(perf_clock);
    while (ok) {
      const std::int64_t now = hui::sys::monotonic_us();
      if (now <= 0 || (duration_us > 0 && first_present && now - first_present >= duration_us)) break;
      if (core.begin_frame()) {
        // Every fifth summary, about ten seconds apart.
        if (++summaries % 5 == 1) log_memory();
        if (const std::string videos = video_layers::summary(); !videos.empty()) log_videos(presenter, videos);
      }
      const auto count = pad.read(samples);
      const auto input = tracker.update(std::span<const hui::PadSample>(samples, count), now);
      pad.tick(static_cast<float>(now - previous) / 1000000.0f);
      host_platform_tick(now);
      if (const char* direction = nav_action(input.nav)) ok = core.dispatch(direction);
      for (const auto& [button, action] : kExtraActions) {
        if (ok && input.is_pressed(button)) ok = core.dispatch(action_name(action));
      }
      // Each frame the finger is down, and once when it lifts.
      float touch_x = 0, touch_y = 0;
      const bool touch_down = host_platform_read_touch(&touch_x, &touch_y);
      if (ok && (touch_down || touched)) ok = core.touch(touch_down, touch_x, touch_y);
      touched = touch_down;
      commands.poll(now, core.script);
      const auto now_ms = static_cast<std::uint64_t>(now / 1000);
      ok = ok && core.play_script(now_ms);
      if (core.quit_requested()) break;
      ok = ok && core.deliver_pad(gamepad_state(input), now_ms);
      if (!ok) break;
      // The display shows a new frame every vblank, so motion advances by whole vblanks, not by the
      // loop's jittery wall-clock interval.
      const std::int64_t vblank_us = 1000000 / refresh_hz;
      const std::int64_t vblanks = std::max<std::int64_t>(1, (now - previous + vblank_us / 2) / vblank_us);
      if (!core.advance(vblanks * 1000.0 / refresh_hz)) {
        ok = false;
        break;
      }
      // A frame that changed nothing is not presented: the screen keeps showing the last one, and the
      // loop sleeps to the next vblank instead of waiting in the driver, which costs CPU.
      const HostCore::Presented presented = core.present(display.width(), display.height(), vblank_us, first_present != 0);
      ok = presented.ok;
      if (presented.still) {
        const std::int64_t wake = last_vblank_us + (hui::sys::monotonic_us() - last_vblank_us + vblank_us) / vblank_us * vblank_us;
        const std::int64_t left = wake - hui::sys::monotonic_us();
        if (left > 0) usleep(static_cast<useconds_t>(left));
      } else {
        ok = ok && display.swap();
        last_vblank_us = hui::sys::monotonic_us();
      }
      if (!ok) break;
      core.end_frame();
      if (!first_present) {
        first_present = hui::sys::monotonic_us();
        hui::sys::hide_splash_screen();
        async_log::write("[ROA] first frame presented at %lldms", since_launch_ms());
      }
      const bool first_at_mode = !mode_known;
      if (first_at_mode) {
        // The accepted mode is known only once a frame has been presented.
        mode_known = true;
        EGLint mode_width = 0, mode_height = 0, mode_hz = 0;
        if (eglGetDisplayModePS5(eglGetDisplay(EGL_DEFAULT_DISPLAY), &mode_width, &mode_height, &mode_hz)) {
          async_log::write("[ROA] display mode %dx%d at %d Hz", mode_width, mode_height, mode_hz);
          if (mode_hz > 0) refresh_hz = mode_hz;
        }
        roa_set_refresh_rate(refresh_hz);
      }
      previous = now;
      // The first frame at a mode may wait out the TV's resync in its swap; that wait is not motion.
      if (first_at_mode) previous = hui::sys::monotonic_us();
      ++frames;
      if (roa_exit_requested()) break;
      if (const int hz = roa_take_refresh_request(); hz && core.gpu.drawing()) {
        // Reopening EGL would lose the GPU renderer's picture, and the engine repaints only what changes.
        async_log::write("[ROA] refresh %d Hz: not while the GPU renderer draws; set it in app.json", hz);
      } else if (hz) {
        ok = switch_refresh_rate(display, presenter, layout, hz);
        refresh_hz = hz;
        mode_known = false;
        // Nor is the reopen itself.
        previous = hui::sys::monotonic_us();
      }
    }
    async_log::write("[ROA] loop ended ok=%d frames=%llu", ok, static_cast<unsigned long long>(frames));
    watching = false;
    if (watcher_started) pthread_join(watcher, nullptr);
    if (core.profiler.running()) {
      core.profiler.stop();
      const bool saved = core.profiler.write(dev_path("profile.folded").c_str());
      async_log::write("[ROA] profile: %llu samples, saved=%d", static_cast<unsigned long long>(core.profiler.samples()), saved);
    }
  }
  if (!ok && runtime) async_log::write("[ROA] error=%s", er_runtime_last_error());
  core.stop();
  host_platform_set_pad(nullptr);
  pad.close();
  presenter.release();
  display.close();
  return ok;
}

void* render_thread(void*) {
  name_thread("render");
  hui::sys::log("[ROA] render thread started, 8 MiB stack, at %lldms", since_launch_ms());
  if (!async_log::start()) hui::sys::log("[ROA] log writer thread failed; logging synchronously");
  const bool ok = run_proof();
  async_log::stop();
  hui::sys::log("[ROA] cleanup complete, result=%d; requesting title closure", ok);
  hui::sys::quit();
  return nullptr;
}
} // namespace

int main() {
  hui::sys::log("[ROA] %s (%s)", ROA_NAME, ROA_TITLE);
  std::size_t heap = 0, flexible_before = 0, flexible_after = 0;
  hui_heap_capacity(&heap, &flexible_before, &flexible_after);
  if (heap)
    hui::sys::log("[ROA] heap: %zu MiB (flexible memory free %zu MiB before it, %zu MiB after)",
                  heap >> 20, flexible_before >> 20, flexible_after >> 20);
  else
    hui::sys::log("[ROA] heap: none, system malloc (flexible memory free %zu MiB)", flexible_before >> 20);
  launch_us = hui::sys::monotonic_us();
#if ROA_VIDEO
  video::preload_platform_decoder();
#endif
  initialize_filesystem_access();
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (!result) {
    result = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
    pthread_t thread;
    if (!result) result = pthread_create(&thread, &attributes, render_thread, nullptr);
    pthread_attr_destroy(&attributes);
  }
  if (result) {
    hui::sys::log("[ROA] thread creation failed=%d", result);
    hui::sys::quit();
  }
  // Native titles must not return to CRT exit(); the render thread owns closure.
  hui::sys::park();
  return 0;
}
