#pragma once

#include "ImageRowReducer.h"
#include <cstring>

namespace image_decode::detail {

inline bool isGif(std::span<const std::byte> bytes) {
  return bytes.size() >= 6 &&
         (std::memcmp(bytes.data(), "GIF87a", 6) == 0 ||
          std::memcmp(bytes.data(), "GIF89a", 6) == 0);
}

// Only the first frame is used by chart artwork. Decode its palette indices
// directly into the reducer, without stb's source-sized animation/history buffers.
inline std::optional<DecodedImageData>
decodeGifRows(std::span<const std::byte> bytes, const ImageDecodeOptions &options) {
  if (!isGif(bytes) || bytes.size() > options.maximumEncodedBytes ||
      options.stop.stop_requested()) return std::nullopt;
  std::size_t cursor = 6;
  bool valid = true;
  const auto get = [&]() -> int {
    if (cursor >= bytes.size()) { valid = false; return 0; }
    return std::to_integer<unsigned char>(bytes[cursor++]);
  };
  const auto word = [&]() { const int low = get(); return low | (get() << 8); };
  const auto skip = [&](std::size_t count) {
    if (count > bytes.size() - cursor) { valid = false; return; }
    cursor += count;
  };
  const auto blocks = [&] {
    while (valid) {
      if (options.stop.stop_requested()) { valid = false; return; }
      const int length = get();
      if (!length) break;
      skip(length);
    }
  };
  const int width = word(), height = word(), flags = get(), background = get();
  get();
  if (!valid || width <= 0 || height <= 0 || width > options.maximumDimension ||
      height > options.maximumDimension ||
      static_cast<std::uint64_t>(width) * height > options.maximumDecodedBytes / 4 ||
      static_cast<std::uint64_t>(width) * height > SIZE_MAX / 4) return std::nullopt;
  using Palette = std::array<std::array<unsigned char, 4>, 256>;
  Palette global{};
  const auto palette = [&](Palette &table, int count) {
    for (int i = 0; i < count; ++i) {
      for (int c = 0; c < 3; ++c) table[i][c] = static_cast<unsigned char>(get());
      table[i][3] = 255;
    }
  };
  const int globalCount = flags & 128 ? 2 << (flags & 7) : 0;
  palette(global, globalCount);
  int transparent = -1;
  while (valid && !options.stop.stop_requested()) {
    const int tag = get();
    if (tag == 0x21) {
      const int extension = get();
      if (extension == 0xf9) {
        if (get() != 4) return std::nullopt;
        const int control = get(); word();
        const int index = get();
        transparent = control & 1 ? index : -1;
        if (get() != 0) return std::nullopt;
      } else blocks();
      continue;
    }
    if (tag != 0x2c) return std::nullopt;
    const int left = word(), top = word(), frameWidth = word(), frameHeight = word();
    const int frameFlags = get();
    if (!valid || frameWidth <= 0 || frameHeight <= 0 ||
        left + frameWidth > width || top + frameHeight > height) return std::nullopt;
    Palette colors = global;
    const int colorCount = frameFlags & 128 ? 2 << (frameFlags & 7) : globalCount;
    if (frameFlags & 128) palette(colors, colorCount);
    if (!valid || colorCount == 0) return std::nullopt;
    const bool interlaced = (frameFlags & 64) != 0;
    ImageRowReducer reducer(width, height, options, interlaced);
    std::array<unsigned char, 4> backdrop{};
    if (background > 0 && background < globalCount) {
      // Preserve stb's first-frame untouched-canvas palette byte order.
      backdrop = {global[background][2], global[background][1],
                   global[background][0], 255};
    }
    const auto fillRow = [&](int y, int begin, int end) {
      for (int x = begin; x < end; ++x) reducer.add(x, y, backdrop);
    };
    for (int y = 0; y < top; ++y) {
      if (options.stop.stop_requested()) return std::nullopt;
      fillRow(y, 0, width);
    }
    std::vector<int> rows;
    rows.reserve(frameHeight);
    if (interlaced) {
      for (const auto [start, step] : {std::pair{0, 8}, {4, 8}, {2, 4}, {1, 2}})
        for (int y = start; y < frameHeight; y += step) rows.push_back(top + y);
    } else {
      for (int y = 0; y < frameHeight; ++y) rows.push_back(top + y);
    }
    const int minimumBits = get();
    if (!valid || minimumBits < 2 || minimumBits > 8) return std::nullopt;
    const int clear = 1 << minimumBits, end = clear + 1;
    std::array<int, 4096> prefix{};
    std::array<unsigned char, 4096> suffix{}, stack{};
    for (int i = 0; i < clear; ++i) suffix[i] = static_cast<unsigned char>(i);
    int codeBits = minimumBits + 1, next = end + 1, previous = -1;
    int subblock = 0, availableBits = 0;
    unsigned bits = 0;
    std::size_t emitted = 0;
    const auto pixels = static_cast<std::size_t>(frameWidth) * frameHeight;
    const auto emit = [&](int index) {
      if (emitted >= pixels || index >= colorCount) return false;
      const int x = static_cast<int>(emitted % frameWidth);
      const int y = rows[emitted / frameWidth];
      if (x == 0) fillRow(y, 0, left);
      reducer.add(left + x, y, index == transparent
          ? std::array<unsigned char, 4>{} : colors[index]);
      if (x + 1 == frameWidth) fillRow(y, left + frameWidth, width);
      ++emitted;
      return true;
    };
    for (;;) {
      if (!valid || options.stop.stop_requested()) return std::nullopt;
      while (availableBits < codeBits) {
        if (subblock == 0) {
          subblock = get();
          if (!valid || subblock == 0) return std::nullopt;
        }
        bits |= static_cast<unsigned>(get()) << availableBits;
        --subblock;
        availableBits += 8;
      }
      if (!valid) return std::nullopt;
      int code = static_cast<int>(bits & ((1U << codeBits) - 1));
      bits >>= codeBits;
      availableBits -= codeBits;
      if (code == clear) {
        codeBits = minimumBits + 1; next = end + 1; previous = -1;
        continue;
      }
      if (code == end) {
        skip(subblock); blocks();
        if (!valid || emitted != pixels) return std::nullopt;
        for (int y = top + frameHeight; y < height; ++y) {
          if (options.stop.stop_requested()) return std::nullopt;
          fillRow(y, 0, width);
        }
        return reducer.finish();
      }
      const int original = code;
      std::size_t length = 0;
      if (code > next || (code == next && previous < 0)) return std::nullopt;
      if (code == next) {
        code = previous;
        int first = code;
        while (first >= clear) {
          if (first >= next || prefix[first] >= first) return std::nullopt;
          first = prefix[first];
        }
        stack[length++] = suffix[first];
      }
      while (code >= clear) {
        if (code <= end || code >= next || prefix[code] >= code ||
            length >= stack.size()) return std::nullopt;
        stack[length++] = suffix[code]; code = prefix[code];
      }
      if (length >= stack.size()) return std::nullopt;
      const int first = suffix[code];
      stack[length++] = static_cast<unsigned char>(first);
      while (length) if (!emit(stack[--length])) return std::nullopt;
      if (previous >= 0 && next < 4096) {
        prefix[next] = previous; suffix[next] = static_cast<unsigned char>(first);
        if (++next == (1 << codeBits) && codeBits < 12) ++codeBits;
      }
      previous = original;
    }
  }
  return std::nullopt;
}
}
