# Nintendo Switch host

`native/switch` runs an app on a Nintendo Switch with custom firmware (Atmosphère), as a homebrew NRO:
the GPU renderer (`native/shared/gpu_renderer.hpp`) draws through Mesa's OpenGL ES 3.2, the same path
as the [Android host](ANDROID.md). The Tegra X1 is the SHIELD's chip at lower clocks.

```sh
.venv/bin/python3 tools/switch.py --app-dir ../my-app --send <console address>
```

builds `.build/<app>/switch/<app>.nro` with devkitPro (`DEVKITPRO`, default `/opt/devkitpro`: switch-dev,
switch-mesa, switch-ffmpeg, switch-curl) and, with `--send`, pushes it to the Homebrew Menu's netloader (Y)
and prints the app's output until it exits. Open the Homebrew Menu over a game (hold R while starting it):
the album applet gets about 400 MB, a game's slot all of the memory.

The app is laid out twice, at 1920x1080 and 1280x720, whatever its `app.json` says, and both bundles go
into the NRO's RomFS with the fonts. The host picks the screen's own resolution at launch, 720p on the
console and 1080p docked; docking or undocking later keeps it, and the system scales the frames. The
app's data is on the SD card:

| Path | |
| --- | --- |
| `sdmc:/switch/<app>/sandbox/` | `/app0`, `/download0`, `/temp0` resolve here |
| `sdmc:/switch/<app>/options.env` | `KEY=VALUE` lines set before boot |
| `nxlink -a <address> -s <app>.nro KEY=VALUE ...` | the same options for one run, on top of the file's |

Options: `PS5_REACT_GPU=0` draws with the software rasterizer, `PS5_REACT_LAYOUT=720` or `1080` picks a
layout whatever the screen, `PS5_REACT_BOOST=0` leaves the clocks alone, `PS5_REACT_INPUT_SCRIPT` runs a
script as on the desktop.

## Clocks

For the app's run the host sets the console's boost and docked rates and restores them on exit: CPU
1785 MHz (1020 by default), GPU 768 MHz (307 handheld) and memory 1600 MHz (1331 handheld). At the
handheld defaults a scroll, which redraws the whole screen each frame, stalled the presenter for tens of
milliseconds.

## Input

The controller's buttons in Nintendo's layout: A confirms, B goes back, X is Triangle, Y Square, L/R and
ZL/ZR the shoulders and triggers, + Options. The left stick's directions navigate like the D-pad. Holding
+ for a second quits to the Homebrew Menu.

The console's screen works as on a phone: a tap presses the element under it, a drag scrolls the content
under it and flings it on, and focus hides until the next button
([NAVIGATION.md](NAVIGATION.md#host-input-input-protocol-v6)).

## Video

NVDEC decodes H.264, HEVC (8 and 10 bit), VP8, VP9, MPEG-2, MPEG-4 and VC-1 through devkitPro's FFmpeg and
its nvtegra hwaccel (`native/switch/video_decoder.cpp`); FFmpeg parses the streams, so they need no Annex B
conversion. VIC copies each picture out into 256-aligned planes (a 4K 10-bit one in about 9 ms), which the
shared presenter uploads and draws; HDR is tone-mapped to SDR, since the dock's output is SDR. AV1 and
anything else decodes in software, up to 1080p.

## Platform notes

- devkitA64's newlib lacks `pread`/`pwrite`, `sysconf` and `getrusage`: `native/switch/libc_shims.cpp`.
- libnx has no `pthread_detach` (ENOSYS), so `std::thread::detach()` throws there: shared code joins its
  threads instead (`native/shared/video_api.cpp` releases players on threads that it joins later). An
  uncaught exception returns the NRO to hbloader with its other threads still running, which then
  crash on the unmapped code, and the crash report blames them.
- The system's socket service drops broadcasts to 255.255.255.255; LAN discovery sends to the subnet's
  broadcast address there (`native/shared/lan.cpp`).
- devkitPro's curl (7.69) predates `CURLOPT_PROTOCOLS_STR`; its TLS is the system's ssl service.
- Mesa's GLES stalls when a texture a queued draw reads is overwritten: the GPU renderer keeps captured
  pictures by content and gives a new one a texture of its own.
- Not yet: archive extraction, the video upscalers (they need GL 4.3 storage buffers).
