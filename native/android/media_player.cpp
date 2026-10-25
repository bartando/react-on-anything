// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "media_player.hpp"
#include "gl_api.hpp"
#include "gl_video.hpp"
#include "java.hpp"

#include <GLES2/gl2ext.h>
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
// MediaPlayer.TrackInfo types, and MediaPlayer.SEEK_CLOSEST_SYNC.
constexpr int kTrackVideo = 1, kTrackAudio = 2;
constexpr int kSeekClosestSync = 2;

// The framework's classes and methods, looked up once.
struct Classes {
  jclass player, surface_texture, surface, uri, map, track, format;
  jmethodID player_new, set_surface, set_data_source, prepare, start, pause, seek, position, duration, playing,
      width, height, set_volume, set_looping, release, track_info, select_track, selected_track;
  jmethodID texture_new, texture_release, surface_new, surface_release, uri_parse, map_new, map_put;
  jmethodID track_type, track_language, track_format, format_string, format_has, format_integer;
};

const Classes& classes(JNIEnv* env) {
  static const Classes c = [env] {
    Classes c{};
    const auto global = [env](const char* name) {
      jclass local = env->FindClass(name);
      auto out = static_cast<jclass>(env->NewGlobalRef(local));
      env->DeleteLocalRef(local);
      return out;
    };
    c.player = global("android/media/MediaPlayer");
    c.surface_texture = global("android/graphics/SurfaceTexture");
    c.surface = global("android/view/Surface");
    c.uri = global("android/net/Uri");
    c.map = global("java/util/HashMap");
    c.track = global("android/media/MediaPlayer$TrackInfo");
    c.format = global("android/media/MediaFormat");
    c.player_new = env->GetMethodID(c.player, "<init>", "()V");
    c.set_surface = env->GetMethodID(c.player, "setSurface", "(Landroid/view/Surface;)V");
    c.set_data_source = env->GetMethodID(c.player, "setDataSource",
                                         "(Landroid/content/Context;Landroid/net/Uri;Ljava/util/Map;)V");
    c.prepare = env->GetMethodID(c.player, "prepare", "()V");
    c.start = env->GetMethodID(c.player, "start", "()V");
    c.pause = env->GetMethodID(c.player, "pause", "()V");
    c.seek = env->GetMethodID(c.player, "seekTo", "(JI)V");
    c.position = env->GetMethodID(c.player, "getCurrentPosition", "()I");
    c.duration = env->GetMethodID(c.player, "getDuration", "()I");
    c.playing = env->GetMethodID(c.player, "isPlaying", "()Z");
    c.width = env->GetMethodID(c.player, "getVideoWidth", "()I");
    c.height = env->GetMethodID(c.player, "getVideoHeight", "()I");
    c.set_volume = env->GetMethodID(c.player, "setVolume", "(FF)V");
    c.set_looping = env->GetMethodID(c.player, "setLooping", "(Z)V");
    c.release = env->GetMethodID(c.player, "release", "()V");
    c.track_info = env->GetMethodID(c.player, "getTrackInfo", "()[Landroid/media/MediaPlayer$TrackInfo;");
    c.select_track = env->GetMethodID(c.player, "selectTrack", "(I)V");
    c.selected_track = env->GetMethodID(c.player, "getSelectedTrack", "(I)I");
    c.texture_new = env->GetMethodID(c.surface_texture, "<init>", "(I)V");
    c.texture_release = env->GetMethodID(c.surface_texture, "release", "()V");
    c.surface_new = env->GetMethodID(c.surface, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    c.surface_release = env->GetMethodID(c.surface, "release", "()V");
    c.uri_parse = env->GetStaticMethodID(c.uri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    c.map_new = env->GetMethodID(c.map, "<init>", "()V");
    c.map_put = env->GetMethodID(c.map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    c.track_type = env->GetMethodID(c.track, "getTrackType", "()I");
    c.track_language = env->GetMethodID(c.track, "getLanguage", "()Ljava/lang/String;");
    c.track_format = env->GetMethodID(c.track, "getFormat", "()Landroid/media/MediaFormat;");
    c.format_string = env->GetMethodID(c.format, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    c.format_has = env->GetMethodID(c.format, "containsKey", "(Ljava/lang/String;)Z");
    c.format_integer = env->GetMethodID(c.format, "getInteger", "(Ljava/lang/String;)I");
    return c;
  }();
  return c;
}

// "video/hevc" → "hevc", as the FFmpeg player names codecs; AAC's MIME type names no codec.
std::string codec_name(const std::string& mime) {
  if (mime == "audio/mp4a-latm") return "aac";
  const std::size_t slash = mime.find('/');
  std::string name = slash == std::string::npos ? mime : mime.substr(slash + 1);
  if (name == "avc") return "h264";
  return name;
}

// Textures of released pictures, deleted on the GL thread at the next present.
std::mutex retired_lock;
std::vector<GLuint> retired;
std::uint64_t presents = 0; // GL thread
GLuint program = 0, vao = 0;

const char* vertex_source = GLSL_VERSION R"(
uniform vec4 dst;   // framebuffer pixels: x0 y0 x1 y1
uniform vec4 src;   // the part of the picture drawn, 0..1: x0 y0 x1 y1; beyond it, `contain`'s black bars
uniform vec2 size;  // the framebuffer
out vec2 at;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 p = mix(dst.xy, dst.zw, corner);
  gl_Position = vec4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
  at = mix(src.xy, src.zw, corner);
}
)";
// The SurfaceTexture's transform maps texture coordinates with t = 0 at the picture's bottom.
const char* fragment_source = R"(#version 320 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES picture;
uniform mat4 transform;
uniform float alpha;
in vec2 at;
out vec4 color;
void main() {
  if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) {
    color = vec4(0.0, 0.0, 0.0, alpha);
    return;
  }
  vec2 t = (transform * vec4(at.x, 1.0 - at.y, 0.0, 1.0)).xy;
  color = vec4(texture(picture, t).rgb, 1.0) * alpha;
}
)";

