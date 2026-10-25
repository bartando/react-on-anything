// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "media_codec.hpp"
#include "video_frame.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <media/NdkMediaFormat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <cstdlib>
#include <thread>

namespace {
constexpr std::int64_t kInputWaitUs = 10000;
// How long send() waits for an input buffer before calling the codec stuck.
constexpr int kInputWaits = 500;
// Images the reader lends out at once: the frames queued ahead (kMaxFrames), the one showing and the
// one before it, which the display may still be reading, and room to decode into.
constexpr std::int32_t kImages = 10;

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

// MediaCodec takes H.264 and HEVC as Annex B; MP4 and Matroska store them length-prefixed, with the
// parameter sets apart in the codec's header (avcC, hvcC). AnnexB converts packets and puts the header's
// sets at the start of each keyframe's packet, so sets the packet carries itself come after and win:
// some files' headers hold stale ones, which FFmpeg 7.1's filter puts right before the slice instead.
struct AnnexB {
  int length_size = 0; // 0: the packets are Annex B already
  bool hevc = false;
  std::vector<std::uint8_t> sets;

  void start(const AVCodecParameters* parameters) {
    hevc = parameters->codec_id == AV_CODEC_ID_HEVC;
    const std::uint8_t* data = parameters->extradata;
    const int size = parameters->extradata_size;
    if (!data || size < 7 || data[0] != 1) return; // none, or Annex B already
    const auto add = [&](const std::uint8_t* nal, int bytes) {
      static const std::uint8_t start[] = {0, 0, 0, 1};
      sets.insert(sets.end(), start, start + 4);
      sets.insert(sets.end(), nal, nal + bytes);
    };
    int at;
    if (hevc) {
      if (size < 23) return;
      length_size = (data[21] & 3) + 1;
      const int arrays = data[22];
      at = 23;
      for (int a = 0; a < arrays && at + 3 <= size; ++a) {
        const int count = data[at + 1] << 8 | data[at + 2];
        at += 3;
        for (int n = 0; n < count && at + 2 <= size; ++n) {
          const int bytes = data[at] << 8 | data[at + 1];
          if (at + 2 + bytes > size) return;
          add(data + at + 2, bytes);
          at += 2 + bytes;
        }
      }
    } else {
      length_size = (data[4] & 3) + 1;
      at = 5;
      for (int list = 0; list < 2 && at < size; ++list) {
        const int count = list ? data[at] : data[at] & 31; // SPS count in 5 bits, PPS count in 8
        ++at;
        for (int n = 0; n < count && at + 2 <= size; ++n) {
          const int bytes = data[at] << 8 | data[at + 1];
          if (at + 2 + bytes > size) return;
          add(data + at + 2, bytes);
          at += 2 + bytes;
        }
      }
    }
  }

