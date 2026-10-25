// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

// The directory app paths resolve under (/app0, /download0, /temp0 below it): the app's external files
// directory, so a development build can push its bundle and data there with adb.
void android_set_root(const char* path);
const char* android_root();
// Where the system's CA certificates were gathered into one PEM file for curl, or nullptr.
void android_set_ca_bundle(const char* path);
// The activity's window, which video layers (gl_video.cpp) go under; null while it has none.
struct ANativeWindow;
void android_set_window(ANativeWindow* window);
ANativeWindow* android_window();
// The model and release reported by device.info().
void android_set_device(const char* model, const char* release);