GLuint compile(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    std::fprintf(stderr, "[video] shader: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

namespace video {

struct Player::Picture {
  GLuint texture = 0;
  jobject surface_texture = nullptr, surface = nullptr;
  ASurfaceTexture* native = nullptr;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::uint64_t latched = 0; // the present that last took the newest picture

  // Any thread: the texture waits for the GL thread.
  ~Picture() {
    JNIEnv* env = java::env();
    const Classes& c = classes(env);
    if (native) ASurfaceTexture_release(native);
    if (surface) {
      env->CallVoidMethod(surface, c.surface_release);
      java::failed(env);
      env->DeleteGlobalRef(surface);
    }
    if (surface_texture) {
      env->CallVoidMethod(surface_texture, c.texture_release);
      java::failed(env);
      env->DeleteGlobalRef(surface_texture);
    }
    std::lock_guard lock(retired_lock);
    if (texture) retired.push_back(texture);
  }
};

Player::Player(std::string url, const Options& options)
    : paused_(options.paused), muted_(options.muted), loop_(options.loop), audio_output_(options.audio),
      volume_(options.volume), pending_seek_(options.start > 0 ? options.start : -1) {
  status_.decoder = "MediaPlayer";
  status_.paused = paused_;
  JNIEnv* env = java::env();
  const Classes& c = classes(env);
  picture_ = std::make_shared<Picture>();
  glGenTextures(1, &picture_->texture);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, picture_->texture);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  jobject texture = env->NewObject(c.surface_texture, c.texture_new, static_cast<jint>(picture_->texture));
  jobject surface = texture ? env->NewObject(c.surface, c.surface_new, texture) : nullptr;
  jobject player = env->NewObject(c.player, c.player_new);
  std::string error;
  if (java::failed(env, &error) || !texture || !surface || !player) {
    status_.state = State::failed;
    status_.error = error.empty() ? "MediaPlayer could not be created" : error;
    return;
  }
  picture_->surface_texture = env->NewGlobalRef(texture);
  picture_->surface = env->NewGlobalRef(surface);
  picture_->native = ASurfaceTexture_fromSurfaceTexture(env, texture);
  player_ = env->NewGlobalRef(player);
  env->DeleteLocalRef(texture);
  env->DeleteLocalRef(surface);
  env->DeleteLocalRef(player);
  env->CallVoidMethod(player_, c.set_surface, picture_->surface);
  if (java::failed(env, &error)) {
    status_.state = State::failed;
    status_.error = error;
    return;
  }
  shown_.owner = picture_;
  preparer_ = std::thread(&Player::prepare_main, this, std::move(url), options);
}

Player::~Player() {
  if (preparer_.joinable()) preparer_.join();
  if (player_) {
    JNIEnv* env = java::env();
    env->CallVoidMethod(player_, classes(env).release);
    java::failed(env);
    env->DeleteGlobalRef(player_);
  }
}

// Connects and reads the stream's headers, which can take seconds: on a thread of its own.
void Player::prepare_main(std::string url, Options options) {
  JNIEnv* env = java::env();
  const Classes& c = classes(env);
  std::string error;
  jstring address = env->NewStringUTF(url.c_str());
  jobject uri = env->CallStaticObjectMethod(c.uri, c.uri_parse, address);
  jobject headers = env->NewObject(c.map, c.map_new);
  for (const std::string& header : options.headers) {
    const std::size_t colon = header.find(':');
    if (colon == std::string::npos) continue;
    std::size_t value = colon + 1;
    while (value < header.size() && header[value] == ' ') ++value;
    jstring key = env->NewStringUTF(header.substr(0, colon).c_str());
    jstring text = env->NewStringUTF(header.substr(value).c_str());
    env->DeleteLocalRef(env->CallObjectMethod(headers, c.map_put, key, text));
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(text);
  }
  env->CallVoidMethod(player_, c.set_data_source, java::activity(), uri, headers);
  if (!java::failed(env, &error)) {
    env->CallVoidMethod(player_, c.prepare);
    java::failed(env, &error);
  }
  env->DeleteLocalRef(headers);
  env->DeleteLocalRef(uri);
  env->DeleteLocalRef(address);
  if (!error.empty()) {
    std::fprintf(stderr, "[video] %s: %s\n", url.c_str(), error.c_str());
    std::lock_guard lock(mutex_);
    status_.state = State::failed;
    status_.error = error;
    return;
  }

  Status found;
  found.duration = env->CallIntMethod(player_, c.duration) / 1000.0;
  found.width = env->CallIntMethod(player_, c.width);
  found.height = env->CallIntMethod(player_, c.height);
  auto tracks = static_cast<jobjectArray>(env->CallObjectMethod(player_, c.track_info));
  java::failed(env);
  const jsize count = tracks ? env->GetArrayLength(tracks) : 0;
  jstring mime_key = env->NewStringUTF("mime"), channels_key = env->NewStringUTF("channel-count");
  for (jsize i = 0; i < count; ++i) {
    jobject track = env->GetObjectArrayElement(tracks, i);
    const int type = env->CallIntMethod(track, c.track_type);
    jobject format = env->CallObjectMethod(track, c.track_format);
    java::failed(env);
    std::string mime;
    int channels = 0;
    if (format) {
      mime = java::text(env, static_cast<jstring>(env->CallObjectMethod(format, c.format_string, mime_key)));
      if (env->CallBooleanMethod(format, c.format_has, channels_key))
        channels = env->CallIntMethod(format, c.format_integer, channels_key);
      java::failed(env);
      env->DeleteLocalRef(format);
    }
    if (type == kTrackVideo && found.video_codec.empty()) found.video_codec = codec_name(mime);
    if (type == kTrackAudio) {
      Track audio;
      audio.index = i;
      audio.codec = codec_name(mime);
      audio.channels = channels;
      auto language = static_cast<jstring>(env->CallObjectMethod(track, c.track_language));
      audio.language = java::text(env, language);
      if (audio.language == "und") audio.language.clear();
      env->DeleteLocalRef(language);
      found.audio_tracks.push_back(audio);
    }
    env->DeleteLocalRef(track);
  }
  env->DeleteLocalRef(mime_key);
  env->DeleteLocalRef(channels_key);
  if (tracks) env->DeleteLocalRef(tracks);
  found.audio_track = env->CallIntMethod(player_, c.selected_track, kTrackAudio);
  java::failed(env);
  if (!found.audio_tracks.empty()) found.audio_codec = found.audio_tracks.front().codec;
  {
    std::lock_guard lock(mutex_);
    status_.duration = found.duration;
    status_.width = found.width;
    status_.height = found.height;
    status_.video_codec = found.video_codec;
    status_.audio_codec = found.audio_codec;
    status_.audio_tracks = std::move(found.audio_tracks);
    status_.audio_track = found.audio_track;
  }
  prepared_ = true;
}

bool Player::call(const char* name) {
  JNIEnv* env = java::env();
  const Classes& c = classes(env);
  const jmethodID method = !std::strcmp(name, "start") ? c.start : c.pause;
  env->CallVoidMethod(player_, method);
  std::string error;
  if (!java::failed(env, &error)) return true;
  std::lock_guard lock(mutex_);
  status_.state = State::failed;
  status_.error = error;
  return false;
}

void Player::apply_volume() {
  if (!started_) return;
  JNIEnv* env = java::env();
  const float gain = muted_ || !audio_output_ ? 0.0f : volume_;
  env->CallVoidMethod(player_, classes(env).set_volume, gain, gain);
  java::failed(env);
}

void Player::set_paused(bool paused, std::int64_t) {
  if (paused == paused_) return;
  paused_ = paused;
  if (started_) call(paused ? "pause" : "start");
}

void Player::seek(double seconds) {
  if (!started_) {
    pending_seek_ = std::max(seconds, 0.0);
    return;
  }
  JNIEnv* env = java::env();
  env->CallVoidMethod(player_, classes(env).seek, static_cast<jlong>(std::max(seconds, 0.0) * 1000), kSeekClosestSync);
  java::failed(env);
  last_position_ms_ = -1;
}

void Player::set_volume(float volume) {
  volume_ = std::clamp(volume, 0.0f, 1.0f);
  apply_volume();
}

void Player::set_muted(bool muted) {
  muted_ = muted;
  apply_volume();
}

void Player::set_audio_output(bool on) {
  audio_output_ = on;
  apply_volume();
}

void Player::set_loop(bool loop) {
  loop_ = loop;
  if (!started_) return;
  JNIEnv* env = java::env();
  env->CallVoidMethod(player_, classes(env).set_looping, static_cast<jboolean>(loop));
  java::failed(env);
}

void Player::select_audio_track(int index) {
  if (!started_ || index < 0) return;
  JNIEnv* env = java::env();
  env->CallVoidMethod(player_, classes(env).select_track, index);
  if (java::failed(env)) return;
  std::lock_guard lock(mutex_);
  status_.audio_track = index;
}

void Player::update(std::int64_t now_us, std::int64_t) {
  if (!prepared_) return;
  {
    std::lock_guard lock(mutex_);
    if (status_.state == State::failed) return;
  }
  JNIEnv* env = java::env();
  const Classes& c = classes(env);
  if (!started_) {
    started_ = true;
    {
      std::lock_guard lock(mutex_);
      shown_.width = status_.width;
      shown_.height = status_.height;
    }
    set_loop(loop_);
    apply_volume();
    if (pending_seek_ >= 0) seek(pending_seek_);
    if (!paused_ && !call("start")) return;
  }
  const int position = env->CallIntMethod(player_, c.position);
  const bool playing = env->CallBooleanMethod(player_, c.playing);
  if (java::failed(env)) return;
  // A playing stream whose position stands still is waiting for data.
  if (!playing || position != last_position_ms_) stalled_since_us_ = now_us;
  last_position_ms_ = position;
  std::lock_guard lock(mutex_);
  status_.position = position / 1000.0;
  status_.paused = paused_;
  const bool ended = !paused_ && !playing && !loop_ && status_.duration > 0 && position >= status_.duration * 1000 - 500;
  status_.state = ended ? State::ended : now_us - stalled_since_us_ > 400000 ? State::buffering : State::ready;
}

Status Player::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

} // namespace video

// The presenter's video layers on Android: the SurfaceTexture, latched once per present, drawn through
// the element's fit and clip.
bool GlVideo::init(bool) {
  if (program) return true;
  GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source), fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
  if (vertex && fragment) {
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
      glDeleteProgram(program);
      program = 0;
    }
  }
  if (vertex) glDeleteShader(vertex);
  if (fragment) glDeleteShader(fragment);
  glGenVertexArrays(1, &vao);
  return program != 0;
}

