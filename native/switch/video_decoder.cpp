// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The Switch's hardware decoder: FFmpeg's own decoders with devkitPro's nvtegra hwaccel, so NVDEC decodes
// the slices while FFmpeg parses the stream (no Annex B conversion, the stream's parameter sets as they
// come). Each picture is copied out by VIC, the video image compositor, into planes the presenter
// uploads: VIC takes the copy only into 256-aligned pitches and addresses, and does a 4K one in about
// 9 ms where the CPU took 22.
#include "video_frame.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {
constexpr AVRational kMicroseconds = {1, 1000000};
constexpr int kPitchAlign = 256;
constexpr std::size_t kPageAlign = 0x1000;

// Plane buffers for VIC's copies, kept for reuse: a frame's planes go back when the presenter is done.
class PlanePool {
public:
  ~PlanePool() {
    for (void* buffer : free_) std::free(buffer);
  }
  void* take(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    if (bytes != bytes_) {
      for (void* buffer : free_) std::free(buffer);
      free_.clear();
      bytes_ = bytes;
    }
    if (!free_.empty()) {
      void* buffer = free_.back();
      free_.pop_back();
      return buffer;
    }
    return std::aligned_alloc(kPageAlign, bytes);
  }
  void give(void* buffer, std::size_t bytes) {
    std::lock_guard lock(mutex_);
    if (bytes == bytes_) free_.push_back(buffer);
    else std::free(buffer);
  }

private:
  std::mutex mutex_;
  std::size_t bytes_ = 0;
  std::vector<void*> free_;
};

// A frame's planes, returned to the pool with it.
struct Planes {
  std::shared_ptr<PlanePool> pool;
  void* data[2] = {};
  std::size_t bytes[2] = {};
  ~Planes() {
    for (int i = 0; i < 2; ++i)
      if (data[i]) pool->give(data[i], bytes[i]);
  }
};

std::size_t round_up(std::size_t value, std::size_t to) { return (value + to - 1) / to * to; }

video::Matrix frame_matrix(const AVFrame* frame) {
  switch (frame->colorspace) {
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: return video::Matrix::bt2020;
    case AVCOL_SPC_BT709: return video::Matrix::bt709;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_FCC: return video::Matrix::bt601;
    default: return frame->height >= 720 ? video::Matrix::bt709 : video::Matrix::bt601;
  }
}

video::Transfer frame_transfer(const AVFrame* frame) {
  return frame->color_trc == AVCOL_TRC_SMPTE2084 ? video::Transfer::pq
       : frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ? video::Transfer::hlg : video::Transfer::sdr;
}

AVPixelFormat pick_nvtegra(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
    if (*format == AV_PIX_FMT_NVTEGRA) return *format;
  // A stream NVDEC does not take (a profile it lacks) decodes in software, in FFmpeg's first choice.
  std::fprintf(stderr, "[video] NVDEC does not take this stream: %s in software\n", av_get_pix_fmt_name(formats[0]));
  return formats[0];
}

class NvdecDecoder final : public video::Decoder {
public:
  NvdecDecoder(const AVCodecParameters* parameters, AVRational time_base) {
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec || !(context_ = avcodec_alloc_context3(codec))) return;
    avcodec_parameters_to_context(context_, parameters);
    context_->pkt_timebase = time_base;
    // A hwaccel decodes one picture at a time; frame threads would only queue them.
    context_->thread_count = 1;
    const int made = av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
    if (made < 0) {
      std::fprintf(stderr, "[video] no NVDEC device: %s\n", av_err2str(made));
      avcodec_free_context(&context_);
      return;
    }
    context_->hw_device_ctx = av_buffer_ref(device_);
    context_->get_format = pick_nvtegra;
    if (avcodec_open2(context_, codec, nullptr) < 0) avcodec_free_context(&context_);
    frame_ = av_frame_alloc();
  }
  ~NvdecDecoder() override {
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
    av_buffer_unref(&device_);
  }
  bool ok() const { return context_ && frame_; }

  bool send(const AVPacket* packet) override {
    const int result = avcodec_send_packet(context_, packet);
    return result >= 0 || result == AVERROR_INVALIDDATA || result == AVERROR_EOF || result == AVERROR(EAGAIN);
  }

  bool receive(video::Frame& out, bool& ended) override {
    const int result = avcodec_receive_frame(context_, frame_);
    if (result == AVERROR_EOF) ended = true;
    if (result < 0) return false;
    const bool taken = frame_->format == AV_PIX_FMT_NVTEGRA ? copy_out(out) : software(out);
    if (taken) {
      out.matrix = frame_matrix(frame_);
      out.full_range = frame_->color_range == AVCOL_RANGE_JPEG;
      out.transfer = frame_transfer(frame_);
      out.aspect = frame_->sample_aspect_ratio.num > 0 && frame_->sample_aspect_ratio.den > 0
        ? static_cast<float>(av_q2d(frame_->sample_aspect_ratio)) : 1.0f;
      const std::int64_t ts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
      out.pts_us = ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, context_->pkt_timebase, kMicroseconds);
    }
    av_frame_unref(frame_);
    return taken;
  }

  void flush() override { avcodec_flush_buffers(context_); }
  const char* name() const override { return "NVDEC"; }

