// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The Android TV host: a NativeActivity that runs the engine and draws it with the GPU renderer
// (gpu_renderer.hpp) through OpenGL ES 3.2, at the layout's size, which the system scales to the TV.
//
// It reads the bundle, the fonts and the app's data from its external files directory, so a development
// build pushes them with adb (tools/android.py):
//   <files>/app.bundle.js, <files>/fonts/, <files>/sandbox/{app0,download0,temp0}
// and <files>/options.env, KEY=VALUE lines set in the environment before boot: PS5_REACT_GPU=0 draws
// with the software rasterizer instead, PS5_REACT_INPUT_SCRIPT runs a script as on the desktop.
#include "host_platform.hpp"
#include "java.hpp"
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
void er_register_assets(void);
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

#include <EGL/egl.h>
#include <android/log.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int width = PS5_REACT_WIDTH, height = PS5_REACT_HEIGHT;

std::int64_t now_us() {
  timespec now = {};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}
std::uint64_t now_ms() { return static_cast<std::uint64_t>(now_us() / 1000); }
std::uint32_t perf_clock() { return static_cast<std::uint32_t>(now_us()); }

// stdout and stderr go nowhere in an Android app: a thread copies their lines to logcat.
void log_output() {
  int fds[2];
  if (pipe(fds) != 0) return;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  std::thread([fd = fds[0]] {
    char buffer[2048];
    std::string line;
    for (ssize_t n; (n = read(fd, buffer, sizeof buffer)) > 0;) {
      line.append(buffer, static_cast<std::size_t>(n));
      for (std::size_t end; (end = line.find('\n')) != std::string::npos; line.erase(0, end + 1))
        __android_log_write(ANDROID_LOG_INFO, "ps5-react", line.substr(0, end).c_str());
    }
  }).detach();
}

// Every PEM in the system's CA store into one file curl reads (CURLOPT_CAINFO).
bool gather_certificates(const std::string& out) {
  DIR* dir = opendir("/system/etc/security/cacerts");
  if (!dir) return false;
  std::ofstream bundle(out, std::ios::trunc);
  while (const dirent* item = readdir(dir)) {
    if (item->d_name[0] == '.') continue;
    std::ifstream cert(std::string("/system/etc/security/cacerts/") + item->d_name);
    bundle << cert.rdbuf() << '\n';
  }
  closedir(dir);
  return static_cast<bool>(bundle);
}

// options.env: KEY=VALUE per line, # comments.
void read_options(const std::string& path) {
  std::ifstream file(path);
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
    std::printf("[PS5-REACT] option %s\n", line.c_str());
  }
}

std::string property(const char* name) {
  char value[PROP_VALUE_MAX] = "";
  __system_property_get(name, value);
  return value;
}

// Remote, controller and keyboard keys: the action each dispatches, and its GamepadState button
// (kButtonNames index), -1 for none.
struct KeyBinding {
  int32_t key;
  HostAction action;
  int button;
};
constexpr KeyBinding kKeys[] = {
  {AKEYCODE_DPAD_UP, HostAction::up, 0}, {AKEYCODE_DPAD_DOWN, HostAction::down, 1},
  {AKEYCODE_DPAD_LEFT, HostAction::left, 2}, {AKEYCODE_DPAD_RIGHT, HostAction::right, 3},
  {AKEYCODE_DPAD_CENTER, HostAction::confirm, 4}, {AKEYCODE_ENTER, HostAction::confirm, 4},
  {AKEYCODE_NUMPAD_ENTER, HostAction::confirm, 4}, {AKEYCODE_BUTTON_A, HostAction::confirm, 4},
  {AKEYCODE_BACK, HostAction::back, 5}, {AKEYCODE_BUTTON_B, HostAction::back, 5},
  {AKEYCODE_ESCAPE, HostAction::back, 5}, {AKEYCODE_BUTTON_Y, HostAction::triangle, 6},
  {AKEYCODE_BUTTON_X, HostAction::square, 7}, {AKEYCODE_BUTTON_L1, HostAction::l1, 8},
  {AKEYCODE_BUTTON_R1, HostAction::r1, 9}, {AKEYCODE_BUTTON_L2, HostAction::l2, 10},
  {AKEYCODE_BUTTON_R2, HostAction::r2, 11}, {AKEYCODE_BUTTON_START, HostAction::options, 14},
  {AKEYCODE_MENU, HostAction::options, 14},
};

