// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The Nintendo Switch host: a homebrew NRO that runs the engine and draws it with the GPU renderer
// (gpu_renderer.hpp) through Mesa's OpenGL ES 3.2, at the layout's size, which the system scales to the
// screen (720p handheld, 1080p docked).
//
// The app is laid out twice and the host picks the screen's own resolution at launch: 1280x720 on the
// console's screen, 1920x1080 on a TV. Docking or undocking later keeps it, and the system scales the
// frames. The bundles come from the NRO's RomFS (romfs:/<width>x<height>/app.bundle.js, romfs:/fonts/), so
// one nxlink push deploys everything (tools/switch.py). The app's data lives on the SD card:
//   sdmc:/switch/<app>/sandbox/{app0,download0,temp0}
// and sdmc:/switch/<app>/options.env, KEY=VALUE lines set in the environment before boot, as on Android.
// Launched by nxlink, stdout goes back to it.
#include "host_platform.hpp"
#include "network.hpp"
#include "image_loader.hpp"
#include "archives.hpp"
#include "packages.hpp"
#include "removals.hpp"
#include "browser_capture.hpp"
#include "lan.hpp"
#include "gl_api.hpp"

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
void er_register_assets_1920x1080(void);
void er_register_assets_1280x720(void);
}
#include "actions.hpp"
#include "damage_tracker.hpp"
#include "frame_stats.hpp"
#include "gl_presenter.hpp"
#include "gpu_renderer.hpp"
#include "host_api.hpp"
#include "input_script.hpp"
#include "js_heap.hpp"
#include "render_workers.hpp"
#include "screenshot.hpp"
#include "text_shaper.hpp"
#include "video_api.hpp"

#include <netinet/in.h> // nxlink.h's in_addr
#include <switch.h>
#include <EGL/egl.h>
#include <sys/stat.h>
#include <algorithm>
#include <iterator>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct Layout {
  int width, height;
  void (*register_assets)();
  const char* bundle;
};
constexpr Layout kDocked{1920, 1080, er_register_assets_1920x1080, "romfs:/1920x1080/app.bundle.js"};
constexpr Layout kHandheld{1280, 720, er_register_assets_1280x720, "romfs:/1280x720/app.bundle.js"};
Layout layout = kDocked;
int width = kDocked.width, height = kDocked.height;
// Clocks for the app's run, restored on exit. By default the CPU runs at 1020 MHz, and handheld the GPU
// at 384-460 MHz and memory at 1331 MHz: a scroll redraws all 1920x1080 pixels each frame, and the GPU
// stalled the presenter for up to 80 ms. These are the console's own boost and docked rates.
struct Clock {
  PcvModuleId module;
  u32 hz;
  const char* name;
};
constexpr Clock kBoost[] = {
  {PcvModuleId_CpuBus, 1785000000, "cpu"},
  {PcvModuleId_GPU, 768000000, "gpu"},
  {PcvModuleId_EMC, 1600000000, "memory"},
};

std::int64_t now_us() { return static_cast<std::int64_t>(armTicksToNs(armGetSystemTick()) / 1000); }
std::uint64_t now_ms() { return static_cast<std::uint64_t>(now_us() / 1000); }
std::uint32_t perf_clock() { return static_cast<std::uint32_t>(now_us()); }

