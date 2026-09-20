#pragma once

#include "ImageRowReducer.h"
#include <cstring>

namespace image_decode::detail {
inline bool isPic(std::span<const std::byte> bytes) {
  return bytes.size() >= 92 && bytes[0] == std::byte{0x53} &&
      bytes[1] == std::byte{0x80} && bytes[2] == std::byte{0xf6} &&
      bytes[3] == std::byte{0x34} && std::memcmp(bytes.data() + 88, "PICT", 4) == 0;
}
inline std::optional<DecodedImageData>
decodePicRows(std::span<const std::byte> bytes, const ImageDecodeOptions &options) {
  if (!isPic(bytes) || bytes.size() < 104 || bytes.size() > options.maximumEncodedBytes ||
      options.stop.stop_requested()) return std::nullopt;
  std::size_t cursor = 92;
  bool valid = true;
  const auto get = [&]() -> int {
    if (cursor >= bytes.size()) { valid = false; return 0; }
    return std::to_integer<unsigned char>(bytes[cursor++]);
  };
  const auto word = [&]() { const int high = get(); return (high << 8) | get(); };
  const int width = word(), height = word();
  cursor = 104;
  if (width <= 0 || height <= 0 || width > options.maximumDimension ||
      height > options.maximumDimension ||
      static_cast<std::uint64_t>(width) * height > options.maximumDecodedBytes / 4 ||
      static_cast<std::uint64_t>(width) * height > SIZE_MAX / 4) return std::nullopt;
  struct Packet { int type, channels; };
  std::array<Packet, 10> packets{};
  int count = 0, chained;
  do {
    if (count == static_cast<int>(packets.size())) return std::nullopt;
    chained = get();
    const int depth = get(), type = get(), channels = get();
    if (!valid || depth != 8 || type > 2) return std::nullopt;
    packets[count++] = {type, channels};
  } while (chained);
  ImageRowReducer reducer(width, height, options);
  std::vector<std::array<unsigned char, 4>> row(width);
  const auto value = [&](int channels, std::array<unsigned char, 4> &pixel) {
    for (int c = 0; c < 4; ++c)
      if (channels & (128 >> c)) pixel[c] = static_cast<unsigned char>(get());
  };
  for (int y = 0; y < height; ++y) {
    if (options.stop.stop_requested()) return std::nullopt;
    std::fill(row.begin(), row.end(), std::array<unsigned char, 4>{255, 255, 255, 255});
    for (int p = 0; p < count; ++p) {
      const auto packet = packets[p];
      int x = 0;
      while (x < width) {
        if (!valid || options.stop.stop_requested()) return std::nullopt;
        int run = 1;
        bool repeat = packet.type == 1;
        if (packet.type == 1) run = std::min(get(), width - x);
        else if (packet.type == 2) {
          const int control = get();
          repeat = control >= 128;
          run = control == 128 ? word() : repeat ? control - 127 : control + 1;
        }
        if (!valid || run <= 0 || run > width - x) return std::nullopt;
        std::array<unsigned char, 4> pixel{};
        if (repeat) value(packet.channels, pixel);
        for (int i = 0; i < run; ++i) {
          if (repeat) {
            for (int c = 0; c < 4; ++c)
              if (packet.channels & (128 >> c)) row[x][c] = pixel[c];
          } else value(packet.channels, row[x]);
          ++x;
        }
      }
    }
    if (!valid) return std::nullopt;
    for (int x = 0; x < width; ++x) reducer.add(x, y, row[x]);
  }
  return reducer.finish();
}
}
