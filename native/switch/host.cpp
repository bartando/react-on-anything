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
// and sdmc:/switch/<app>/options.env, KEY=VALUE lines set in the environment before boot, as on Android; nxlink's
// arguments after the NRO add to them for one run.
// Launched by nxlink, stdout goes back to it.
#include "host_platform.hpp"
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
#include "gl_presenter.hpp"
#include "host_api.hpp"
#include "host_core.hpp"
#include "pad_input.hpp"

#include <netinet/in.h> // nxlink.h's in_addr
#include <switch.h>
#include <EGL/egl.h>
#include <sys/stat.h>
#include <algorithm>
#include <iterator>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

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

void log_line(const char* line) { std::printf("%s\n", line); }

HostCore::Config core_config() {
  HostCore::Config config;
  config.width = width;
  config.height = height;
  config.clock_us = now_us;
  config.log = log_line;
  config.option = HostCore::env_option;
  config.gpu_by_default = true;
  return config;
}

struct Host {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  GlPresenter presenter;
  HostCore core{core_config()};
  PadState pad_state{};
  struct Boosted {
    ClkrstSession session{};
    u32 before = 0; // restored on exit; 0 when the clock was not changed
  } boosted[std::size(kBoost)];
  bool clkrst = false;
  bool running = true;
  u64 buttons_held = 0;
  Repeater repeater; // its sources are kButtons indexes
  std::uint64_t plus_down_ms = 0;
  bool finger_down = false;
  float finger_x = 0, finger_y = 0;

  ~Host() {
    core.stop();
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
    return presenter.init(width, height) && core.start_engine(presenter) && core.start_runtime() &&
           core.load_file(layout.register_assets, "romfs:/fonts", layout.bundle);
  }

  // The panel reports 1280x720 whatever the layout; a finger is passed on each frame it moves, and once
  // when it lifts. Docked there is no panel, and no touches.
  bool read_touch() {
    HidTouchScreenState state{};
    const bool down = hidGetTouchScreenStates(&state, 1) && state.count > 0;
    if (!down) {
      if (!finger_down) return true;
      finger_down = false;
      return core.screen_touch(false, 0, 0);
    }
    const float x = static_cast<float>(state.touches[0].x) * width / 1280.0f;
    const float y = static_cast<float>(state.touches[0].y) * height / 720.0f;
    if (finger_down && x == finger_x && y == finger_y) return true;
    finger_down = true;
    finger_x = x;
    finger_y = y;
    return core.screen_touch(true, x, y);
  }

  // Presses since the last frame, held state for repeats, and the controller for Controller.getState().
  bool read_pad(GamepadState& state) {
    padUpdate(&pad_state);
    const u64 held = padGetButtons(&pad_state);
    const u64 pressed = held & ~buttons_held;
    buttons_held = held;
    state.connected = padIsConnected(&pad_state);
    for (std::size_t i = 0; i < std::size(kButtons); ++i) {
      const ButtonBinding& binding = kButtons[i];
      if (held & binding.mask) state.buttons |= 1u << binding.button;
      if (!(pressed & binding.mask)) continue;
      // Next to the slow-frame lines, so a lag can be traced to what the player pressed.
      std::printf("[ROA] press %s\n", action_name(binding.action));
      if (!core.dispatch(action_name(binding.action))) return false;
      repeater.press(binding.action, static_cast<int>(i), now_ms());
    }
    if (pressed & HidNpadButton_Plus) plus_down_ms = now_ms();
    if ((held & HidNpadButton_Plus) && now_ms() - plus_down_ms >= kQuitHoldMs) {
      std::printf("[ROA] + held: quitting\n");
      running = false;
      return true;
    }
    if (held & HidNpadButton_StickL) state.buttons |= 1u << kStickL3;
    if (held & HidNpadButton_StickR) state.buttons |= 1u << kStickR3;
    const bool repeating = repeater.holding() && (held & kButtons[repeater.source()].mask);
    if (const char* action = repeater.due(repeating, now_ms())) {
      std::printf("[ROA] press %s (held)\n", action);
      if (!core.dispatch(action)) return false;
    }
    const HidAnalogStickState left = padGetStickPos(&pad_state, 0), right = padGetStickPos(&pad_state, 1);
    // Up is positive on the Switch and negative in GamepadState, as on the other hosts.
    state.left_x = stick_deadzone(left.x / static_cast<float>(JOYSTICK_MAX));
    state.left_y = -stick_deadzone(left.y / static_cast<float>(JOYSTICK_MAX));
    state.right_x = stick_deadzone(right.x / static_cast<float>(JOYSTICK_MAX));
    state.right_y = -stick_deadzone(right.y / static_cast<float>(JOYSTICK_MAX));
    // The Switch's triggers are digital.
    state.l2 = held & HidNpadButton_ZL ? 1.0f : 0.0f;
    state.r2 = held & HidNpadButton_ZR ? 1.0f : 0.0f;
    return true;
  }

  bool frame() {
    core.begin_frame();
    GamepadState state;
    if (!read_pad(state) || !read_touch() || !core.play_script(now_ms())) return false;
    if (core.quit_requested()) running = false;
    if (!running) return true;
    if (!core.deliver_pad(state, now_ms()) || !core.advance(core.wall_elapsed_ms())) return false;
    if (roa_exit_requested()) {
      running = false;
      return true;
    }
    // The swap chain's buffers rotate: every frame draws the whole screen.
    if (!core.present(width, height, 16667, false).ok) return false;
    eglSwapBuffers(display, surface);
    core.end_frame();
    return !*er_runtime_last_error();
  }
};
} // namespace

int main(int argc, char** argv) {
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
  HostCore::load_env_file((data + "/options.env").c_str(), log_line);
  // nxlink's arguments after the NRO, KEY=VALUE each, on top of the file's.
  for (int i = 1; i < argc; ++i) HostCore::set_env_option(argv[i], log_line);
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
