// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The Android TV host: a NativeActivity that runs the engine and draws it with the GPU renderer
// (gpu_renderer.hpp) through OpenGL ES 3.2, at the layout's size, which the system scales to the TV.
//
// It reads the bundle, the fonts and the app's data from its external files directory, so a development
// build pushes them with adb (tools/android.py):
//   <files>/app.bundle.js, <files>/fonts/, <files>/sandbox/{app0,download0,temp0}
// and <files>/options.env, KEY=VALUE lines set in the environment before boot: ROA_GPU=0 draws
// with the software rasterizer instead, ROA_INPUT_SCRIPT runs a script as on the desktop.
#include "host_platform.hpp"
#include "java.hpp"
#include "media_codec.hpp"
#include "gl_api.hpp"

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
void er_register_assets(void);
}
#include "actions.hpp"
#include "gl_presenter.hpp"
#include "host_api.hpp"
#include "host_core.hpp"
#include "pad_input.hpp"
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
constexpr int width = ROA_WIDTH, height = ROA_HEIGHT;

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
        __android_log_write(ANDROID_LOG_INFO, "react-on-anything", line.substr(0, end).c_str());
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
  // A remote has no face buttons: its menu key is Triangle, as Android TV apps open a context menu with
  // it, and its rewind and fast-forward keys are L1 and R1.
  {AKEYCODE_MENU, HostAction::triangle, 6}, {AKEYCODE_MEDIA_REWIND, HostAction::l1, 8},
  {AKEYCODE_MEDIA_FAST_FORWARD, HostAction::r1, 9},
};
// A remote's OK held this long is Triangle instead; a shorter press confirms when it is let go.
constexpr std::uint64_t kLongPressMs = 500;
bool remote_ok(int32_t key) { return key == AKEYCODE_DPAD_CENTER || key == AKEYCODE_ENTER; }

float stick(const AInputEvent* event, int32_t axis) {
  return stick_deadzone(AMotionEvent_getAxisValue(event, axis, 0));
}

void log_line(const char* line) { std::printf("%s\n", line); }

struct Host {
  android_app* app = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  GlPresenter presenter;
  HostCore core{HostCore::environment(width, height, now_us, log_line, /*gpu_by_default=*/true)};
  // The render thread stops for a collection (about 200ms on the Shield): the videos keep playing.
  static int video_during_collection(std::int64_t now) {
    video_layers::advance(now, 16667);
    return video::refresh_layers();
  }
  std::string files;
  bool running = true, booted = false;
  bool exposed = true;
  // Input arrives in callbacks between frames; actions wait for the runtime there.
  std::vector<const char*> actions;
  GamepadState pad;
  std::uint32_t key_buttons = 0, hat_buttons = 0;
  TriggerEdge l2, r2;
  // The remote's OK while held: when it went down, and whether it already counted as a long press.
  std::uint64_t ok_down_ms = 0;
  bool ok_held = false, ok_long = false;
  Repeater repeater; // its sources are GamepadState buttons

  ~Host() {
    core.stop();
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
    booted = presenter.init(width, height) && core.start_engine(presenter) && core.start_runtime() &&
             core.load_file(er_register_assets, (files + "/fonts").c_str(), (files + "/app.bundle.js").c_str());
    return booted;
  }

  void press(HostAction action, int button) {
    actions.push_back(action_name(action));
    repeater.press(action, button, now_ms());
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
          if (AKeyEvent_getRepeatCount(event) != 0) return 1;
          if (remote_ok(key)) {
            ok_held = true;
            ok_long = false;
            ok_down_ms = now_ms();
          } else {
            press(binding.action, binding.button);
          }
        } else if (action == AKEY_EVENT_ACTION_UP) {
          key_buttons &= ~(1u << binding.button);
          repeater.release(binding.button);
          if (remote_ok(key) && ok_held && !ok_long) actions.push_back(action_name(HostAction::confirm));
          if (remote_ok(key)) ok_held = false;
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
    if (l2.crossed(pad.l2)) actions.push_back(action_name(HostAction::l2));
    if (r2.crossed(pad.r2)) actions.push_back(action_name(HostAction::r2));
    // A controller's d-pad is a hat: its directions press and release like keys.
    const float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
    const float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
    const std::uint32_t hat = (hat_y < -0.5f ? 1u : 0u) | (hat_y > 0.5f ? 2u : 0u) | (hat_x < -0.5f ? 4u : 0u) |
                              (hat_x > 0.5f ? 8u : 0u);
    constexpr HostAction kHat[] = {HostAction::up, HostAction::down, HostAction::left, HostAction::right};
    for (int i = 0; i < 4; ++i) {
      const std::uint32_t bit = 1u << i;
      if ((hat & bit) && !(hat_buttons & bit)) press(kHat[i], i);
      else if (!(hat & bit) && (hat_buttons & bit)) repeater.release(i);
    }
    hat_buttons = hat;
    return 1;
  }