float stick(const AInputEvent* event, int32_t axis) {
  constexpr float deadzone = 0.16f;
  const float value = std::clamp(AMotionEvent_getAxisValue(event, axis, 0), -1.0f, 1.0f);
  if (std::abs(value) < deadzone) return 0;
  return std::copysign((std::abs(value) - deadzone) / (1 - deadzone), value);
}

struct Host {
  android_app* app = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  GlPresenter presenter;
  GpuRenderer gpu;
  FrameStats stats;
  GcScheduler gc_scheduler{now_us};
  InputScript script;
  VirtualPad virtual_pad;
  std::string files;
  bool running = true, booted = false, runtime_started = false, backend_started = false, presenter_started = false;
  bool exposed = true;
  int render_worker_count = 1;
  std::int64_t previous_step_us = 0;
  std::uint64_t next_image_stats = 0;
  std::string image_stats;
  std::uint32_t slow_frame_us = 33000;
  // Input arrives in callbacks between frames; actions wait for the runtime there.
  std::vector<const char*> actions;
  GamepadState pad;
  std::uint32_t key_buttons = 0, hat_buttons = 0;
  bool l2_down = false, r2_down = false;
  // A held direction repeats after 350 ms, every 110 ms, as on the desktop.
  int held_button = -1;
  const char* held_action = nullptr;
  std::uint64_t next_repeat = 0;

  ~Host() {
    gpu.stop();
    ps5_react_stop_images();
    archives::stop();
    browser_capture::stop();
    lan::stop();
    packages::stop();
    removals::stop();
    network::stop();
    video_layers::stop();
    if (runtime_started) er_runtime_shutdown();
    text_shaper::shutdown();
    ps5_react_stop_sound();
    render_workers::stop();
    if (backend_started) er_software_backend_destroy();
    presenter.release();
    destroy_surface();
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
  }

