// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "network.hpp"
#include "image_loader.hpp"
#include "archives.hpp"
#include "app_config.hpp"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>
#endif

extern "C" {
#include "er_runtime.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"
void er_register_assets(void);
}
#include "baked_sounds.h"
#include "gl_presenter.hpp"
#include "host_api.hpp"
#include "host_core.hpp"
#include "host_platform.hpp"
#include "actions.hpp"
#include "control.hpp"
#include "pad_input.hpp"
#include "render_workers.hpp"
#include "screenshot.hpp"
#include "storage_stats.hpp"
#include "directory_records.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Render at twice the logical resolution so rounded edges remain smooth on Retina.
constexpr int width = ROA_WIDTH, height = ROA_HEIGHT;

std::int64_t now_us() {
  static const Uint64 frequency = SDL_GetPerformanceFrequency();
  const Uint64 ticks = SDL_GetPerformanceCounter();
  return static_cast<std::int64_t>(ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency);
}

std::uint32_t perf_clock() { return static_cast<std::uint32_t>(now_us()); }

void log_line(const char* line) {
  std::printf("%s\n", line);
  std::fflush(stdout); // The dev watcher reads a pipe, where stdout is fully buffered.
}

// Keyboard and controller bindings; triggers are axes, read separately. See docs/NAVIGATION.md.
constexpr std::pair<SDL_Keycode, HostAction> kKeyActions[] = {
  {SDLK_UP, HostAction::up}, {SDLK_DOWN, HostAction::down}, {SDLK_LEFT, HostAction::left},
  {SDLK_RIGHT, HostAction::right}, {SDLK_RETURN, HostAction::confirm}, {SDLK_SPACE, HostAction::confirm},
  {SDLK_BACKSPACE, HostAction::back}, {SDLK_q, HostAction::l1}, {SDLK_e, HostAction::r1},
  {SDLK_t, HostAction::triangle}, {SDLK_f, HostAction::square}, {SDLK_o, HostAction::options},
};
// The keys' GamepadState buttons (kButtonNames indexes). Z and C are L2 and R2: full travel, or a light
// press (kLightTravel, short of the half-travel press) while Shift is down. O is Options, P the touchpad.
constexpr std::pair<SDL_Scancode, int> kKeyButtons[] = {
  {SDL_SCANCODE_UP, 0}, {SDL_SCANCODE_DOWN, 1}, {SDL_SCANCODE_LEFT, 2}, {SDL_SCANCODE_RIGHT, 3},
  {SDL_SCANCODE_RETURN, 4}, {SDL_SCANCODE_SPACE, 4}, {SDL_SCANCODE_BACKSPACE, 5}, {SDL_SCANCODE_T, 6},
  {SDL_SCANCODE_F, 7}, {SDL_SCANCODE_Q, 8}, {SDL_SCANCODE_E, 9}, {SDL_SCANCODE_O, 14}, {SDL_SCANCODE_P, 15},
};
constexpr float kLightTravel = 0.4f;

float key_travel(SDL_Scancode key) {
  if (!SDL_GetKeyboardState(nullptr)[key]) return 0;
  return SDL_GetModState() & KMOD_SHIFT ? kLightTravel : 1;
}

constexpr std::pair<SDL_GameControllerButton, HostAction> kButtonActions[] = {
  {SDL_CONTROLLER_BUTTON_DPAD_UP, HostAction::up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, HostAction::down},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, HostAction::left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, HostAction::right},
  {SDL_CONTROLLER_BUTTON_A, HostAction::confirm}, {SDL_CONTROLLER_BUTTON_B, HostAction::back},
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, HostAction::l1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, HostAction::r1},
  {SDL_CONTROLLER_BUTTON_Y, HostAction::triangle}, {SDL_CONTROLLER_BUTTON_X, HostAction::square},
  {SDL_CONTROLLER_BUTTON_START, HostAction::options},
};
// Repeater sources: a key's scancode, or a controller button past this.
constexpr int kButtonSource = SDL_NUM_SCANCODES;

