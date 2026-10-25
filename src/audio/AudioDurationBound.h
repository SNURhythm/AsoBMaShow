#pragma once

#include "VorbisDurationBound.h"
#include <algorithm>
#include <cstring>

namespace audio::duration_bound {
struct Duration {
  std::uint64_t frames;
  int sampleRate;
};

// No decoder setup or unbounded scans. WAV payloads are skipped by offset;
// FLAC needs only STREAMINFO. Ogg reads at most 64 KiB plus its identification page.
// readAt(offset, bytes) must fill the requested span or return false.
template <typename ReadAt>
std::optional<Duration> InspectAudio(std::uint64_t size, ReadAt readAt) {
  std::array<unsigned char, 42> header{};
  const auto headerSize = static_cast<std::size_t>(std::min<std::uint64_t>(size, header.size()));
  if (headerSize < 12 || !readAt(0, std::span(header).first(headerSize))) return {};
  const auto u16 = [](const unsigned char *p) {
    return unsigned(p[0]) | unsigned(p[1]) << 8;
  };
  const auto u32 = [](const unsigned char *p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
           std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
  };
  if (std::memcmp(header.data(), "fLaC", 4) == 0) {
    if (headerSize != 42 || (header[4] & 0x7f) != 0 ||
        header[5] != 0 || header[6] != 0 || header[7] != 34) return {};
    std::uint64_t packed = 0;
    for (std::size_t i = 18; i < 26; ++i) packed = (packed << 8) | header[i];
    const auto rate = static_cast<int>(packed >> 44);
    const auto frames = packed & 0xfffffffffULL;
    // A zero total-sample field means unknown, not an empty FLAC stream.
    if (rate <= 0 || frames == 0) return {};
    return Duration{frames, rate};
  }
  if (std::memcmp(header.data(), "RIFF", 4) == 0 &&
      std::memcmp(header.data() + 8, "WAVE", 4) == 0) {
    const auto end = std::uint64_t(u32(header.data() + 4)) + 8;
    // libsndfile may inspect chunks beyond a short declared RIFF boundary.
    // Such trailing content cannot support a proven bound without validation.
    if (end < 12 || end != size) return {};
    unsigned alignment = 0, rate = 0;
    std::uint64_t dataBytes = 0;
    bool foundData = false;
    std::uint64_t offset = 12;
    for (unsigned chunks = 0; offset < end && chunks < 64; ++chunks) {
      std::array<unsigned char, 8> chunk{};
      if (end - offset < chunk.size() || !readAt(offset, std::span(chunk))) return {};
      const auto bytes = u32(chunk.data() + 4);
      offset += chunk.size();
      if (bytes > end - offset) return {};
      if (std::memcmp(chunk.data(), "fmt ", 4) == 0) {
        std::array<unsigned char, 16> format{};
        if (alignment || bytes < format.size() || !readAt(offset, std::span(format))) return {};
        const auto encoding = u16(format.data());
        const auto channels = u16(format.data() + 2);
        const auto bits = u16(format.data() + 14);
        alignment = u16(format.data() + 12);
        rate = u32(format.data() + 4);
        if ((encoding != 1 && encoding != 3) || channels == 0 || channels > 64 ||
            rate == 0 || rate > 768000 ||
            (encoding == 1 ? (bits != 8 && bits != 16 && bits != 24 && bits != 32)
                           : (bits != 32 && bits != 64)) ||
            alignment != channels * (bits / 8)) return {};
      } else if (std::memcmp(chunk.data(), "data", 4) == 0) {
        foundData = true;
        dataBytes += bytes;
      }
      offset += std::uint64_t(bytes) + (bytes & 1U);
    }
    if (offset != end || !alignment || !foundData || dataBytes % alignment != 0) return {};
    return Duration{dataBytes / alignment, static_cast<int>(rate)};
  }
  if (std::memcmp(header.data(), "OggS", 4) == 0) {
    std::array<unsigned char, 64U * 1024U> bytes;
    if (size <= bytes.size()) {
      const auto file = std::span(bytes).first(static_cast<std::size_t>(size));
      if (!readAt(0, file)) return {};
      if (const auto bound = InspectVorbis(file)) return Duration{bound->frames, bound->sampleRate};
    } else {
      // An Ogg page is at most 65,307 bytes, so the final page fits this tail.
      std::array<unsigned char, 58> first{};
      if (!readAt(0, std::span(first)) || !readAt(size - bytes.size(), std::span(bytes))) return {};
      if (const auto bound = InspectVorbisEnds(first, bytes))
        return Duration{bound->frames, bound->sampleRate};
    }
  }
  return {};
}

inline std::optional<Duration> InspectAudio(std::span<const unsigned char> bytes) {
  return InspectAudio(bytes.size(), [&](std::uint64_t offset, std::span<unsigned char> output) {
    if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
    std::copy_n(bytes.data() + offset, output.size(), output.data());
    return true;
  });
}
} // namespace audio::duration_bound