  bool start_gl() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count < 1) return false;
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    return context != EGL_NO_CONTEXT;
  }

  // The window's buffers are the layout's size; the system scales them to the display.
  bool create_surface() {
    ANativeWindow_setBuffersGeometry(app->window, width, height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    surface = eglCreateWindowSurface(display, config, app->window, nullptr);
    if (surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context)) return false;
    eglSwapInterval(display, 1);
    exposed = true;
    return true;
  }

  void destroy_surface() {
    if (surface == EGL_NO_SURFACE) return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    surface = EGL_NO_SURFACE;
  }

  bool window_ready() const { return surface != EGL_NO_SURFACE; }

  bool boot() {
    std::printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    er_perf_set_clock(perf_clock);
    if (!presenter.init(width, height)) return false;
    presenter_started = true;
    backend_started = er_software_backend_init(width, height);
    if (!backend_started || !damage_tracker_install(width, height)) return false;
    er_set_layers_enabled(presenter.layers_supported());
    // The GPU draws unless PS5_REACT_GPU=0; it needs the ops in drawing order, from one worker.
    const char* gpu_option = std::getenv("PS5_REACT_GPU");
    const bool gpu_drawing = !gpu_option || std::strcmp(gpu_option, "0") != 0;
    const char* workers = std::getenv("PS5_REACT_RENDER_WORKERS");
    render_worker_count = render_workers::start(gpu_drawing ? 1
                                                : workers && std::atoi(workers) > 0 ? std::atoi(workers)
                                                                                    : render_workers::available_cpus());
    std::printf("[PS5-REACT] render workers=%d of %d, cpus=%d, %s\n", render_worker_count,
                render_workers::max_workers(), render_workers::available_cpus(), gpu_drawing ? "gpu" : "software");
    if (gpu_drawing && !gpu.start(width, height, GpuRenderer::Mode::draw))
      std::fprintf(stderr, "[PS5-REACT] gpu renderer: could not start\n");
    if (network::start()) ps5_react_start_images();
    ps5_react_start_sound();
    ErRuntimeConfig cfg = {};
    cfg.screen_width = width; cfg.screen_height = height; cfg.screen_scale = 2;
    cfg.memory_limit = kJsMemoryLimit;
    cfg.malloc_functions = js_heap_functions();
    cfg.max_stack_size = 1024 * 1024;
    cfg.install_host_globals = ps5_react_install_host_api;
    runtime_started = er_runtime_init(&cfg);
    if (!runtime_started) return false;
    er_register_assets();
    text_shaper::install((files + "/fonts").c_str(), [](const char* line) { std::printf("[PS5-REACT] %s\n", line); });
    const std::string path = files + "/app.bundle.js";
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
      std::fprintf(stderr, "[PS5-REACT] no bundle at %s: %s\n", path.c_str(), std::strerror(errno));
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
    if (!read || !er_runtime_load_source(source.data(), static_cast<std::size_t>(size), path.c_str())) return false;
    if (const char* text = std::getenv("PS5_REACT_INPUT_SCRIPT")) {
      char error[160];
      if (!script.parse(text, error, sizeof error)) std::fprintf(stderr, "PS5_REACT_INPUT_SCRIPT: %s\n", error);
    }
    previous_step_us = now_us();
    booted = true;
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
    return call_input("__ps5ReactDispatch", 1, &arg);
  }

  void press(HostAction action, int button) {
    actions.push_back(action_name(action));
    if (repeats(action)) {
      held_button = button;
      held_action = action_name(action);
      next_repeat = now_ms() + 350;
    }
  }

  void release(int button) {
    if (button == held_button) held_action = nullptr;
  }

  // 1 when the event was the app's.
  int32_t input(const AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
      const int32_t key = AKeyEvent_getKeyCode(event), action = AKeyEvent_getAction(event);
      for (const KeyBinding& binding : kKeys) {
        if (binding.key != key) continue;
        if (action == AKEY_EVENT_ACTION_DOWN) {
          key_buttons |= 1u << binding.button;
          // The host repeats held directions itself; the system's repeats are dropped.
          if (AKeyEvent_getRepeatCount(event) == 0) press(binding.action, binding.button);
        } else if (action == AKEY_EVENT_ACTION_UP) {
          key_buttons &= ~(1u << binding.button);
          release(binding.button);
        }
        return 1;
      }
      return 0;
    }
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION ||
        !(AInputEvent_getSource(event) & AINPUT_SOURCE_JOYSTICK))
      return 0;
    pad.connected = true;
    pad.left_x = stick(event, AMOTION_EVENT_AXIS_X);
    pad.left_y = stick(event, AMOTION_EVENT_AXIS_Y);
    pad.right_x = stick(event, AMOTION_EVENT_AXIS_Z);
    pad.right_y = stick(event, AMOTION_EVENT_AXIS_RZ);
    pad.l2 = std::max(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                      AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0));
    pad.r2 = std::max(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0),
                      AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0));
    // Triggers act on crossing half travel and re-arm below a quarter.
    const auto crossed = [](bool& down, float travel) {
      if (!down && travel > 0.5f) return down = true;
      if (down && travel < 0.25f) down = false;
      return false;
    };
    if (crossed(l2_down, pad.l2)) actions.push_back(action_name(HostAction::l2));
    if (crossed(r2_down, pad.r2)) actions.push_back(action_name(HostAction::r2));
    // A controller's d-pad is a hat: its directions press and release like keys.
    const float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
    const float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
    const std::uint32_t hat = (hat_y < -0.5f ? 1u : 0u) | (hat_y > 0.5f ? 2u : 0u) | (hat_x < -0.5f ? 4u : 0u) |
                              (hat_x > 0.5f ? 8u : 0u);
    constexpr HostAction kHat[] = {HostAction::up, HostAction::down, HostAction::left, HostAction::right};
    for (int i = 0; i < 4; ++i) {
      const std::uint32_t bit = 1u << i;
      if ((hat & bit) && !(hat_buttons & bit)) press(kHat[i], i);
      else if (!(hat & bit) && (hat_buttons & bit)) release(i);
    }
    hat_buttons = hat;
    return 1;
  }

  bool play_script() {
    const char* action = script.next(now_ms());
    if (!action) return true;
    std::printf("[PS5-REACT] script: %s\n", action);
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
      std::printf("[PS5-REACT] shot %s: %s\n", path, saved ? "saved" : std::strerror(errno));
    } else return dispatch(action);
    return true;
  }

  bool frame() {
    if (const char* line = stats.start_frame(now_us())) std::printf("[PS5-REACT] %s\n", line);
    er_set_raster_deferred(true);
    for (const char* action : actions)
      if (!dispatch(action)) return false;
    actions.clear();
    if (!play_script()) return false;
    if (held_action) {
      const bool down = ((key_buttons | hat_buttons) >> held_button) & 1;
      if (!down) held_action = nullptr;
      else if (now_ms() >= next_repeat) {
        next_repeat = now_ms() + 110;
        if (!dispatch(held_action)) return false;
      }
    }
    if (!running) return true;
    GamepadState state = pad;
    state.buttons = key_buttons | (hat_buttons & 0xFu);
    if (state.l2 > 0.5f) state.buttons |= 1u << 10;
    if (state.r2 > 0.5f) state.buttons |= 1u << 11;
    for (std::uint32_t pressed = virtual_pad.merge(state, now_ms()), i = 0; pressed; pressed >>= 1, ++i) {
      const char* held = pressed & 1 ? VirtualPad::action(i) : nullptr;
      if (held && !dispatch(held)) return false;
    }
    ps5_react_set_gamepad(state);
    stats.lap(FrameStats::input, now_us());
    er_perf_phase_begin(ER_PERF_PHASE_JS);
    er_runtime_pump();
    const std::int64_t step_us = now_us();
    const bool stepped = ps5_react_frame(er_runtime_context(), std::min(50.0, (step_us - previous_step_us) / 1000.0));
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
    if (ps5_react_exit_requested()) {
      running = false;
      return true;
    }
    EGLint sw = 0, sh = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &sw);
    eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
    const std::size_t repainted = damage_tracker_area();
    const bool still = !exposed && presenter.unchanged(damage_tracker_rects(), damage_tracker_moves(), sw, sh);
    exposed = false;
    if (!still) {
      er_perf_phase_begin(ER_PERF_PHASE_PRESENT);
      int layer_count = 0;
      const ERLayer* layers = er_get_layers(&layer_count);
      presenter.set_layers({layers, static_cast<std::size_t>(layer_count)});
      video_layers::update(now_us(), 16667);
      const bool drawn = gpu.drawing() ? presenter.draw_texture(gpu.texture(), sw, sh)
                                       : presenter.draw(er_software_framebuffer(), damage_tracker_rects(),
                                                        damage_tracker_moves(), sw, sh);
      if (!drawn) return false;
      er_perf_phase_end(ER_PERF_PHASE_PRESENT);
      const GlPresenter::Timing& t = presenter.last_timing();
      stats.note_present(t.upload_us, t.draw_us, t.upload_bytes);
    }
    damage_tracker_clear();
    stats.lap(FrameStats::present, now_us());
    // A still frame waits for the next vblank by swapping anyway: the swap interval paces the loop.
    eglSwapBuffers(display, surface);
    stats.lap(FrameStats::swap, now_us());
    if (const char* line = gc_scheduler.frame(JS_GetRuntime(er_runtime_context()), now_us(), repainted,
                                              static_cast<std::size_t>(width) * height))
      std::printf("[PS5-REACT] %s\n", line);
    embedded_renderer_tick(16);
    if (const char* line = stats.end_frame(slow_frame_us)) std::printf("[PS5-REACT] %s\n", line);
    if (now_ms() >= next_image_stats) {
      next_image_stats = now_ms() + 2000;
      if (std::string line = images::stats(); line != image_stats) {
        std::printf("[PS5-REACT] images: %s\n", line.c_str());
        image_stats = std::move(line);
      }
    }
    return !*er_runtime_last_error();
  }
};

