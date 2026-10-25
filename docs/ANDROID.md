# Android TV host

`native/android` runs an app on Android TV (developed on an NVIDIA SHIELD, Android 11): a
NativeActivity that draws with the GPU renderer (`native/shared/gpu_renderer.hpp`) through OpenGL
ES 3.2. The engine still lays out and records every frame; the GPU draws it. `PS5_REACT_GPU=0` in
`options.env` draws with the software rasterizer instead, for comparison.

```sh
.venv/bin/python3 tools/android.py --app-dir ../my-app --device <adb serial>
```

builds `.build/<app>/android/<app>.apk` for 32-bit ARM (`armeabi-v7a`, API 30), installs it, pushes the
bundle to the app's external files directory and starts it. The first build compiles curl and OpenSSL
for the ABI into `.deps/android`. The app is laid out at 1920x1080 whatever its `app.json` says: Android
TV apps draw at 1080p and the system scales them to the TV.

On the device, under `/sdcard/Android/data/<package>/files`:

| Path | |
| --- | --- |
| `app.bundle.js`, `fonts/` | pushed by `tools/android.py`; a JavaScript change needs no new APK |
| `sandbox/` | `/app0`, `/download0`, `/temp0` resolve here |
| `options.env` | `KEY=VALUE` lines set before boot (`--options 'A=1;B=2'`) |

Logs go to logcat under the tag `ps5-react`. Not there yet: video playback (FFmpeg) and archives
(libarchive); their APIs refuse each request. The system's CA store is gathered into one file for curl
at startup.