float stick(SDL_GameController* controller, SDL_GameControllerAxis axis) {
  return stick_deadzone(SDL_GameControllerGetAxis(controller, axis) / 32767.0f);
}

GamepadState read_gamepad(SDL_GameController* controller) {
  GamepadState state;
  // Indexes follow kButtonNames; l2/r2 come from the triggers below.
  constexpr int buttons[] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, -1,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_TOUCHPAD,
  };
  if (controller && SDL_GameControllerGetAttached(controller)) {
    state.connected = true;
    state.left_x = stick(controller, SDL_CONTROLLER_AXIS_LEFTX);
    state.left_y = stick(controller, SDL_CONTROLLER_AXIS_LEFTY);
    state.right_x = stick(controller, SDL_CONTROLLER_AXIS_RIGHTX);
    state.right_y = stick(controller, SDL_CONTROLLER_AXIS_RIGHTY);
    state.l2 = std::max(0.0f, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f);
    state.r2 = std::max(0.0f, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f);
    for (std::uint32_t i = 0; i < std::size(buttons); ++i)
      if (buttons[i] >= 0 && SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(buttons[i])))
        state.buttons |= 1u << i;
  }
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  for (const auto& [key, button] : kKeyButtons)
    if (keys[key]) state.buttons |= 1u << button;
  state.l2 = std::max(state.l2, key_travel(SDL_SCANCODE_Z));
  state.r2 = std::max(state.r2, key_travel(SDL_SCANCODE_C));
  if (state.l2 > 0.5f) state.buttons |= 1u << 10;
  if (state.r2 > 0.5f) state.buttons |= 1u << 11;
  return state;
}

HostCore::Config core_config() {
  HostCore::Config config;
  config.width = width;
  config.height = height;
  config.clock_us = now_us;
  config.log = log_line;
  config.option = HostCore::env_option;
  return config;
}

struct Host {
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  SDL_GameController* controller = nullptr;
  GlPresenter presenter;
  HostCore core{core_config()};
  bool running = true;
  // The interactive loop leaves a frame that changed nothing unpresented, as the PS5 host does; the
  // self-tests read back every frame. exposed forces the next frame out after the window reappears.
  bool skip_still = false, exposed = false;
  // The monitor's frame interval, for picking video frames by display frame.
  std::int64_t display_frame_us = 16667;
  Repeater repeater;
  Control control;
  bool controlled = false;
  struct Wait { int client; Uint32 until; };
  std::vector<Wait> waits;
  TriggerEdge l2, r2, key_l2, key_r2;
  bool mouse_down = false; // the left button, a finger on the touchscreen

  ~Host() {
    core.stop();
    presenter.release(); // GL objects must be deleted before their context.
    desktop_set_controller(nullptr);
    if (controller) SDL_GameControllerClose(controller);
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
  }

