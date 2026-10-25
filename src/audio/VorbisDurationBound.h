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

namespace detail {
inline std::uint32_t oggU32(const unsigned char *bytes) {
  return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
      std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
}
struct OggPage {
  std::size_t header, body;
  unsigned flags, segments;
  std::uint32_t serial, sequence;
  std::uint64_t granule;
};
inline std::optional<OggPage> inspectOggPage(std::span<const unsigned char> bytes) {
  if (bytes.size() < 27 || bytes[0] != 'O' || bytes[1] != 'g' ||
      bytes[2] != 'g' || bytes[3] != 'S' || bytes[4] != 0 || (bytes[5] & ~7U)) return {};
  const auto segments = bytes[26];
  const auto header = 27U + segments;
  if (bytes.size() < header) return {};
  std::size_t body = 0;
  for (std::size_t i = 0; i < segments; ++i) body += bytes[27 + i];
  const auto size = header + body;
  if (bytes.size() < size) return {};
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
  std::uint32_t crc = 0;
  for (std::size_t i = 0; i < size; ++i) {
    const auto byte = i >= 22 && i < 26 ? 0U : bytes[i];
    crc = (crc << 8) ^ crcTable[(crc >> 24) ^ byte];
  }
  if (crc != oggU32(bytes.data() + 22)) return {};
  return OggPage{header, body, bytes[5], segments, oggU32(bytes.data() + 14),
      oggU32(bytes.data() + 18), std::uint64_t(oggU32(bytes.data() + 6)) |
          std::uint64_t(oggU32(bytes.data() + 10)) << 32};
}
inline std::optional<int> vorbisRate(std::span<const unsigned char> bytes, const OggPage &page) {
  if (page.flags != 2 || page.granule != 0 || page.sequence != 0 ||
      page.segments != 1 || page.body != 30) return {};
  const auto packet = page.header;
  constexpr std::array<unsigned char, 7> magic{1, 'v', 'o', 'r', 'b', 'i', 's'};
  for (std::size_t i = 0; i < magic.size(); ++i)
    if (bytes[packet + i] != magic[i]) return {};
  const auto sampleRate = oggU32(bytes.data() + packet + 12);
  const auto block = bytes[packet + 28];
  if (oggU32(bytes.data() + packet + 7) != 0 || bytes[packet + 11] == 0 ||
      bytes[packet + 11] > 64 || sampleRate == 0 ||
      sampleRate > std::numeric_limits<int>::max() ||
      (block & 15) < 6 || (block >> 4) > 13 ||
      (block & 15) > (block >> 4) || !(bytes[packet + 29] & 1)) return {};
  return static_cast<int>(sampleRate);
}
} // namespace detail

// Conservative duration of a complete, single-stream Ogg/Vorbis file. Unknown
// or malformed containers must fall back to the normal decoder.
inline std::optional<Vorbis> InspectVorbis(std::span<const unsigned char> bytes) {
  std::size_t offset = 0;
  std::uint32_t serial = 0, sequence = 0;
  std::uint64_t lastGranule = 0;
  int rate = 0;
  bool continued = false;
  while (offset < bytes.size()) {
    const auto data = bytes.subspan(offset);
    const auto page = detail::inspectOggPage(data);
    if (!page) return {};
    if (offset == 0) {
      const auto sampleRate = detail::vorbisRate(data, *page);
      if (!sampleRate) return {};
      rate = *sampleRate;
      serial = page->serial;
    } else if (page->serial != serial || page->sequence != sequence ||
               (page->flags & 2) || bool(page->flags & 1) != continued) {
      return {};
    }
    if (page->granule != std::numeric_limits<std::uint64_t>::max()) {
      if (page->granule > std::numeric_limits<std::int64_t>::max() || page->granule < lastGranule)
        return {};
      lastGranule = page->granule;
    }
    if (page->segments) continued = data[page->header - 1] == 255;
    offset += page->header + page->body;
    ++sequence;
    if (page->flags & 4) {
      if (offset != bytes.size() || sequence < 3 || continued ||
          page->granule == std::numeric_limits<std::uint64_t>::max()) return {};
      // libsndfile subtracts a nonnegative initial PCM offset from the final
      // granule. This is an upper bound, never a replacement for exact duration.
      return Vorbis{page->granule, rate};
    }
  }
  return {};
}

// Large seekable files need only the identification page and final page, not
// setup/codebooks or intervening payloads. Match libsndfile's last granule for
// the identification stream; foreign/chained tails and trailing junk are unknown.
inline std::optional<Vorbis> InspectVorbisEnds(std::span<const unsigned char> first,
                                              std::span<const unsigned char> tail) {
  const auto head = detail::inspectOggPage(first);
  const auto rate = head ? detail::vorbisRate(first, *head) : std::optional<int>{};
  if (!rate || tail.size() < 27) return {};
  std::optional<std::size_t> terminal;
  unsigned candidates = 0;
  for (std::size_t offset = tail.size() - 27 + 1; offset-- > 0;) {
    const auto bytes = tail.subspan(offset);
    if (bytes[0] != 'O' || bytes[1] != 'g' || bytes[2] != 'g' || bytes[3] != 'S') continue;
    if (++candidates > 64) return {};
    const auto header = 27U + bytes[26];
    if (bytes.size() < header) continue;
    std::size_t size = header;
    for (std::size_t i = 27; i < header; ++i) size += bytes[i];
    if (size != bytes.size()) continue;
    // Nested terminal-looking headers are ambiguous. Do not mistake payload
    // bytes for a later, shorter stream; also bound forged-signature work.
    if (terminal) return {};
    terminal = offset;
  }
  if (!terminal) return {};
  const auto bytes = tail.subspan(*terminal);
  // Exactly one CRC check, independent of payload/header candidates.
  const auto page = detail::inspectOggPage(bytes);
  if (!page || !(page->flags & 4) || (page->flags & 2) || page->serial != head->serial ||
      page->sequence < 2 || !page->segments || bytes[page->header - 1] == 255 ||
      page->granule > std::numeric_limits<std::int64_t>::max()) return {};
  return Vorbis{page->granule, *rate};
}
} // namespace audio::duration_bound