  // The packet as Annex B into `out`; false if its lengths run past it.
  bool convert(const AVPacket* packet, std::vector<std::uint8_t>& out) const {
    out.clear();
    const std::uint8_t* data = packet->data;
    const int size = packet->size;
    bool inserted = false;
    for (int at = 0; at + length_size <= size;) {
      std::uint32_t bytes = 0;
      for (int i = 0; i < length_size; ++i) bytes = bytes << 8 | data[at + i];
      at += length_size;
      if (bytes == 0 || bytes > static_cast<std::uint32_t>(size - at)) return bytes == 0 && at == size;
      const int type = hevc ? (data[at] >> 1) & 0x3f : data[at] & 0x1f;
      const bool keyframe = hevc ? type >= 16 && type <= 23 : type == 5;
      if (keyframe && !inserted) {
        out.insert(out.begin(), sets.begin(), sets.end());
        inserted = true;
      }
      static const std::uint8_t start[] = {0, 0, 0, 1};
      out.insert(out.end(), start, start + 4);
      out.insert(out.end(), data + at, data + at + bytes);
      at += static_cast<int>(bytes);
    }
    return true;
  }
};

// The pictures' colours as the stream tags them, for the display.
video::LayerColor color_of(const AVCodecParameters* parameters) {
  video::LayerColor color;
  const bool bt2020 = parameters->color_primaries == AVCOL_PRI_BT2020 ||
                      parameters->color_space == AVCOL_SPC_BT2020_NCL || parameters->color_space == AVCOL_SPC_BT2020_CL;
  const bool pq = parameters->color_trc == AVCOL_TRC_SMPTE2084, hlg = parameters->color_trc == AVCOL_TRC_ARIB_STD_B67;
  // Untagged: by size, as the other hosts' players guess.
  const bool sd = parameters->color_space == AVCOL_SPC_BT470BG || parameters->color_space == AVCOL_SPC_SMPTE170M ||
                  (parameters->color_space == AVCOL_SPC_UNSPECIFIED && parameters->height < 720);
  const int standard = bt2020 ? ADATASPACE_STANDARD_BT2020 : sd ? ADATASPACE_STANDARD_BT601_625 : ADATASPACE_STANDARD_BT709;
  const int transfer = pq ? ADATASPACE_TRANSFER_ST2084 : hlg ? ADATASPACE_TRANSFER_HLG : ADATASPACE_TRANSFER_SMPTE_170M;
  const int range = parameters->color_range == AVCOL_RANGE_JPEG ? ADATASPACE_RANGE_FULL : ADATASPACE_RANGE_LIMITED;
  color.space = static_cast<ADataSpace>(standard | transfer | range);
  color.hdr = pq || hlg;
  const AVPacketSideData* side = parameters->coded_side_data;
  const int count = parameters->nb_coded_side_data;
  if (const AVPacketSideData* found = av_packet_side_data_get(side, count, AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
    const auto* m = reinterpret_cast<const AVMasteringDisplayMetadata*>(found->data);
    const auto point = [](const AVRational xy[2]) {
      return AColor_xy{static_cast<float>(av_q2d(xy[0])), static_cast<float>(av_q2d(xy[1]))};
    };
    if (m->has_primaries && m->has_luminance) {
      // FFmpeg orders the primaries red, green, blue.
      color.mastering.displayPrimaryRed = point(m->display_primaries[0]);
      color.mastering.displayPrimaryGreen = point(m->display_primaries[1]);
      color.mastering.displayPrimaryBlue = point(m->display_primaries[2]);
      color.mastering.whitePoint = point(m->white_point);
      color.mastering.maxLuminance = static_cast<float>(av_q2d(m->max_luminance));
      color.mastering.minLuminance = static_cast<float>(av_q2d(m->min_luminance));
      color.has_mastering = true;
    }
  }
  if (const AVPacketSideData* found = av_packet_side_data_get(side, count, AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
    const auto* l = reinterpret_cast<const AVContentLightMetadata*>(found->data);
    color.light_level = {static_cast<float>(l->MaxCLL), static_cast<float>(l->MaxFALL)};
    color.has_light_level = true;
  }
  return color;
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
    if (parameters->codec_id == AV_CODEC_ID_H264 || parameters->codec_id == AV_CODEC_ID_HEVC) annex_b_.start(parameters);

    auto output = std::make_shared<video::CodecOutput>();
    output->color = color_of(parameters);
    // Private: the decoder picks the layout, 10-bit included, and the display reads it as it is.
    if (AImageReader_newWithUsage(width_, height_, AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
                                  kImages, &output->reader) != AMEDIA_OK)
      return;
    ANativeWindow* window = nullptr;
    if (AImageReader_getWindow(output->reader, &window) != AMEDIA_OK || !window) return;
    output_ = output;

    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) return;
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width_);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height_);
    // A 4K keyframe outgrows the default input buffer.
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                          std::max(width_ * height_ * 3 / 4, 2 * 1024 * 1024));
    if (!annex_b_.length_size && parameters->codec_id != AV_CODEC_ID_H264 && parameters->codec_id != AV_CODEC_ID_HEVC &&
        parameters->extradata_size > 0)
      AMediaFormat_setBuffer(format, "csd-0", parameters->extradata, static_cast<std::size_t>(parameters->extradata_size));
    const media_status_t configured = AMediaCodec_configure(codec, format, window, nullptr, 0);
    AMediaFormat_delete(format);
    if (configured != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
      std::fprintf(stderr, "[video] MediaCodec refused %s %dx%d\n", mime, width_, height_);
      AMediaCodec_delete(codec);
      return;
    }
    output_->codec = codec;
    // PS5_REACT_VIDEO_DUMP=<directory>: the first packets as the codec gets them, to decode elsewhere.
    if (const char* directory = std::getenv("PS5_REACT_VIDEO_DUMP")) {
      static int dumps = 0;
      const std::string path = std::string(directory) + "/video-" + std::to_string(++dumps) + "." +
                               (parameters->codec_id == AV_CODEC_ID_HEVC ? "hevc" : "h264");
      dump_ = std::fopen(path.c_str(), "wb");
      std::fprintf(stderr, "[video] dumping %s to %s\n", mime, path.c_str());
    }
    char* name = nullptr;
    if (AMediaCodec_getName(codec, &name) == AMEDIA_OK && name) {
      name_ = std::string("MediaCodec ") + name;
      AMediaCodec_releaseName(codec, name);
    }
  }

  ~MediaCodecDecoder() override {
    ready_.clear();
    if (dump_) std::fclose(dump_);
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
    if (!annex_b_.length_size) return feed(packet->data, packet->size, packet);
    if (!annex_b_.convert(packet, converted_)) return true; // skipped, as a corrupt packet is
    return feed(converted_.data(), static_cast<int>(converted_.size()), packet);
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
    AMediaCodec_flush(output_->codec);
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

  bool feed(const std::uint8_t* data, int size, const AVPacket* packet) {
    const int index = input();
    if (index < 0) return false;
    std::size_t capacity = 0;
    std::uint8_t* buffer = AMediaCodec_getInputBuffer(output_->codec, static_cast<std::size_t>(index), &capacity);
    if (!buffer || capacity < static_cast<std::size_t>(size)) {
      std::fprintf(stderr, "[video] MediaCodec input of %zu bytes for a %d-byte packet\n", capacity, size);
      return false;
    }
    std::memcpy(buffer, data, static_cast<std::size_t>(size));
    if (dump_ && dumped_++ < 1500) std::fwrite(data, 1, static_cast<std::size_t>(size), dump_);
    const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    const std::int64_t us = ts == AV_NOPTS_VALUE ? last_us_ : av_rescale_q(ts, time_base_, AVRational{1, 1000000});
    last_us_ = us;
    return AMediaCodec_queueInputBuffer(output_->codec, static_cast<std::size_t>(index), 0,
                                        static_cast<std::size_t>(size), static_cast<std::uint64_t>(us),
                                        0) == AMEDIA_OK;
  }

  // Takes one finished picture, if the codec has one within `timeout_us`.
  void drain(std::int64_t timeout_us) {
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(output_->codec, &info, timeout_us);
    if (index < 0) return; // nothing yet, or a format or buffer change the image's crop covers
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ended_ = true;
    if (info.size <= 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
      AMediaCodec_releaseOutputBuffer(output_->codec, static_cast<std::size_t>(index), false);
      return;
    }
    // Into the reader, then out of it: the picture reaches it through the codec's thread.
    const std::int64_t stamp = ++rendered_;
    AMediaCodec_releaseOutputBufferAtTime(output_->codec, static_cast<std::size_t>(index), stamp);
    AImage* image = nullptr;
    for (int tries = 0; tries < 200 && !image; ++tries) {
      AImage* next = nullptr;
      const media_status_t taken = AImageReader_acquireNextImage(output_->reader, &next);
      if (taken == AMEDIA_OK) {
        std::int64_t at = 0;
        AImage_getTimestamp(next, &at);
        if (at == stamp) image = next;
        else AImage_delete(next); // one rendered before a flush
        continue;
      }
      if (taken != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!image) {
      std::fprintf(stderr, "[video] MediaCodec's picture did not reach the reader\n");
      return;
    }
    auto picture = std::make_shared<video::LayerFrame>();
    picture->output = output_;
    picture->image = image;
    AImage_getHardwareBuffer(image, &picture->buffer);
    AImageCropRect crop{};
    const bool cropped = AImage_getCropRect(image, &crop) == AMEDIA_OK && crop.right > crop.left && crop.bottom > crop.top;
    picture->width = cropped ? crop.right - crop.left : width_;
    picture->height = cropped ? crop.bottom - crop.top : height_;
    video::Frame frame;
    frame.width = width_;
    frame.height = height_;
    frame.aspect = aspect_;
    frame.pts_us = info.presentationTimeUs;
    frame.transfer = output_->color.space & ADATASPACE_TRANSFER_ST2084 ? video::Transfer::pq
                   : output_->color.hdr ? video::Transfer::hlg : video::Transfer::sdr;
    frame.owner = std::move(picture);
    ready_.push_back(std::move(frame));
  }

  AVRational time_base_;
  AnnexB annex_b_;
  std::vector<std::uint8_t> converted_;
  std::shared_ptr<video::CodecOutput> output_;
  std::deque<video::Frame> ready_;
  std::string name_ = "MediaCodec";
  int width_ = 0, height_ = 0;
  float aspect_ = 1;
  std::int64_t last_us_ = 0, rendered_ = 0;
  bool eos_sent_ = false, ended_ = false;
  FILE* dump_ = nullptr;
  int dumped_ = 0;
};
} // namespace

namespace video {

CodecOutput::~CodecOutput() {
  if (codec) {
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
  }
  if (reader) AImageReader_delete(reader);
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