private:
  // VIC copies the picture into pooled planes: NV12, or P010 for 10-bit streams.
  bool copy_out(video::Frame& out) {
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame_->hw_frames_ctx->data);
    const AVPixelFormat format = frames->sw_format;
    if (format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_P010) {
      std::fprintf(stderr, "[video] NVDEC frames in %s are not drawn\n", av_get_pix_fmt_name(format));
      return false;
    }
    const int bytes = format == AV_PIX_FMT_P010 ? 2 : 1;
    const int pitch = static_cast<int>(round_up(static_cast<std::size_t>(frame_->width) * bytes, kPitchAlign));
    const std::size_t rows = round_up(static_cast<std::size_t>(frame_->height), kPitchAlign);
    auto planes = std::make_shared<Planes>();
    planes->pool = pool_;
    AVFrame* copy = av_frame_alloc();
    if (!copy) return false;
    copy->format = format;
    copy->width = frame_->width;
    copy->height = frame_->height;
    for (int plane = 0; plane < 2; ++plane) {
      planes->bytes[plane] = round_up(static_cast<std::size_t>(pitch) * (plane ? rows / 2 : rows), kPageAlign);
      planes->data[plane] = pool_->take(planes->bytes[plane]);
      if (!planes->data[plane]) {
        av_frame_free(&copy);
        return false;
      }
      copy->data[plane] = static_cast<std::uint8_t*>(planes->data[plane]);
      copy->linesize[plane] = pitch;
      // nvtegra checks the planes' alignment through their buffers and copies on the CPU without them;
      // these only borrow the pool's memory, which `planes` gives back.
      copy->buf[plane] = av_buffer_create(copy->data[plane], planes->bytes[plane], [](void*, std::uint8_t*) {},
                                          nullptr, 0);
      if (!copy->buf[plane]) {
        av_frame_free(&copy);
        return false;
      }
    }
    const int copied = av_hwframe_transfer_data(copy, frame_, 0);
    av_frame_free(&copy);
    if (copied < 0) {
      std::fprintf(stderr, "[video] VIC copy failed: %s\n", av_err2str(copied));
      return false;
    }
    out.format = format == AV_PIX_FMT_P010 ? video::Format::p010 : video::Format::nv12;
    out.width = frame_->width;
    out.height = frame_->height;
    for (int plane = 0; plane < 2; ++plane) {
      out.planes[plane] = static_cast<const std::uint8_t*>(planes->data[plane]);
      out.pitch[plane] = pitch;
    }
    out.planes[2] = nullptr;
    out.pitch[2] = 0;
    out.owner = std::move(planes);
    return true;
  }

  // A picture NVDEC left to FFmpeg's software decoder, in a layout the presenter draws as it is.
  bool software(video::Frame& out) {
    switch (frame_->format) {
      case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: out.format = video::Format::i420; break;
      case AV_PIX_FMT_NV12: out.format = video::Format::nv12; break;
      case AV_PIX_FMT_YUV420P10LE: out.format = video::Format::i420_10; break;
      case AV_PIX_FMT_P010LE: out.format = video::Format::p010; break;
      default: return false;
    }
    AVFrame* kept = av_frame_clone(frame_);
    if (!kept) return false;
    out.width = kept->width;
    out.height = kept->height;
    for (int i = 0; i < 3; ++i) {
      out.planes[i] = kept->data[i];
      out.pitch[i] = kept->linesize[i];
    }
    out.owner = std::shared_ptr<void>(kept, [](void* frame) {
      AVFrame* f = static_cast<AVFrame*>(frame);
      av_frame_free(&f);
    });
    return true;
  }

  AVCodecContext* context_ = nullptr;
  AVBufferRef* device_ = nullptr;
  AVFrame* frame_ = nullptr;
  std::shared_ptr<PlanePool> pool_ = std::make_shared<PlanePool>();
};
} // namespace

void video::preload_platform_decoder() {}

// FFmpeg in software for what NVDEC does not decode (AV1 through dav1d), up to 1080p: four Cortex-A57s
// do not keep up past it.
std::int64_t video::software_pixel_limit() { return 1920 * 1088; }

std::unique_ptr<video::Decoder> video::make_platform_decoder(const AVCodecParameters* parameters, int time_base_num,
                                                             int time_base_den) {
  switch (parameters->codec_id) {
    case AV_CODEC_ID_H264: case AV_CODEC_ID_HEVC: case AV_CODEC_ID_VP9: case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_MPEG2VIDEO: case AV_CODEC_ID_MPEG4: case AV_CODEC_ID_VC1: break;
    default: return nullptr;
  }
  auto decoder = std::make_unique<NvdecDecoder>(parameters, AVRational{time_base_num, time_base_den});
  if (!decoder->ok()) return nullptr;
  return decoder;
}
