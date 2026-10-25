// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Calls into the Android framework's Java classes from native threads (the APK has no code of its own).
#pragma once

#include <jni.h>
#include <string>

namespace java {
// From android_main: the VM and the activity, which serves as the Context framework calls need.
void start(JavaVM* vm, jobject activity);
jobject activity();
// The calling thread's environment, attaching it to the VM for the rest of its life.
JNIEnv* env();
// Clears a pending exception; true when there was one, its text in `error` when given.
bool failed(JNIEnv* env, std::string* error = nullptr);
// A Java string as UTF-8; "" for null.
std::string text(JNIEnv* env, jstring value);
} // namespace java