  // `testing` opens a small window of a fixed size, which the self-test's snapshots compare between runs.
  bool start(bool testing) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG) ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)) return false;
    // Half the layout in points (a Retina screen doubles it), shrunk to fit the screen. Even so the
    // window manager may trim a few points off a new window, differently from launch to launch.
    int window_w = testing ? 960 : width / 2, window_h = testing ? 540 : height / 2;
    if (SDL_Rect usable; !testing && SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
      const double fit = std::min({1.0, static_cast<double>(usable.w) / window_w, static_cast<double>(usable.h) / window_h});
      window_w = static_cast<int>(window_w * fit);
      window_h = static_cast<int>(window_h * fit);
    }
    window = SDL_CreateWindow(ROA_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_w, window_h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window || !(context = SDL_GL_CreateContext(window))) return false;
    SDL_GL_SetSwapInterval(1);
    if (SDL_DisplayMode mode; SDL_GetWindowDisplayMode(window, &mode) == 0 && mode.refresh_rate > 0)
      display_frame_us = 1000000 / mode.refresh_rate;
    std::printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
      if (SDL_IsGameController(i) && (controller = SDL_GameControllerOpen(i))) break;
    std::printf("Physical controller: %s\n", controller ? SDL_GameControllerName(controller) : "not connected");
    desktop_set_controller(controller);
    er_perf_set_clock(perf_clock);
    return presenter.init(width, height);
  }

  bool boot(const char* path) {
    if (!core.start_engine(presenter) || !core.start_runtime()) return false;
    // ROA_PROFILE=<file> samples JavaScript; the folded stacks are written there on exit.
    if (std::getenv("ROA_PROFILE")) core.profiler.start(JS_GetRuntime(er_runtime_context()), 1000);
    if (controlled) {
      // Read when the bundle loads: Text reports its content to the inspector only then.
      JSContext* ctx = er_runtime_context();
      JSValue global = JS_GetGlobalObject(ctx);
      JS_SetPropertyStr(ctx, global, "__roaInspecting", JS_TRUE);
      JS_FreeValue(ctx, global);
    }
    return core.load_file(er_register_assets, ROA_FONT_DIR, path);
  }

  // The mouse in window points as a finger in screen px: the window shows the layout scaled.
  bool mouse_touch(bool down, int x, int y) {
    int window_w = 0, window_h = 0;
    SDL_GetWindowSize(window, &window_w, &window_h);
    if (window_w <= 0 || window_h <= 0) return true;
    return core.screen_touch(down, static_cast<float>(x) * width / window_w, static_cast<float>(y) * height / window_h);
  }

  // Polled state, not key-up events: synthetic self-test presses never stay held.
  bool held_down() const {
    const int source = repeater.source();
    if (source < kButtonSource) return SDL_GetKeyboardState(nullptr)[source];
    return controller && SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(source - kButtonSource));
  }

  // Commands from tools/ps5_drive.py, a few a frame so a busy client cannot stall rendering.
  bool serve_control() {
    const Uint32 ticks = SDL_GetTicks();
    std::erase_if(waits, [&](const Wait& wait) {
      if (!SDL_TICKS_PASSED(ticks, wait.until)) return false;
      Control::reply(wait.client, "ok");
      return true;
    });
    for (int served = 0; served < 4; ++served) {
      int client = -1;
      const std::string line = control.next(client);
      if (client < 0) return true;
      const std::size_t space = line.find(' ');
      const std::string verb = line.substr(0, space), arg = space == std::string::npos ? "" : line.substr(space + 1);
      if (verb == "press") {
        const bool known = std::any_of(std::begin(kActionNames), std::end(kActionNames),
                                       [&](const char* name) { return arg == name; });
        if (known && !core.dispatch(arg.c_str())) return false;
        Control::reply(client, known ? "ok" : "unknown action: " + arg);
      } else if (verb == "hold" || verb == "release" || verb == "trigger") {
        std::string step = line;
        std::replace(step.begin(), step.end(), ' ', ':');
        char error[160];
        Control::reply(client, core.virtual_pad.apply(step, SDL_GetTicks64(), error, sizeof error) ? "ok" : std::string(error));
      } else if (verb == "shot") {
        // `shot <path> full` keeps the framebuffer's full resolution, for images meant to be shown.
        const bool full = arg.size() > 5 && arg.ends_with(" full");
        const std::string path = full ? arg.substr(0, arg.size() - 5) : arg;
        Control::reply(client, save_screenshot(path.c_str(), full ? 1 : 2) ? path : std::string("error: ") + std::strerror(errno));
      } else if (verb == "snapshot") {
        Control::reply(client, core.call_js("__roaInspect", nullptr));
      } else if (verb == "focus") {
        Control::reply(client, core.call_js("__roaFocus", arg.c_str()) == "true" ? "ok" : "not found: " + arg);
      } else if (verb == "script") {
        // Runs input-script steps from now on, e.g. a touchpad swipe.
        char error[512] = {};
        Control::reply(client, core.script.parse(arg.c_str(), error, sizeof error) ? "ok" : std::string("error: ") + error);
      } else if (verb == "logs") {
        Control::reply(client, control.take_logs());
      } else if (verb == "wait") {
        waits.push_back({client, ticks + static_cast<Uint32>(std::max(0, std::atoi(arg.c_str())))});
      } else if (verb == "quit") {
        running = false;
        Control::reply(client, "ok");
      } else {
        Control::reply(client, "unknown command: " + verb);
      }
    }
    return true;
  }

  bool read_input() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      const char* action = nullptr;
      if (event.type == SDL_QUIT) running = false;
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) exposed = true;
      // The host repeats held directions itself, so OS key repeats are ignored.
      if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        const SDL_Scancode key = event.key.keysym.scancode;
        // Escape stands for the PS button: it leaves. O is Options.
        if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
        for (const auto& [code, mapped] : kKeyActions) {
          if (code != event.key.keysym.sym) continue;
          action = action_name(mapped);
          repeater.press(mapped, key, SDL_GetTicks64());
        }
      } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        const auto button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
        for (const auto& [code, mapped] : kButtonActions) {
          if (code != button) continue;
          action = action_name(mapped);
          repeater.press(mapped, kButtonSource + button, SDL_GetTicks64());
        }
      } else if ((event.type == SDL_CONTROLLERTOUCHPADDOWN || event.type == SDL_CONTROLLERTOUCHPADMOTION ||
                  event.type == SDL_CONTROLLERTOUCHPADUP) && event.ctouchpad.touchpad == 0 && event.ctouchpad.finger == 0) {
        if (!core.touch(event.type != SDL_CONTROLLERTOUCHPADUP, event.ctouchpad.x, event.ctouchpad.y)) return false;
      } else if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) &&
                 event.button.button == SDL_BUTTON_LEFT) {
        mouse_down = event.type == SDL_MOUSEBUTTONDOWN;
        if (!mouse_touch(mouse_down, event.button.x, event.button.y)) return false;
      } else if (event.type == SDL_MOUSEMOTION && mouse_down) {
        if (!mouse_touch(true, event.motion.x, event.motion.y)) return false;
      } else if (event.type == SDL_CONTROLLERAXISMOTION) {
        const float travel = event.caxis.value / 32767.0f;
        if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && l2.crossed(travel)) action = action_name(HostAction::l2);
        if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && r2.crossed(travel)) action = action_name(HostAction::r2);
      }
      if (action && !core.dispatch(action)) return false;
    }
    return true;
  }

  bool frame(bool swap = true) {
    core.begin_frame();
    if (!read_input() || !core.play_script(SDL_GetTicks64())) return false;
    if (core.quit_requested()) running = false;
    if (controlled && !serve_control()) return false;
    if (const char* held = repeater.due(held_down(), SDL_GetTicks64()); held && !core.dispatch(held)) return false;
    if (!running) return true;
    if (key_l2.crossed(key_travel(SDL_SCANCODE_Z)) && !core.dispatch(action_name(HostAction::l2))) return false;
    if (key_r2.crossed(key_travel(SDL_SCANCODE_C)) && !core.dispatch(action_name(HostAction::r2))) return false;
    if (!core.deliver_pad(read_gamepad(controller), SDL_GetTicks64()) || !core.advance(core.wall_elapsed_ms()))
      return false;
    if (roa_exit_requested()) {
      running = false;
      return true;
    }
    int sw = 0, sh = 0;
    SDL_GL_GetDrawableSize(window, &sw, &sh);
    const HostCore::Presented presented = core.present(sw, sh, display_frame_us, skip_still && !exposed);
    exposed = false;
    if (!presented.ok) return false;
    if (presented.still) {
      SDL_DisplayMode mode{};
      SDL_Delay(1000 / (SDL_GetWindowDisplayMode(window, &mode) == 0 && mode.refresh_rate > 0 ? mode.refresh_rate : 60));
    } else if (swap) {
      SDL_GL_SwapWindow(window);
    }
    core.end_frame();
    if (const int hz = roa_take_refresh_request()) {
      // The preview keeps the monitor's rate, but rebuilds the presenter as the PS5 host must, so that
      // path runs here too, and reports the requested rate as the one the display accepted.
      presenter.release();
      if (!presenter.init(width, height)) return false;
      roa_set_refresh_rate(hz);
      core.logf("[ROA] refresh switch to %d Hz (the preview keeps the monitor's rate)", hz);
    }
    return !*er_runtime_last_error();
  }

  bool snapshot(const char* path) {
    if (!frame(false)) return false;
    int sw = 0, sh = 0;
    SDL_GL_GetDrawableSize(window, &sw, &sh);
    if (sw <= 0 || sh <= 0) return false;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(sw) * sh * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, sw, sh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    if (glGetError() != GL_NO_ERROR) return false;
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    bool ok = std::fprintf(file, "P6\n%d %d\n255\n", sw, sh) > 0;
    for (int y = sh - 1; y >= 0 && ok; --y)
      ok = std::fwrite(rgb.data() + static_cast<std::size_t>(y) * sw * 3, 3, sw, file) == static_cast<std::size_t>(sw);
    if (std::fclose(file) != 0) ok = false;
    SDL_GL_SwapWindow(window);
    return ok;
  }
};

