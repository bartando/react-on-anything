// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "java.hpp"

#include <pthread.h>

namespace {
JavaVM* vm = nullptr;
jobject context = nullptr;
pthread_key_t attached;

void detach(void*) {
  if (vm) vm->DetachCurrentThread();
}
} // namespace

namespace java {
void start(JavaVM* machine, jobject activity) {
  vm = machine;
  context = activity;
  pthread_key_create(&attached, detach);
}

jobject activity() { return context; }

JNIEnv* env() {
  JNIEnv* out = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&out), JNI_VERSION_1_6) == JNI_OK) return out;
  if (vm->AttachCurrentThread(&out, nullptr) != JNI_OK) return nullptr;
  // A thread that ends detaches; one that lives on stays attached.
  pthread_setspecific(attached, out);
  return out;
}

bool failed(JNIEnv* env, std::string* error) {
  jthrowable thrown = env->ExceptionOccurred();
  if (!thrown) return false;
  env->ExceptionClear();
  if (error) {
    jclass throwable = env->FindClass("java/lang/Throwable");
    jmethodID describe = env->GetMethodID(throwable, "toString", "()Ljava/lang/String;");
    auto message = static_cast<jstring>(env->CallObjectMethod(thrown, describe));
    if (env->ExceptionCheck()) env->ExceptionClear();
    *error = text(env, message);
    env->DeleteLocalRef(message);
    env->DeleteLocalRef(throwable);
  }
  env->DeleteLocalRef(thrown);
  return true;
}

std::string text(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string out = chars ? chars : "";
  if (chars) env->ReleaseStringUTFChars(value, chars);
  return out;
}
} // namespace java