void on_command(android_app* app, int32_t command) {
  Host& host = *static_cast<Host*>(app->userData);
  switch (command) {
    case APP_CMD_INIT_WINDOW:
      if (!app->window) break;
      if (host.context == EGL_NO_CONTEXT && !host.start_gl()) {
        std::fprintf(stderr, "[PS5-REACT] EGL: could not create an OpenGL ES 3.2 context\n");
        host.running = false;
        break;
      }
      if (!host.create_surface()) {
        std::fprintf(stderr, "[PS5-REACT] EGL: could not create the window surface (0x%x)\n", eglGetError());
        host.running = false;
        break;
      }
      if (!host.booted && !host.boot()) {
        std::fprintf(stderr, "[PS5-REACT] boot failed: %s\n", er_runtime_last_error());
        host.running = false;
      }
      break;
    case APP_CMD_TERM_WINDOW:
      host.destroy_surface();
      break;
    case APP_CMD_DESTROY:
      host.running = false;
      break;
    default:
      break;
  }
}

int32_t on_input(android_app* app, AInputEvent* event) {
  return static_cast<Host*>(app->userData)->input(event);
}
} // namespace

void android_main(android_app* app) {
  log_output();
  java::start(app->activity->vm, app->activity->clazz);
  // The engine's state is process-wide: one run per process.
  static Host host;
  host.app = app;
  host.files = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
  app->userData = &host;
  app->onAppCmd = on_command;
  app->onInputEvent = on_input;
  ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
  const std::string sandbox = host.files + "/sandbox";
  for (const char* mount : {"", "/app0", "/download0", "/temp0"}) mkdir((sandbox + mount).c_str(), 0755);
  android_set_root(sandbox.c_str());
  const std::string certificates = std::string(app->activity->internalDataPath) + "/cacerts.pem";
  if (gather_certificates(certificates)) android_set_ca_bundle(certificates.c_str());
  android_set_device(property("ro.product.model").c_str(), property("ro.build.version.release").c_str());
  read_options(host.files + "/options.env");
  std::printf("[PS5-REACT] files %s, layout %dx%d\n", host.files.c_str(), width, height);

  while (host.running && !app->destroyRequested) {
    int events = 0;
    android_poll_source* source = nullptr;
    // Every pending event, then a frame; without a window there is nothing to draw, so it waits.
    while (ALooper_pollOnce(host.window_ready() && host.booted ? 0 : -1, nullptr, &events,
                            reinterpret_cast<void**>(&source)) >= 0) {
      if (source) source->process(app, source);
      if (app->destroyRequested || !host.running) break;
    }
    if (host.running && host.window_ready() && host.booted && !host.frame()) {
      std::fprintf(stderr, "[PS5-REACT] stopped: %s\n", er_runtime_last_error());
      host.running = false;
    }
  }
  ANativeActivity_finish(app->activity);
  // The engine and the runtime cannot start twice in one process; the next launch gets a new one.
  std::fflush(stdout);
  std::_Exit(0);
}

void archives::trace(const char*, const char*) {}
std::size_t archives::pipeline_bytes() { return 32 * 1024 * 1024; }
bool archives::hold_memory(bool hold) { images::hold_memory(hold); return true; }
bool archives::downloading() { return network::downloading(); }
bool archives::list_directory(const char* path, void (*visit)(const char*, void*), void* user) {
  return host::read_dir(path, visit, user);
}