bool expect(int focus, int count, int detail) {
  JSContext* ctx = er_runtime_context();
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue state = JS_GetPropertyStr(ctx, global, "__roaTestState");
  const char* fields[] = {"focus", "count", "detail"};
  const int expected[] = {focus, count, detail};
  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    JSValue value = JS_GetPropertyStr(ctx, state, fields[i]);
    int32_t actual = -1;
    if (JS_ToInt32(ctx, &actual, value) || actual != expected[i]) {
      std::fprintf(stderr, "%s: expected %d, got %d\n", fields[i], expected[i], actual);
      ok = false;
    }
    JS_FreeValue(ctx, value);
  }
  JS_FreeValue(ctx, state); JS_FreeValue(ctx, global);
  return ok;
}

bool color_test(Host& host) {
  const std::uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
  std::vector<std::uint32_t> pattern(width * height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      pattern[y*width+x] = colors[(y >= height/2 ? 2 : 0) + (x >= width/2 ? 1 : 0)];
  int sw = 0, sh = 0;
  SDL_GL_GetDrawableSize(host.window, &sw, &sh);
  if (!host.presenter.draw(pattern.data(), sw, sh)) return false;
  // Samples in the centre of each quadrant test channel order and vertical orientation.
  for (int i = 0; i < 4; ++i) {
    unsigned char pixel[4] = {};
    glReadPixels((i%2 ? 3 : 1)*sw/4, (i/2 ? 1 : 3)*sh/4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    const auto c = colors[i];
    if (pixel[0] != ((c>>16)&255) || pixel[1] != ((c>>8)&255) ||
        pixel[2] != (c&255) || pixel[3] != 255) return false;
  }
  return glGetError() == GL_NO_ERROR;
}

bool press(Host& host, SDL_Keycode key, int focus, int count, int detail) {
  SDL_Event event = {};
  event.type = SDL_KEYDOWN; event.key.keysym.sym = key;
  if (SDL_PushEvent(&event) != 1) return false;
  for (int i = 0; i < 4; ++i) if (!host.frame()) return false;
  return expect(focus, count, detail);
}

bool storage_test() {
  double total = 0, free = 0;
  if (!storage::disk_bytes(4096, 100, 25, total, free) || total != 409600 || free != 102400 ||
      !storage::disk_bytes(4096, 100, -2, total, free) || total != 409600 || free != 0 ||
      !storage::disk_bytes(4096, 100, 120, total, free) || free != total ||
      storage::disk_bytes(0, 100, 50, total, free) ||
      !storage::disk_bytes(4096, UINT64_MAX, 1, total, free) ||
      total <= static_cast<double>(UINT64_MAX) || free != 4096) return false;
  host::MountEntry mounts[2]{};
  int count = 0;
  host::MountEntry mount{};
  std::strcpy(mount.path, "/app0");
  std::strcpy(mount.device, "/dev/test");
  std::strcpy(mount.type, "nullfs");
  storage::append_unique(mounts, 2, count, mount);
  storage::append_unique(mounts, 2, count, mount);
  if (count != 1) return false;
  std::strcpy(mount.path, "/download0");
  storage::append_unique(mounts, 2, count, mount);
  std::strcpy(mount.path, "/temp0");
  storage::append_unique(mounts, 2, count, mount);
  if (count != 2 || std::strcmp(mounts[1].path, "/download0")) return false;

  // Packed PS5 records can be unaligned and must never read past their bounds.
  char bytes[25]{};
  const auto record = [&](int offset, const char* name) {
    const std::uint32_t inode = 1;
    const std::uint16_t length = 12;
    std::memcpy(bytes + offset, &inode, sizeof inode);
    std::memcpy(bytes + offset + 4, &length, sizeof length);
    bytes[offset + 7] = static_cast<char>(std::strlen(name));
    std::strcpy(bytes + offset + 8, name);
  };
  record(1, "."); record(13, "abc");
  std::vector<std::string> names;
  const auto collect = [](const char* name, void* user) {
    static_cast<std::vector<std::string>*>(user)->emplace_back(name);
  };
  if (!host::visit_directory_records(bytes + 1, 24, collect, &names) ||
      names.size() != 1 || names[0] != "abc") return false;
  names.clear();
  if (host::visit_directory_records(bytes + 13, 7, collect, &names) ||
      host::visit_directory_records(bytes + 13, 11, collect, &names)) return false;
  bytes[24] = 'x'; // Missing NUL.
  if (host::visit_directory_records(bytes + 13, 12, collect, &names)) return false;
  bytes[24] = '\0'; bytes[17] = 0; bytes[18] = 0; // Zero record length.
  if (host::visit_directory_records(bytes + 13, 12, collect, &names) || !names.empty()) return false;
  // Exercise actual host bindings, sandbox path resolution, and error propagation.
  constexpr char script[] = R"JS((() => {
    const network = globalThis.__roaNative.network;
    if (!network || !network.version().includes("libcurl") ||
        !Array.isArray(network.poll())) return false;
    for (const invalid of [
      () => network.request("file:///tmp/example", {}),
      () => network.request("https://example.com", {maxBytes: 0}),
      () => network.request("https://example.com", {headers: {Host: "other.test"}}),
      () => network.download("https://example.com", "/download0/a", {headers: {Range: "bytes=0-1"}}),
      () => network.download("https://example.com", "/download0/a", {connections: 65}),
      () => network.download("https://example.com", "/download0/a", {recoverCompleted: 'yes'}),
      () => network.cancel(),
    ]) {
      try { invalid(); return false; }
      catch (error) { if (!error.message.includes("network.")) return false; }
    }
    const rangeProbe = network.request("http://127.0.0.1:1", {headers: {Range: "bytes=0-0"}, maxBytes: 8192});
    if (!Number.isInteger(rangeProbe) || rangeProbe <= 0) return false;
    network.cancel(rangeProbe);
    // With any sound baked, a second play while the first still sounds is skipped.
    const sound = globalThis.__roaNative.sound;
    const baked = globalThis.__roaTestSound;
    if (baked && (sound.play(baked, 0, 0) !== true || sound.play(baked, 0, 0) !== false)) return false;
    // Baked names drop the extension, so no import is named this.
    try { sound.play('not-baked.wav', 1, 0); return false; }
    catch (error) { if (!error.message.includes('sound.play not-baked.wav')) return false; }
    const fs = globalThis.__roaNative.fs;
    const testName = `listing-test-${Date.now()}.txt`;
    const testPath = `/download0/${testName}`;
    fs.writeFile(testPath, 'listing test', false);
    try {
      const listing = fs.readDir('/download0');
      const item = listing.find(entry => entry.name === testName);
      if (!item || !item.isFile || item.isDirectory || item.size !== 12 ||
          !Number.isFinite(item.modified)) return false;
      const info = fs.stat(testPath);
      if (!/^\d+$/.test(info.device) || !/^\d+$/.test(info.inode) ||
          typeof info.device !== 'string' || typeof info.inode !== 'string' ||
          info.device !== item.device || info.inode !== item.inode) return false;
      try { fs.readDir(testPath); return false; }
      catch (error) { if (!error.message.includes('fs.readDir')) return false; }
      // readFile sizes its buffer from the file: exact for small, empty and multi-megabyte files.
      if (fs.readFile(testPath) !== 'listing test') return false;
      fs.writeFile(testPath, '', false);
      if (fs.readFile(testPath) !== '') return false;
      const large = 'x'.repeat(3 * 1024 * 1024 + 7);
      fs.writeFile(testPath, large, false);
      if (fs.readFile(testPath) !== large) return false;
      fs.writeFile(testPath, 'synced', false, true);
      if (fs.readFile(testPath) !== 'synced') return false;
    } finally { fs.remove(testPath); }
    // removeTree deletes the tree but only unlinks the symbolic link set up below, keeping its target.
    fs.removeTree('/download0/remove-tree-test');
    if (fs.stat('/download0/remove-tree-test') || !fs.stat('/download0/remove-tree-target/keep.txt')) return false;
    fs.removeTree('/download0/remove-tree-target');
    try { fs.removeTree('/download0/remove-tree-test'); return false; }
    catch (error) { if (!error.message.includes('fs.removeTree')) return false; }
    const usage = fs.diskUsage('/download0');
    if (!Number.isFinite(usage.total) || usage.total <= 0 ||
        usage.free < 0 || usage.free > usage.total) return false;
    const mounts = fs.mounts();
    if (!Array.isArray(mounts) || !mounts.length || mounts.length > 64 ||
        !mounts.every(m => typeof m.path === 'string' && typeof m.device === 'string' &&
                           typeof m.type === 'string')) return false;
    try { fs.diskUsage('/missing-storage-test-path'); return false; }
    catch (error) {
      return error.message.includes('fs.diskUsage') &&
             error.message.includes('/missing-storage-test-path');
    }
  })())JS";
  char tree[PATH_MAX], target[PATH_MAX];
  if (!host::resolve_path("/download0/remove-tree-test", tree, sizeof tree) ||
      !host::resolve_path("/download0/remove-tree-target", target, sizeof target)) return false;
  const std::string tree_path = tree, target_path = target;
  // A previous run that stopped early may have left the fixture behind.
  const auto directory = [](const std::string& path) { return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; };
  unlink((tree_path + "/nested/link").c_str());
  std::FILE* kept = nullptr;
  if (!directory(target_path) || !(kept = std::fopen((target_path + "/keep.txt").c_str(), "w")) ||
      !directory(tree_path) || !directory(tree_path + "/nested") ||
      symlink(target, (tree_path + "/nested/link").c_str()) != 0) {
    if (kept) std::fclose(kept);
    return false;
  }
  std::fclose(kept);
  if (std::FILE* file = std::fopen((tree_path + "/nested/file.txt").c_str(), "w")) std::fclose(file);
  JSContext* ctx = er_runtime_context();
  // The first sound the app baked, whatever it is named; none leaves only the unknown-name check.
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "__roaTestSound",
                    roa_sound_count ? JS_NewString(ctx, roa_sounds[0].name) : JS_NULL);
  JS_FreeValue(ctx, global);
  JSValue result = JS_Eval(ctx, script, sizeof script - 1, "storage-test.js", JS_EVAL_TYPE_GLOBAL);
  const bool ok = !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
  if (JS_IsException(result)) {
    JSValue error = JS_GetException(ctx);
    const char* message = JS_ToCString(ctx, error);
    std::fprintf(stderr, "Storage test: %s\n", message ? message : "unknown exception");
    JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, error);
  }
  JS_FreeValue(ctx, result);
  if (ok) std::puts("PASS: bounded PS5 directory records, mount deduplication, storage bytes, sounds and desktop native bindings.");
  return ok;
}

