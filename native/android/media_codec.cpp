// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "media_codec.hpp"
#include "java.hpp"
#include "video_frame.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <android/native_window_jni.h>
#include <android/surface_texture_jni.h>
#include <media/NdkMediaFormat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <time.h>

namespace {
constexpr std::int64_t kInputWaitUs = 10000;
// How long send() waits for an input buffer before calling the codec stuck.
constexpr int kInputWaits = 500;

const char* mime_of(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264: return "video/avc";
    case AV_CODEC_ID_HEVC: return "video/hevc";
    case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_AV1: return "video/av01";
    case AV_CODEC_ID_MPEG2VIDEO: return "video/mpeg2";
    case AV_CODEC_ID_MPEG4: return "video/mp4v-es";
    default: return nullptr;
  }
}

// MediaCodec takes H.264 and HEVC as Annex B, parameter sets in band; MP4 and Matroska store them apart.
const char* filter_of(AVCodecID id) {
  return id == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb" : nullptr;
}

std::int64_t monotonic_ns() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

struct Classes {
  jclass surface_texture, surface;
  jmethodID texture_new, texture_release, surface_new, surface_release;
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
    c.surface_texture = global("android/graphics/SurfaceTexture");
    c.surface = global("android/view/Surface");
    // Detached: the GL thread attaches it to a texture of its own on the first draw.
    c.texture_new = env->GetMethodID(c.surface_texture, "<init>", "(Z)V");
    c.texture_release = env->GetMethodID(c.surface_texture, "release", "()V");
    c.surface_new = env->GetMethodID(c.surface, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    c.surface_release = env->GetMethodID(c.surface, "release", "()V");
    return c;
  }();
  return c;
}