// options.env: KEY=VALUE per line, # comments.
void read_options(const std::string& path) {
  std::ifstream file(path);
  for (std::string line; std::getline(file, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
    std::printf("[ROA] option %s\n", line.c_str());
  }
}

// Controller buttons: the action each dispatches, and its GamepadState button (kButtonNames index).
// Nintendo's layout: A (right) confirms and B (bottom) goes back; X (top) is Triangle, Y (left) Square.
// The left stick's directions navigate like the d-pad.
struct ButtonBinding {
  u64 mask;
  HostAction action;
  int button;
};
constexpr ButtonBinding kButtons[] = {
  {HidNpadButton_Up | HidNpadButton_StickLUp, HostAction::up, 0},
  {HidNpadButton_Down | HidNpadButton_StickLDown, HostAction::down, 1},
  {HidNpadButton_Left | HidNpadButton_StickLLeft, HostAction::left, 2},
  {HidNpadButton_Right | HidNpadButton_StickLRight, HostAction::right, 3},
  {HidNpadButton_A, HostAction::confirm, 4}, {HidNpadButton_B, HostAction::back, 5},
  {HidNpadButton_X, HostAction::triangle, 6}, {HidNpadButton_Y, HostAction::square, 7},
  {HidNpadButton_L, HostAction::l1, 8}, {HidNpadButton_R, HostAction::r1, 9},
  {HidNpadButton_ZL, HostAction::l2, 10}, {HidNpadButton_ZR, HostAction::r2, 11},
  {HidNpadButton_Plus, HostAction::options, 14},
};
constexpr int kStickL3 = 12, kStickR3 = 13;
// Holding + this long quits to the Homebrew Menu, as homebrew apps do; a press is still Options.
constexpr std::uint64_t kQuitHoldMs = 1000;

float stick(s32 value) {
  constexpr float deadzone = 0.16f;
  const float v = std::clamp(value / static_cast<float>(JOYSTICK_MAX), -1.0f, 1.0f);
  if (std::abs(v) < deadzone) return 0;
  return std::copysign((std::abs(v) - deadzone) / (1 - deadzone), v);
}

struct Host {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  GlPresenter presenter;
  GpuRenderer gpu;
  FrameStats stats;
  GcScheduler gc_scheduler{now_us};
  InputScript script;
  VirtualPad virtual_pad;
  PadState pad_state{};
  struct Boosted {
    ClkrstSession session{};
    u32 before = 0; // restored on exit; 0 when the clock was not changed
  } boosted[std::size(kBoost)];
  bool clkrst = false;
  bool running = true, runtime_started = false, backend_started = false;
  int render_worker_count = 1;
  std::int64_t previous_step_us = 0;
  std::uint64_t next_image_stats = 0;
  std::string image_stats;
  std::uint32_t slow_frame_us = 33000;
  u64 buttons_held = 0;
  // A held direction repeats after 350 ms, every 110 ms, as on the desktop.
  int held_button = -1;
  const char* held_action = nullptr;
  u64 held_mask = 0;
  std::uint64_t next_repeat = 0;
  std::uint64_t plus_down_ms = 0;
  bool finger_down = false;
  float finger_x = 0, finger_y = 0;

  ~Host() {
    gpu.stop();
    roa_stop_images();
    archives::stop();
    browser_capture::stop();
    lan::stop();
    packages::stop();
    removals::stop();
    network::stop();
    video_layers::stop();
    if (runtime_started) er_runtime_shutdown();
    text_shaper::shutdown();
    roa_stop_sound();
    render_workers::stop();
    if (backend_started) er_software_backend_destroy();
    presenter.release();
    if (display != EGL_NO_DISPLAY) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
      if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
      eglTerminate(display);
    }
    for (Boosted& clock : boosted) {
      if (!clock.before) continue;
      clkrstSetClockRate(&clock.session, clock.before);
      clkrstCloseSession(&clock.session);
    }
    if (clkrst) clkrstExit();
  }

  // ROA_BOOST=0 leaves the clocks as the system set them.
  void boost_clocks() {
    const char* option = std::getenv("ROA_BOOST");
    if ((option && !std::strcmp(option, "0")) || R_FAILED(clkrstInitialize())) return;
    clkrst = true;
    for (std::size_t i = 0; i < std::size(kBoost); ++i) {
      Boosted& clock = boosted[i];
      if (R_FAILED(clkrstOpenSession(&clock.session, kBoost[i].module, 3))) continue;
      u32 before = 0;
      clkrstGetClockRate(&clock.session, &before);
      if (before && before < kBoost[i].hz && R_SUCCEEDED(clkrstSetClockRate(&clock.session, kBoost[i].hz))) {
        clock.before = before;
        std::printf("[ROA] %s %u -> %u MHz\n", kBoost[i].name, before / 1000000, kBoost[i].hz / 1000000);
      } else {
        std::printf("[ROA] %s stays at %u MHz\n", kBoost[i].name, before / 1000000);
        clkrstCloseSession(&clock.session);
      }
    }
  }

  bool start_gl() {
    NWindow* window = nwindowGetDefault();
    nwindowSetDimensions(window, width, height);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count < 1) return false;
    surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) return false;
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) return false;
    eglSwapInterval(display, 1);
    return true;
  }

  bool boot() {
    std::printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    er_perf_set_clock(perf_clock);
    if (!presenter.init(width, height)) return false;
    backend_started = er_software_backend_init(width, height);
    if (!backend_started || !damage_tracker_install(width, height)) return false;
    er_set_layers_enabled(presenter.layers_supported());
    // The GPU draws unless ROA_GPU=0; it needs the ops in drawing order, from one worker.
    const char* gpu_option = std::getenv("ROA_GPU");
    const bool gpu_drawing = !gpu_option || std::strcmp(gpu_option, "0") != 0;
    render_worker_count = render_workers::start(gpu_drawing ? 1 : render_workers::available_cpus());
    std::printf("[ROA] render workers=%d, %s\n", render_worker_count, gpu_drawing ? "gpu" : "software");
    if (gpu_drawing && !gpu.start(width, height, GpuRenderer::Mode::draw))
      std::fprintf(stderr, "[ROA] gpu renderer: could not start\n");
    if (network::start()) roa_start_images();
    roa_start_sound();
    ErRuntimeConfig cfg = {};
    cfg.screen_width = width; cfg.screen_height = height; cfg.screen_scale = 2;
    cfg.memory_limit = kJsMemoryLimit;
    cfg.malloc_functions = js_heap_functions();
    cfg.max_stack_size = 1024 * 1024;
    cfg.install_host_globals = roa_install_host_api;
    runtime_started = er_runtime_init(&cfg);
    if (!runtime_started) return false;
    layout.register_assets();
    text_shaper::install("romfs:/fonts", [](const char* line) { std::printf("[ROA] %s\n", line); });
    const char* path = layout.bundle;
    FILE* file = std::fopen(path, "rb");
    if (!file) {
      std::fprintf(stderr, "[ROA] no bundle at %s: %s\n", path, std::strerror(errno));
      return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    // QuickJS's source parser requires a trailing NUL beyond the supplied length.
    std::vector<char> source(static_cast<std::size_t>(std::max(size, 0L)) + 1, '\0');
    const bool read = size >= 0 && std::fread(source.data(), 1, static_cast<std::size_t>(size), file) ==
                                       static_cast<std::size_t>(size);
    std::fclose(file);
    if (!read || !er_runtime_load_source(source.data(), static_cast<std::size_t>(size), path)) return false;
    if (const char* text = std::getenv("ROA_INPUT_SCRIPT")) {
      char error[160];
      if (!script.parse(text, error, sizeof error)) std::fprintf(stderr, "ROA_INPUT_SCRIPT: %s\n", error);
    }
    previous_step_us = now_us();
    return true;
  }

  bool call_input(const char* name, int argc, JSValue* argv) {
    gc_scheduler.input(now_us());
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    JSContext* ctx = er_runtime_context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JSValue result = JS_Call(ctx, fn, global, argc, argv);
    const bool ok = !JS_IsException(result);
    if (!ok) {
      JSValue error = JS_GetException(ctx);
      const char* message = JS_ToCString(ctx, error);
      std::fprintf(stderr, "Input failed: %s\n", message ? message : "JS exception");
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

  bool dispatch(const char* action) {
    JSValue arg = JS_NewString(er_runtime_context(), action);
    return call_input("__roaDispatch", 1, &arg);
  }

  // The touchscreen's first finger at (x, y) in screen px, or lifted.
  bool screen_touch(bool down, float x, float y) {
    JSContext* ctx = er_runtime_context();
    JSValue args[2] = {down ? JS_NewFloat64(ctx, x) : JS_NULL, down ? JS_NewFloat64(ctx, y) : JS_NULL};
    return call_input("__roaScreenTouch", 2, args);
  }

  // The panel reports 1280x720 whatever the layout; a finger is passed on each frame it moves, and once
  // when it lifts. Docked there is no panel, and no touches.
  bool read_touch() {
    HidTouchScreenState state{};
    const bool down = hidGetTouchScreenStates(&state, 1) && state.count > 0;
    if (!down) {
      if (!finger_down) return true;
      finger_down = false;
      return screen_touch(false, 0, 0);
    }
    const float x = static_cast<float>(state.touches[0].x) * width / 1280.0f;
    const float y = static_cast<float>(state.touches[0].y) * height / 720.0f;
    if (finger_down && x == finger_x && y == finger_y) return true;
    finger_down = true;
    finger_x = x;
    finger_y = y;
    return screen_touch(true, x, y);
  }

  // Presses since the last frame, held state for repeats, and the controller for Controller.getState().
  bool read_pad(GamepadState& state) {
    padUpdate(&pad_state);
    const u64 held = padGetButtons(&pad_state);
    const u64 pressed = held & ~buttons_held;
    buttons_held = held;
    state.connected = padIsConnected(&pad_state);
    for (const ButtonBinding& binding : kButtons) {
      if (held & binding.mask) state.buttons |= 1u << binding.button;
      if (!(pressed & binding.mask)) continue;
      // Next to the slow-frame lines, so a lag can be traced to what the player pressed.
      std::printf("[ROA] press %s\n", action_name(binding.action));
      if (!dispatch(action_name(binding.action))) return false;
      if (repeats(binding.action)) {
        held_button = binding.button;
        held_action = action_name(binding.action);
        held_mask = binding.mask;
        next_repeat = now_ms() + 350;
      }
    }
    if (pressed & HidNpadButton_Plus) plus_down_ms = now_ms();
    if ((held & HidNpadButton_Plus) && now_ms() - plus_down_ms >= kQuitHoldMs) {
      std::printf("[ROA] + held: quitting\n");
      running = false;
      return true;
    }
    if (held & HidNpadButton_StickL) state.buttons |= 1u << kStickL3;
    if (held & HidNpadButton_StickR) state.buttons |= 1u << kStickR3;
    if (held_action && !(held & held_mask)) held_action = nullptr;
    if (held_action && now_ms() >= next_repeat) {
      next_repeat = now_ms() + 110;
      std::printf("[ROA] press %s (held)\n", held_action);
      if (!dispatch(held_action)) return false;
    }
    const HidAnalogStickState left = padGetStickPos(&pad_state, 0), right = padGetStickPos(&pad_state, 1);
    // Up is positive on the Switch and negative in GamepadState, as on the other hosts.
    state.left_x = stick(left.x);
    state.left_y = -stick(left.y);
    state.right_x = stick(right.x);
    state.right_y = -stick(right.y);
    // The Switch's triggers are digital.
    state.l2 = held & HidNpadButton_ZL ? 1.0f : 0.0f;
    state.r2 = held & HidNpadButton_ZR ? 1.0f : 0.0f;
    return true;
  }

  bool play_script() {
    const char* action = script.next(now_ms());
    if (!action) return true;
    std::printf("[ROA] script: %s\n", action);
    if (!std::strcmp(action, "quit")) running = false;
    else if (VirtualPad::is_step(action)) {
      char error[160];
      if (!virtual_pad.apply(action, now_ms(), error, sizeof error)) std::fprintf(stderr, "script: %s\n", error);
    } else if (!std::strncmp(action, "shot:", 5)) {
      char path[PATH_MAX];
      const std::string name = std::string("/temp0/") + (action + 5) + ".bmp";
      // With the GPU drawing, the software framebuffer holds only what it left to the CPU.
      const bool saved = host::resolve_path(name.c_str(), path, sizeof path) &&
                         (gpu.drawing() ? gpu.save_screen(presenter, path) : save_screenshot(path));
      std::printf("[ROA] shot %s: %s\n", path, saved ? "saved" : std::strerror(errno));
    } else if (!std::strncmp(action, "screen:", 7)) {
      float x = 0, y = 0;
      return screen_touch(std::sscanf(action + 7, "%f,%f", &x, &y) == 2, x, y);
    } else return dispatch(action);
    return true;
  }

  bool frame() {
    if (const char* line = stats.start_frame(now_us())) std::printf("[ROA] %s\n", line);
    er_set_raster_deferred(true);
    GamepadState state;
    if (!read_pad(state) || !read_touch() || !play_script()) return false;
    if (!running) return true;
    for (std::uint32_t pressed = virtual_pad.merge(state, now_ms()), i = 0; pressed; pressed >>= 1, ++i) {
      const char* held = pressed & 1 ? VirtualPad::action(i) : nullptr;
      if (held && !dispatch(held)) return false;
    }
    roa_set_gamepad(state);
    stats.lap(FrameStats::input, now_us());
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_runtime_pump();
    const std::int64_t step_us = now_us();
    const bool stepped = roa_frame(er_runtime_context(), std::min(50.0, (step_us - previous_step_us) / 1000.0));
    previous_step_us = step_us;
    er_perf_phase_end(ER_PERF_PHASE_JS);
    if (!stepped) {
      std::fprintf(stderr, "Frame callback failed\n");
      return false;
    }
    er_set_raster_deferred(false);
    er_commit();
    if (gpu.active()) gpu.replay();
    stats.lap(FrameStats::update, now_us());
    if (roa_exit_requested()) {
      running = false;
      return true;
    }
    const std::size_t repainted = damage_tracker_area();
    er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
    int layer_count = 0;
    const ERLayer* layers = er_get_layers(&layer_count);
    presenter.set_layers({layers, static_cast<std::size_t>(layer_count)});
    video_layers::update(now_us(), 16667);
    // The swap chain's buffers rotate: every frame draws the whole screen.
    const bool drawn = gpu.drawing() ? presenter.draw_texture(gpu.texture(), width, height)
                                     : presenter.draw(er_software_framebuffer(), damage_tracker_rects(),
                                                      damage_tracker_moves(), width, height);
    if (!drawn) return false;
    er_perf_phase_end(ER_PERF_PHASE_PRESENT);
    const GlPresenter::Timing& t = presenter.last_timing();
    stats.note_present(t.upload_us, t.draw_us, t.upload_bytes);
    damage_tracker_clear();
    stats.lap(FrameStats::present, now_us());
    eglSwapBuffers(display, surface);
    stats.lap(FrameStats::swap, now_us());
    if (const char* line = gc_scheduler.frame(JS_GetRuntime(er_runtime_context()), now_us(), repainted,
                                              static_cast<std::size_t>(width) * height))
      std::printf("[ROA] %s\n", line);
    embedded_renderer_tick(16);
    if (const char* line = stats.end_frame(slow_frame_us)) std::printf("[ROA] %s\n", line);
    if (now_ms() >= next_image_stats) {
      next_image_stats = now_ms() + 2000;
      if (std::string line = images::stats(); line != image_stats) {
        std::printf("[ROA] images: %s\n", line.c_str());
        image_stats = std::move(line);
      }
    }
    return !*er_runtime_last_error();
  }
};
} // namespace

int main(int, char**) {
  // Larger socket buffers than the default, for streaming video and parallel image loads.
  SocketInitConfig sockets = *socketGetDefaultInitConfig();
  sockets.tcp_rx_buf_size = 0x40000;
  sockets.tcp_rx_buf_max_size = 0x100000;
  sockets.sb_efficiency = 8;
  socketInitialize(&sockets);
  if (__nxlink_host.s_addr) nxlinkStdio();
  setvbuf(stdout, nullptr, _IOLBF, 0);
  romfsInit();
  const std::string data = "sdmc:/switch/" ROA_APP_NAME;
  const std::string sandbox = data + "/sandbox";
  for (const std::string& dir : {data, sandbox, sandbox + "/app0", sandbox + "/download0", sandbox + "/temp0"})
    mkdir(dir.c_str(), 0755);
  switch_set_root(sandbox.c_str());
  read_options(data + "/options.env");
  // ROA_LAYOUT=720 or 1080 picks one whatever the screen.
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  const char* forced = std::getenv("ROA_LAYOUT");
  layout = forced ? (!std::strcmp(forced, "720") ? kHandheld : kDocked) : docked ? kDocked : kHandheld;
  width = layout.width;
  height = layout.height;
  std::printf("[ROA] data %s, %s, layout %dx%d, applet type %d\n", data.c_str(), docked ? "docked" : "handheld",
              width, height, static_cast<int>(appletGetAppletType()));
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  hidInitializeTouchScreen();
  // The engine's state is process-wide: one run per process.
  Host& host = *new Host;
  padInitializeDefault(&host.pad_state);
  host.boost_clocks();
  if (!host.start_gl()) {
    std::fprintf(stderr, "[ROA] EGL: could not create an OpenGL ES 3.2 context (0x%x)\n", eglGetError());
  } else if (!host.boot()) {
    std::fprintf(stderr, "[ROA] boot failed: %s\n", er_runtime_last_error());
  } else {
    while (host.running && appletMainLoop()) {
      if (!host.frame()) {
        std::fprintf(stderr, "[ROA] stopped: %s\n", er_runtime_last_error());
        break;
      }
    }
  }
  std::fflush(stdout);
  // Tear down while the services the engine's threads use are still up, then let the system clean up.
  delete &host;
  romfsExit();
  socketExit();
  std::_Exit(0);
}
