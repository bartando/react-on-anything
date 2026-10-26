// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include "frame_stats.hpp"
#include "gpu_renderer.hpp"
#include "host_api.hpp"
#include "input_script.hpp"
#include "js_heap.hpp"
#include "js_profiler.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include "quickjs.h"
}

class GlPresenter;

// What every platform host does around the engine: start it and the services, hand input to
// JavaScript, run a frame from JavaScript's step to the present, and stop it all again. A host keeps
// what is its own: the window or display and its GL context, reading its input, the swap, and its
// lifecycle. A frame:
//
//   core.begin_frame();                      // input and JavaScript commit layout only from here
//   ...the host reads its input: core.dispatch(), core.touch(), core.screen_touch()...
//   core.play_script(now_ms);                // the input script's step that is due
//   core.deliver_pad(state, now_ms);         // the controller, with what the virtual pad holds
//   core.advance(elapsed_ms);                // JavaScript's frame and the commit
//   core.present(width, height, frame_us, allow_still);
//   ...the host swaps, or waits for the next vblank when the frame was still...
//   core.end_frame();                        // idle collection, the engine's clock, the slow-frame line
class HostCore {
public:
  struct Config {
    int width = 0, height = 0;               // the layout, in framebuffer pixels
    std::int64_t (*clock_us)() = nullptr;    // monotonic microseconds
    void (*log)(const char* line) = nullptr; // a whole line, without its newline
    // A development option by name ("render-workers"), empty when unset: an ROA_* environment variable
    // (env_option) on most hosts, a file in the app's dev/ folder on the PS5.
    std::string (*option)(const char* name) = nullptr;
    // Where a script's `shot:NAME` goes, as a real path; empty skips it. Unset: /temp0/NAME.bmp.
    std::string (*shot_path)(const char* name) = nullptr;
    // The engine's own lines, when its stdout goes nowhere.
    void (*react_log)(const char* line) = nullptr;
    // Called with each phase of the frame as it starts ("js", "commit", "present", ...), for a watchdog.
    void (*phase)(const char* name) = nullptr;
    // Whether the GPU renderer draws unless the "gpu" option is 0; otherwise only when it is set.
    bool gpu_by_default = false;
    // Whether images::stats() is logged every two seconds while it changes.
    bool log_image_stats = true;
  };

  explicit HostCore(const Config& config);
  // The configuration of a host whose options are ROA_* environment variables: the desktop, the Switch
  // and Android.
  static Config environment(int width, int height, std::int64_t (*clock_us)(), void (*log)(const char* line),
                            bool gpu_by_default);
  HostCore(const HostCore&) = delete;
  HostCore& operator=(const HostCore&) = delete;
  // stop(), which a host calls itself while its GL context is still current.
  ~HostCore();

  // ROA_<NAME> from the environment, the name upper-cased with '-' as '_': Config::option on hosts
  // configured through their environment.
  static std::string env_option(const char* name);
  // Sets the environment from an options file of KEY=VALUE lines (# comments), logging each: how the
  // Switch and Android take options, having no shell to launch from.
  static void load_env_file(const char* path, void (*log)(const char* line));
  // One KEY=VALUE option into the environment, logged; anything else is ignored.
  static void set_env_option(const std::string& line, void (*log)(const char* line));

  // With the GL context current and `presenter` initialized: the software framebuffer and damage
  // tracking, the presenter's layers, the render workers, and the GPU renderer or the canvas trace as
  // the options ask. Also reads the frame options (slow-frame-ms, slow-frame-lines, gc-still) and the
  // input script.
  bool start_engine(GlPresenter& presenter);
  // The network, images and sound, then the JavaScript runtime. A host sets the image cache's budget
  // before this.
  bool start_runtime();
  // Registers the baked assets, installs the fonts in `font_dir` (none when empty) and runs the bundle.
  bool load(void (*register_assets)(), const char* font_dir, const char* source, std::size_t length,
            const char* name);
  bool load_file(void (*register_assets)(), const char* font_dir, const char* path);
  // Stops everything started, users before what they use, the GPU renderer while the GL context is
  // still current. Safe to call again.
  void stop();