  bool frame() {
    core.begin_frame();
    if (ok_held && !ok_long && now_ms() - ok_down_ms >= kLongPressMs) {
      ok_long = true;
      actions.push_back(action_name(HostAction::triangle));
    }
    for (const char* action : actions)
      if (!core.dispatch(action)) return false;
    actions.clear();
    if (!core.play_script(now_ms())) return false;
    if (core.quit_requested()) running = false;
    const bool held = repeater.holding() && (((key_buttons | hat_buttons) >> repeater.source()) & 1);
    if (const char* action = repeater.due(held, now_ms()); action && !core.dispatch(action)) return false;
    if (!running) return true;
    GamepadState state = pad;
    state.buttons = key_buttons | (hat_buttons & 0xFu);
    if (state.l2 > 0.5f) state.buttons |= 1u << 10;
    if (state.r2 > 0.5f) state.buttons |= 1u << 11;
    if (!core.deliver_pad(state, now_ms()) || !core.advance(core.wall_elapsed_ms())) return false;
    if (roa_exit_requested()) {
      running = false;
      return true;
    }
    EGLint sw = 0, sh = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &sw);
    eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
    const HostCore::Presented presented = core.present(sw, sh, 16667, !exposed);
    exposed = false;
    if (!presented.ok) return false;
    // A still frame waits for the next vblank by swapping anyway: the swap interval paces the loop.
    eglSwapBuffers(display, surface);
    core.end_frame();
    return !*er_runtime_last_error();
  }
};

void on_command(android_app* app, int32_t command) {
  Host& host = *static_cast<Host*>(app->userData);
  switch (command) {
    case APP_CMD_INIT_WINDOW:
      if (!app->window) break;
      if (host.context == EGL_NO_CONTEXT && !host.start_gl()) {
        std::fprintf(stderr, "[ROA] EGL: could not create an OpenGL ES 3.2 context\n");
        host.running = false;
        break;
      }
      android_set_window(app->window);
      if (!host.create_surface()) {
        std::fprintf(stderr, "[ROA] EGL: could not create the window surface (0x%x)\n", eglGetError());
        host.running = false;
        break;
      }
      if (!host.booted && !host.boot()) {
        std::fprintf(stderr, "[ROA] boot failed: %s\n", er_runtime_last_error());
        host.running = false;
      }
      break;
    case APP_CMD_TERM_WINDOW:
      android_set_window(nullptr);
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
  host.core.gc_scheduler.during_collection(Host::video_during_collection);
  app->onAppCmd = on_command;
  app->onInputEvent = on_input;
  ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
  // With alpha, so a video's layer below the window shows through the hole the presenter leaves.
  ANativeActivity_setWindowFormat(app->activity, WINDOW_FORMAT_RGBA_8888);
  const std::string sandbox = host.files + "/sandbox";
  for (const char* mount : {"", "/app0", "/download0", "/temp0"}) mkdir((sandbox + mount).c_str(), 0755);
  android_set_root(sandbox.c_str());
  const std::string certificates = std::string(app->activity->internalDataPath) + "/cacerts.pem";
  if (gather_certificates(certificates)) android_set_ca_bundle(certificates.c_str());
  android_set_device(property("ro.product.model").c_str(), property("ro.build.version.release").c_str());
  HostCore::load_env_file((host.files + "/options.env").c_str(), log_line);
  std::printf("[ROA] files %s, layout %dx%d\n", host.files.c_str(), width, height);

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
      std::fprintf(stderr, "[ROA] stopped: %s\n", er_runtime_last_error());
      host.running = false;
    }
  }
  ANativeActivity_finish(app->activity);
  // The engine and the runtime cannot start twice in one process; the next launch gets a new one.
  std::fflush(stdout);
  std::_Exit(0);
}