void GlVideo::release() {
  end_present();
  if (program) glDeleteProgram(program);
  if (vao) glDeleteVertexArrays(1, &vao);
  program = vao = 0;
}

void GlVideo::draw(std::uint64_t, const video::Frame& frame, Fit fit, const Target& target, int x, int y, int w,
                   int h, int fb_w, int fb_h, int, const Compare&) {
  auto* picture = static_cast<video::Player::Picture*>(frame.owner.get());
  if (!program || !picture || !picture->native || frame.width <= 0 || frame.height <= 0) return;
  if (picture->latched != presents + 1) {
    ASurfaceTexture_updateTexImage(picture->native);
    ASurfaceTexture_getTransformMatrix(picture->native, picture->transform);
    picture->latched = presents + 1;
  }
  // As gl_video.cpp fits the picture into the element's box.
  const float bw = float(target.x1 - target.x0), bh = float(target.y1 - target.y0);
  if (bw <= 0 || bh <= 0) return;
  const float shape = frame.width * frame.aspect / frame.height, box = bw / bh;
  float sx0 = 0, sy0 = 0, sx1 = 1, sy1 = 1;
  if (fit == Fit::contain) {
    if (shape > box) {
      const float part = shape / box;
      sy0 = 0.5f - part / 2;
      sy1 = 0.5f + part / 2;
    } else {
      const float part = box / shape;
      sx0 = 0.5f - part / 2;
      sx1 = 0.5f + part / 2;
    }
  } else if (fit == Fit::cover) {
    if (shape > box) {
      const float part = box / shape;
      sx0 = (1 - part) / 2;
      sx1 = sx0 + part;
    } else {
      const float part = shape / box;
      sy0 = (1 - part) / 2;
      sy1 = sy0 + part;
    }
  }
  // GL counts scissor rows from the bottom of the window.
  const float sx = float(w) / fb_w, sy = float(h) / fb_h;
  const int cx0 = x + int(target.clip_x0 * sx + 0.5f), cx1 = x + int(target.clip_x1 * sx + 0.5f);
  const int cy0 = y + h - int(target.clip_y1 * sy + 0.5f), cy1 = y + h - int(target.clip_y0 * sy + 0.5f);
  if (cx1 <= cx0 || cy1 <= cy0) return;
  glEnable(GL_SCISSOR_TEST);
  glScissor(cx0, cy0, cx1 - cx0, cy1 - cy0);
  glUseProgram(program);
  glBindVertexArray(vao);
  glUniform4f(glGetUniformLocation(program, "dst"), float(target.x0), float(target.y0), float(target.x1),
              float(target.y1));
  glUniform4f(glGetUniformLocation(program, "src"), sx0, sy0, sx1, sy1);
  glUniform2f(glGetUniformLocation(program, "size"), float(fb_w), float(fb_h));
  glUniformMatrix4fv(glGetUniformLocation(program, "transform"), 1, GL_FALSE, picture->transform);
  glUniform1f(glGetUniformLocation(program, "alpha"), target.alpha);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, picture->texture);
  glUniform1i(glGetUniformLocation(program, "picture"), 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  glDisable(GL_SCISSOR_TEST);
}

void GlVideo::end_present() {
  ++presents;
  std::lock_guard lock(retired_lock);
  if (!retired.empty()) glDeleteTextures(static_cast<GLsizei>(retired.size()), retired.data());
  retired.clear();
}

std::string GlVideo::take_upscale_log() { return {}; }
void GlVideo::dump_upscale_at(const std::string&, std::uint64_t) {}

// No compute upscalers on Android yet: setUpscale('') is the only model.
namespace upscalers {
int find(const char*) { return kNone; }
const char* name(int) { return ""; }
int count() { return 0; }
std::string names() { return ""; }
} // namespace upscalers