  // Calls the runtime's input entry `name` with `argc` arguments, which it frees; false when it threw.
  bool call_input(const char* name, int argc, JSValue* argv);
  bool dispatch(const char* action);
  // A touchpad's first finger at (x, y) 0..1, or lifted.
  bool touch(bool down, float x, float y);
  // A touchscreen's first finger at (x, y) in framebuffer pixels, or lifted.
  bool screen_touch(bool down, float x, float y);
  // A development hook's result as text: strings as they are, anything else through JSON.
  std::string call_js(const char* name, const char* arg);

  // Logs the frame-stats summary when one is due, and defers rastering to the commit. Returns whether
  // it logged one: a host adds its own periodic lines then.
  bool begin_frame();
  // Runs the input script's step due at `now_ms`: an action, a virtual-pad step, a touch or a shot.
  // "quit" sets quit_requested(). False when JavaScript failed.
  bool play_script(std::uint64_t now_ms);
  bool quit_requested() const { return quit_requested_; }
  // Adds what the virtual pad holds to `state`, dispatches the buttons it pressed, and hands the
  // controller to JavaScript.
  bool deliver_pad(GamepadState state, std::uint64_t now_ms);
  // The wall-clock time since the previous call, at most 50 ms: a frame's step on hosts that do not
  // count vblanks.
  double wall_elapsed_ms();
  // JavaScript's frame, advancing motion by `elapsed_ms`, then the commit, which the GPU renderer
  // replays. False when JavaScript failed.
  bool advance(double elapsed_ms);

  struct Presented {
    bool ok = true;
    bool still = false; // nothing was drawn: the screen already shows this frame
  };
  // Draws the commit into the default framebuffer of `surface_width`×`surface_height`, the video layers
  // at `frame_us` per display frame. With `allow_still`, a frame that changed nothing is not drawn: the
  // host then waits for the next vblank instead of swapping (on the PS5 a present costs the render
  // thread about 2 ms of CPU even when nothing changed). An empty surface is still.
  Presented present(int surface_width, int surface_height, std::int64_t frame_us, bool allow_still);
  // After the swap: an idle collection, the engine's animation clock by the advance()'s elapsed time,
  // the slow-frame line and the image stats.
  void end_frame();

  // Saves the screen as a BMP at a real path: the GPU renderer's picture when it draws, otherwise the
  // software framebuffer.
  bool save_shot(const char* path);

  int width() const { return config_.width; }
  int height() const { return config_.height; }
  int render_worker_count() const { return render_worker_count_; }
  void logf(const char* format, ...) __attribute__((format(printf, 2, 3)));

  GpuRenderer gpu;
  FrameStats stats;
  GcScheduler gc_scheduler;
  JsProfiler profiler;
  InputScript script;
  VirtualPad virtual_pad;
  std::uint32_t slow_frame_us = 33000;

private:
  std::int64_t now() const { return config_.clock_us(); }
  void enter(const char* phase) const { if (config_.phase) config_.phase(phase); }
  std::string option(const char* name) const { return config_.option ? config_.option(name) : std::string(); }

  Config config_;
  GlPresenter* presenter_ = nullptr;
  bool backend_started_ = false, runtime_started_ = false, tracing_ = false, stopped_ = false;
  bool quit_requested_ = false, redrawn_ = false;
  int render_worker_count_ = 1;
  std::size_t repainted_ = 0;
  std::int64_t previous_step_us_ = 0, next_image_stats_us_ = 0;
  // The engine's clock takes whole milliseconds; carrying the remainder keeps 8.33 ms frames at speed.
  double elapsed_ms_ = 0, tick_carry_ms_ = 0;
  std::string image_stats_;
};
