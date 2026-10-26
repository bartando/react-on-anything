// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// H.264 and HEVC for hardware decoders, which take Annex B with the parameter sets in band; MP4 and
// Matroska store them length-prefixed, the sets apart in the codec's header (avcC, hvcC). The header's
// sets go at the start of each keyframe's packet, so sets the packet carries itself come after and win:
// some files' headers hold stale ones, which FFmpeg 7.1's hevc_mp4toannexb puts right before the slice
// instead, and the picture decodes as garbage.
#pragma once

#include <cstdint>
#include <vector>

struct AVCodecParameters;
struct AVPacket;

namespace video {

class AnnexB {
public:
  // Reads the header's sets; false if the stream is Annex B already (MPEG-TS) or has no header.
  bool start(const AVCodecParameters* parameters);
  // The packet as Annex B into `out`; false if its lengths run past it.
  bool convert(const AVPacket* packet, std::vector<std::uint8_t>& out) const;
  bool active() const { return length_size_ != 0; }

private:
  int length_size_ = 0;
  bool hevc_ = false;
  std::vector<std::uint8_t> sets_;
};

} // namespace video