class MediaCodecDecoder final : public video::Decoder {
public:
  MediaCodecDecoder(const AVCodecParameters* parameters, int time_base_num, int time_base_den)
      : time_base_{time_base_num, time_base_den} {
    const char* mime = mime_of(parameters->codec_id);
    if (!mime) return;
    width_ = parameters->width;
    height_ = parameters->height;
    if (parameters->sample_aspect_ratio.num > 0 && parameters->sample_aspect_ratio.den > 0)
      aspect_ = static_cast<float>(av_q2d(parameters->sample_aspect_ratio));
    if (const char* filter_name = filter_of(parameters->codec_id)) {
      const AVBitStreamFilter* filter = av_bsf_get_by_name(filter_name);
      if (!filter || av_bsf_alloc(filter, &filter_) < 0) return;
      avcodec_parameters_copy(filter_->par_in, parameters);
      filter_->time_base_in = time_base_;
      if (av_bsf_init(filter_) < 0) return;
    }

    JNIEnv* env = java::env();
    const Classes& c = classes(env);
    auto output = std::make_shared<video::CodecOutput>();
    jobject texture = env->NewObject(c.surface_texture, c.texture_new, JNI_FALSE);
    jobject surface = texture && !java::failed(env) ? env->NewObject(c.surface, c.surface_new, texture) : nullptr;
    if (java::failed(env) || !texture || !surface) {
      if (texture) env->DeleteLocalRef(texture);
      return;
    }
    output->surface_texture = env->NewGlobalRef(texture);
    output->surface = env->NewGlobalRef(surface);
    output->native = ASurfaceTexture_fromSurfaceTexture(env, texture);
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    env->DeleteLocalRef(texture);
    env->DeleteLocalRef(surface);
    output_ = output;
    if (!window) return;

    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) {
      ANativeWindow_release(window);
      return;
    }
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width_);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height_);
    // A 4K keyframe outgrows the default input buffer.
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                          std::max(width_ * height_ * 3 / 4, 2 * 1024 * 1024));
    if (!filter_ && parameters->extradata_size > 0)
      AMediaFormat_setBuffer(format, "csd-0", parameters->extradata, static_cast<std::size_t>(parameters->extradata_size));
    const media_status_t configured = AMediaCodec_configure(codec, format, window, nullptr, 0);
    AMediaFormat_delete(format);
    ANativeWindow_release(window);
    if (configured != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
      std::fprintf(stderr, "[video] MediaCodec refused %s %dx%d\n", mime, width_, height_);
      AMediaCodec_delete(codec);
      return;
    }
    output_->codec = codec;
    char* name = nullptr;
    if (AMediaCodec_getName(codec, &name) == AMEDIA_OK && name) {
      name_ = std::string("MediaCodec ") + name;
      AMediaCodec_releaseName(codec, name);
    }
  }

  ~MediaCodecDecoder() override {
    ready_.clear();
    if (filter_) av_bsf_free(&filter_);
  }

  bool ok() const { return output_ && output_->codec; }

  bool send(const AVPacket* packet) override {
    if (!packet) {
      const int index = input();
      if (index < 0) return false;
      AMediaCodec_queueInputBuffer(output_->codec, static_cast<std::size_t>(index), 0, 0, 0,
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      eos_sent_ = true;
      return true;
    }
    if (!filter_) return feed(packet);
    AVPacket* copy = av_packet_clone(packet);
    if (!copy || av_bsf_send_packet(filter_, copy) < 0) {
      av_packet_free(&copy);
      return true; // skipped, as a corrupt packet is
    }
    av_packet_free(&copy);
    AVPacket* filtered = av_packet_alloc();
    bool fed = true;
    while (fed && av_bsf_receive_packet(filter_, filtered) == 0) {
      fed = feed(filtered);
      av_packet_unref(filtered);
    }
    av_packet_free(&filtered);
    return fed;
  }

  bool receive(video::Frame& frame, bool& ended) override {
    // After the end of the stream nothing more comes in: wait out what the codec still holds.
    for (int waits = 0; ready_.empty() && !ended_ && waits < (eos_sent_ ? 100 : 1); ++waits)
      drain(eos_sent_ ? 10000 : 0);
    if (ready_.empty()) {
      ended = ended_;
      return false;
    }
    frame = std::move(ready_.front());
    ready_.pop_front();
    return true;
  }

  void flush() override {
    ready_.clear();
    {
      std::lock_guard lock(output_->lock);
      AMediaCodec_flush(output_->codec);
      ++output_->generation;
    }
    if (filter_) av_bsf_flush(filter_);
    eos_sent_ = ended_ = false;
  }

  const char* name() const override { return name_.c_str(); }

private:
  // An input buffer, collecting finished pictures while the codec has none free; -1 if it stays stuck.
  int input() {
    for (int waits = 0; waits < kInputWaits; ++waits) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(output_->codec, kInputWaitUs);
      if (index >= 0) return static_cast<int>(index);
      if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) return -1;
      drain(0);
    }
    std::fprintf(stderr, "[video] MediaCodec took no input for %llds\n",
                 static_cast<long long>(kInputWaits * kInputWaitUs / 1000000));
    return -1;
  }

  bool feed(const AVPacket* packet) {
    const int index = input();
    if (index < 0) return false;
    std::size_t capacity = 0;
    std::uint8_t* buffer = AMediaCodec_getInputBuffer(output_->codec, static_cast<std::size_t>(index), &capacity);
    if (!buffer || capacity < static_cast<std::size_t>(packet->size)) {
      std::fprintf(stderr, "[video] MediaCodec input of %zu bytes for a %d-byte packet\n", capacity, packet->size);
      return false;
    }
    std::memcpy(buffer, packet->data, static_cast<std::size_t>(packet->size));
    const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    const std::int64_t us = ts == AV_NOPTS_VALUE ? last_us_ : av_rescale_q(ts, time_base_, AVRational{1, 1000000});
    last_us_ = us;
    return AMediaCodec_queueInputBuffer(output_->codec, static_cast<std::size_t>(index), 0,
                                        static_cast<std::size_t>(packet->size), static_cast<std::uint64_t>(us),
                                        0) == AMEDIA_OK;
  }

  // Takes one finished picture, if the codec has one within `timeout_us`.
  void drain(std::int64_t timeout_us) {
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(output_->codec, &info, timeout_us);
    if (index < 0) return; // nothing yet, or a format or buffer change the texture's transform covers
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ended_ = true;
    if (info.size <= 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
      AMediaCodec_releaseOutputBuffer(output_->codec, static_cast<std::size_t>(index), false);
      return;
    }
    auto surface = std::make_shared<video::SurfaceFrame>();
    surface->output = output_;
    surface->index = static_cast<int>(index);
    {
      std::lock_guard lock(output_->lock);
      surface->generation = output_->generation;
    }
    video::Frame frame;
    frame.width = width_;
    frame.height = height_;
    frame.aspect = aspect_;
    frame.pts_us = info.presentationTimeUs;
    frame.owner = std::move(surface);
    ready_.push_back(std::move(frame));
  }

  AVRational time_base_;
  AVBSFContext* filter_ = nullptr;
  std::shared_ptr<video::CodecOutput> output_;
  std::deque<video::Frame> ready_;
  std::string name_ = "MediaCodec";
  int width_ = 0, height_ = 0;
  float aspect_ = 1;
  std::int64_t last_us_ = 0;
  bool eos_sent_ = false, ended_ = false;
};
} // namespace

namespace video {

CodecOutput::~CodecOutput() {
  if (codec) {
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
  }
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
  retire_texture(texture);
}

void CodecOutput::release(int index, std::uint64_t at_generation, std::int64_t render_ns) {
  std::lock_guard guard(lock);
  if (!codec || index < 0 || at_generation != generation) return;
  if (render_ns) AMediaCodec_releaseOutputBufferAtTime(codec, static_cast<std::size_t>(index), render_ns);
  else AMediaCodec_releaseOutputBuffer(codec, static_cast<std::size_t>(index), false);
}

void SurfaceFrame::render() {
  if (render_ns) return;
  render_ns = monotonic_ns();
  std::lock_guard guard(output->lock);
  if (!output->codec || generation != output->generation) {
    render_ns = -1;
    return;
  }
  AMediaCodec_releaseOutputBufferAtTime(output->codec, static_cast<std::size_t>(index), render_ns);
}

void preload_platform_decoder() {}

// No planar drawing on Android: every picture goes through MediaCodec.
std::int64_t software_pixel_limit() { return 0; }

std::unique_ptr<Decoder> make_platform_decoder(const AVCodecParameters* parameters, int time_base_num,
                                               int time_base_den) {
  auto decoder = std::make_unique<MediaCodecDecoder>(parameters, time_base_num, time_base_den);
  if (!decoder->ok()) return nullptr;
  return decoder;
}

} // namespace video
