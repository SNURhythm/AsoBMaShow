#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace audio::duration_bound {
struct Vorbis {
  std::uint64_t frames;
  int sampleRate;
};

// Conservative duration of a complete, single-stream Ogg/Vorbis file. Unknown
// or malformed containers must fall back to the normal decoder.
inline std::optional<Vorbis> InspectVorbis(std::span<const unsigned char> bytes) {
  static constexpr auto crcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
      auto value = i << 24;
      for (int bit = 0; bit < 8; ++bit)
        value = (value << 1) ^ ((value & 0x80000000U) ? 0x04c11db7U : 0U);
      table[i] = value;
    }
    return table;
  }();
  const auto u32 = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
        static_cast<std::uint32_t>(bytes[offset + 2]) << 16 |
        static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
  };
  std::size_t offset = 0;
  std::uint32_t serial = 0, sequence = 0;
  std::uint64_t lastGranule = 0;
  int rate = 0;
  bool continued = false;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 27 || bytes[offset] != 'O' ||
        bytes[offset + 1] != 'g' || bytes[offset + 2] != 'g' ||
        bytes[offset + 3] != 'S' || bytes[offset + 4] != 0)
      return std::nullopt;
    const auto flags = bytes[offset + 5];
    const auto segments = bytes[offset + 26];
    const auto header = 27U + segments;
    if (bytes.size() - offset < header || (flags & ~7U)) return std::nullopt;
    std::size_t body = 0;
    for (std::size_t i = 0; i < segments; ++i) body += bytes[offset + 27 + i];
    const auto size = header + body;
    if (bytes.size() - offset < size) return std::nullopt;
    std::uint32_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
      const auto byte = i >= 22 && i < 26 ? 0U : bytes[offset + i];
      crc = (crc << 8) ^ crcTable[(crc >> 24) ^ byte];
    }
    if (crc != u32(offset + 22)) return std::nullopt;
    const auto granule = static_cast<std::uint64_t>(u32(offset + 6)) |
                         static_cast<std::uint64_t>(u32(offset + 10)) << 32;
    if (offset == 0) {
      // Vorbis identification is the only packet on its BOS page. Do not parse
      // setup/codebooks: they are the expensive part of opening each decoder.
      if (flags != 2 || granule != 0 || u32(offset + 18) != 0 ||
          segments != 1 || body != 30) return std::nullopt;
      const auto packet = offset + header;
      constexpr std::array<unsigned char, 7> magic{1, 'v', 'o', 'r', 'b', 'i', 's'};
      for (std::size_t i = 0; i < magic.size(); ++i)
        if (bytes[packet + i] != magic[i]) return std::nullopt;
      const auto sampleRate = u32(packet + 12);
      const auto block = bytes[packet + 28];
      if (u32(packet + 7) != 0 || bytes[packet + 11] == 0 ||
          bytes[packet + 11] > 64 || sampleRate == 0 ||
          sampleRate > std::numeric_limits<int>::max() ||
          (block & 15) < 6 || (block >> 4) > 13 ||
          (block & 15) > (block >> 4) || !(bytes[packet + 29] & 1))
        return std::nullopt;
      rate = static_cast<int>(sampleRate);
      serial = u32(offset + 14);
    } else if (u32(offset + 14) != serial || u32(offset + 18) != sequence ||
               (flags & 2) || bool(flags & 1) != continued) {
      return std::nullopt;
    }
    if (granule != std::numeric_limits<std::uint64_t>::max()) {
      if (granule > std::numeric_limits<std::int64_t>::max() || granule < lastGranule)
        return std::nullopt;
      lastGranule = granule;
    }
    if (segments) continued = bytes[offset + header - 1] == 255;
    offset += size;
    ++sequence;
    if (flags & 4) {
      if (offset != bytes.size() || sequence < 3 || continued ||
          granule == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
      // libsndfile subtracts a nonnegative initial PCM offset from the final
      // granule. This is an upper bound, never a replacement for exact duration.
      return Vorbis{granule, rate};
    }
  }
  return std::nullopt;
}
} // namespace audio::duration_bound