bool self_test(Host& host) {
  if (!storage_test()) return false;
  for (int i = 0; i < 4; ++i) if (!host.frame()) return false;
  if (!expect(0, 0, 0) || !host.snapshot("texture-initial.ppm")) return false;
  if (!press(host, SDLK_RETURN, 0, 1, 1) || !host.snapshot("texture-increment.ppm") ||
      !press(host, SDLK_BACKSPACE, 0, 1, 0) || !press(host, SDLK_RIGHT, 1, 1, 0) ||
      !press(host, SDLK_RETURN, 1, 11, 1) || !press(host, SDLK_BACKSPACE, 1, 11, 0) ||
      !press(host, SDLK_RIGHT, 2, 11, 0) || !press(host, SDLK_RETURN, 2, 0, 1) ||
      !press(host, SDLK_BACKSPACE, 2, 0, 0)) return false;
  SDL_Event button = {};
  button.type = SDL_CONTROLLERBUTTONDOWN;
  button.cbutton.button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  if (SDL_PushEvent(&button) != 1) return false;
  button.cbutton.button = SDL_CONTROLLER_BUTTON_A;
  if (SDL_PushEvent(&button) != 1 || !host.frame() || !expect(0, 1, 1)) return false;
  if (SDL_SetWindowFullscreen(host.window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) return false;
  for (int i = 0; i < 4; ++i) if (!host.frame()) return false;
  if (!(SDL_GetWindowFlags(host.window) & SDL_WINDOW_FULLSCREEN) ||
      !expect(0, 1, 1) || !host.snapshot("texture-fullscreen.ppm")) return false;
  std::puts("PASS: software framebuffer → OpenGL texture, channel order, orientation, React state/focus, simulated controller and fullscreen.");
  // The snapshots are compared against a single-core run, which proves nothing if no commit forked.
  const int workers = host.core.render_worker_count();
  if (workers > 1 && render_workers::forked_commits() == 0) {
    std::fprintf(stderr, "Render workers: %d installed, but no commit rendered in parallel\n", workers);
    return false;
  }
  if (workers > 1) std::printf("PASS: %u commits rendered across %d workers.\n", render_workers::forked_commits(), workers);
  return true;
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s bundle.js [--self-test|--fullscreen]\n", argv[0]);
    return 1;
  }
  const bool testing = argc > 2 && !std::strcmp(argv[2], "--self-test");
  if (testing) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
  Host host;
  bool ok = host.start(testing);
  if (ok && testing) ok = color_test(host);
  if (ok && !testing) {
    if (const char* path = std::getenv("ROA_CONTROL")) {
      host.controlled = host.control.start(path);
      if (!host.controlled) std::fprintf(stderr, "ROA_CONTROL: cannot listen on %s: %s\n", path, std::strerror(errno));
      else std::printf("[ROA] control: %s\n", path);
    }
  }
  if (ok) ok = host.boot(argv[1]);
  if (ok && testing) ok = self_test(host);
  else if (ok) {
    if (argc > 2 && !std::strcmp(argv[2], "--fullscreen")) {
      ok = SDL_SetWindowFullscreen(host.window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
      SDL_ShowCursor(SDL_DISABLE);
    }
    host.skip_still = true;
    while (ok && host.running) ok = host.frame();
    if (JsProfiler& profiler = host.core.profiler; profiler.running()) {
      profiler.stop();
      const char* path = std::getenv("ROA_PROFILE");
      std::printf("profile: %llu samples, %s %s\n", static_cast<unsigned long long>(profiler.samples()),
                  profiler.write(path) ? "saved to" : "could not write", path);
    }
  }
  if (!ok) std::fprintf(stderr, "Texture proof failed: %s / %s\n", SDL_GetError(), er_runtime_last_error());
  if (ok && desktop_relaunch_requested()) {
    SDL_Quit();
    execv(argv[0], argv);
    std::fprintf(stderr, "relaunch: %s\n", std::strerror(errno));
  }
  return ok ? 0 : 1;
}

void archives::trace(const char*, const char*) {}
std::size_t archives::pipeline_bytes() { return 32 * 1024 * 1024; }
bool archives::hold_memory(bool hold) { images::hold_memory(hold); return true; }
bool archives::downloading() { return network::downloading(); }
bool archives::list_directory(const char* path, void (*visit)(const char*, void*), void* user) { return host::read_dir(path, visit, user); }
