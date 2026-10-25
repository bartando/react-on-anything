// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "annex_b.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {
constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
}

namespace video {

bool AnnexB::start(const AVCodecParameters* parameters) {
  hevc_ = parameters->codec_id == AV_CODEC_ID_HEVC;
  length_size_ = 0;
  sets_.clear();
  const std::uint8_t* data = parameters->extradata;
  const int size = parameters->extradata_size;
  if ((parameters->codec_id != AV_CODEC_ID_H264 && !hevc_) || !data || size < 7 || data[0] != 1) return false;
  // One list of NAL units: a 16-bit length before each.
  const auto read = [&](int& at, int count) {
    for (int n = 0; n < count && at + 2 <= size; ++n) {
      const int bytes = data[at] << 8 | data[at + 1];
      if (at + 2 + bytes > size) return false;
      sets_.insert(sets_.end(), kStartCode, kStartCode + 4);
      sets_.insert(sets_.end(), data + at + 2, data + at + 2 + bytes);
      at += 2 + bytes;
    }
    return true;
  };
  int at;
  if (hevc_) {
    if (size < 23) return false;
    length_size_ = (data[21] & 3) + 1;
    const int arrays = data[22];
    at = 23;
    for (int a = 0; a < arrays && at + 3 <= size; ++a) {
      const int count = data[at + 1] << 8 | data[at + 2];
      at += 3;
      if (!read(at, count)) break;
    }
  } else {
    length_size_ = (data[4] & 3) + 1;
    // The SPS count in 5 bits, then the PPS count in 8.
    at = 6;
    if (read(at, data[5] & 31) && at < size) {
      const int pps = data[at++];
      read(at, pps);
    }
  }
  return true;
}

bool AnnexB::convert(const AVPacket* packet, std::vector<std::uint8_t>& out) const {
  out.clear();
  const std::uint8_t* data = packet->data;
  const int size = packet->size;
  bool inserted = false;
  for (int at = 0; at + length_size_ <= size;) {
    std::uint32_t bytes = 0;
    for (int i = 0; i < length_size_; ++i) bytes = bytes << 8 | data[at + i];
    at += length_size_;
    if (bytes == 0 || bytes > static_cast<std::uint32_t>(size - at)) return bytes == 0 && at == size;
    const int type = hevc_ ? (data[at] >> 1) & 0x3f : data[at] & 0x1f;
    const bool keyframe = hevc_ ? type >= 16 && type <= 23 : type == 5;
    if (keyframe && !inserted) {
      out.insert(out.begin(), sets_.begin(), sets_.end());
      inserted = true;
    }
    out.insert(out.end(), kStartCode, kStartCode + 4);
    out.insert(out.end(), data + at, data + at + bytes);
    at += static_cast<int>(bytes);
  }
  return true;
}

} // namespace video
