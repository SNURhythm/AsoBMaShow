#pragma once

#include "ImageFileDecoder.h"
#include "ImageRowReducer.h"

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <vector>

namespace image_decode::detail {

inline bool isPsd(std::span<const std::byte> encoded) {
  return encoded.size() >= 4 && encoded[0] == std::byte{'8'} &&
         encoded[1] == std::byte{'B'} && encoded[2] == std::byte{'P'} &&
         encoded[3] == std::byte{'S'};
}

inline bool decodePsdPackBitsRow(std::span<const std::byte> encoded,
                                 std::span<unsigned char> row,
                                 const ImageDecodeOptions &options) {
  std::size_t input = 0, output = 0;
  while (input < encoded.size()) {
    if (options.stop.stop_requested()) return false;
    const auto control = std::to_integer<unsigned>(encoded[input++]);
    if (control == 128) continue;
    const std::size_t count = control < 128 ? control + 1 : 257 - control;
    if (count > row.size() - output) return false;
    if (control < 128) {
      if (count > encoded.size() - input) return false;
      for (std::size_t i = 0; i < count; ++i) {
        row[output++] = std::to_integer<unsigned char>(encoded[input++]);
      }
    } else {
      if (input == encoded.size()) return false;
      const auto value = std::to_integer<unsigned char>(encoded[input++]);
      std::fill_n(row.begin() + output, count, value);
      output += count;
    }
  }
  return output == row.size();
}

inline std::optional<DecodedImageData>
decodePsdRows(std::span<const std::byte> encoded,
               const ImageDecodeOptions &options) {
  if (options.stop.stop_requested() || !isPsd(encoded) || encoded.size() < 26 ||
      encoded.size() > options.maximumEncodedBytes) return std::nullopt;
  const auto u16 = [&](std::size_t offset) {
    return (std::to_integer<unsigned>(encoded[offset]) << 8) |
           std::to_integer<unsigned>(encoded[offset + 1]);
  };
  const auto u32 = [&](std::size_t offset) {
    return (static_cast<std::uint32_t>(u16(offset)) << 16) | u16(offset + 2);
  };
  const auto channels = u16(12), depth = u16(22);
  const auto sourceHeight = u32(14), sourceWidth = u32(18);
  if (u16(4) != 1 || channels > 16 || (depth != 8 && depth != 16) ||
      u16(24) != 3 || sourceWidth == 0 || sourceHeight == 0 ||
      options.maximumDimension <= 0 ||
      sourceWidth > static_cast<unsigned>(options.maximumDimension) ||
      sourceHeight > static_cast<unsigned>(options.maximumDimension)) {
    return std::nullopt;
  }
  const auto pixels = static_cast<std::uint64_t>(sourceWidth) * sourceHeight;
  if (pixels > options.maximumDecodedBytes / 4 ||
      pixels > std::numeric_limits<std::size_t>::max() / 4) return std::nullopt;

  std::size_t offset = 26;
  // Color-mode data, image resources, and layer/mask data are independent of
  // the final merged RGB image. Skip them with checked lengths.
  for (int section = 0; section < 3; ++section) {
    if (encoded.size() - offset < 4) return std::nullopt;
    const auto bytes = u32(offset);
    offset += 4;
    if (bytes > encoded.size() - offset) return std::nullopt;
    offset += bytes;
  }
  if (encoded.size() - offset < 2) return std::nullopt;
  const auto compression = u16(offset);
  offset += 2;
  if (compression > 1) return std::nullopt;

  const std::size_t sampleBytes = depth / 8;
  const std::size_t rowBytes = static_cast<std::size_t>(sourceWidth) * sampleBytes;
  std::array<std::size_t, 4> channelOffsets{};
  std::size_t rowTable = 0;
  if (compression == 0) {
    const auto planeBytes = static_cast<std::size_t>(pixels) * sampleBytes;
    if (channels != 0 && planeBytes > (encoded.size() - offset) / channels) {
      return std::nullopt;
    }
    for (unsigned channel = 0; channel < std::min(channels, 4U); ++channel) {
      channelOffsets[channel] = offset + channel * planeBytes;
    }
  } else {
    const auto tableBytes = static_cast<std::uint64_t>(sourceHeight) * channels * 2;
    if (tableBytes > encoded.size() - offset) return std::nullopt;
    rowTable = offset;
    offset += static_cast<std::size_t>(tableBytes);
    // The row byte counts already index the compressed data. Four cursors are
    // enough to revisit the channel rows in pixel order without a full-image
    // pixel buffer or a height-sized auxiliary row index.
    for (unsigned channel = 0; channel < channels; ++channel) {
      if (channel < 4) channelOffsets[channel] = offset;
      for (std::uint32_t y = 0; y < sourceHeight; ++y) {
        if (options.stop.stop_requested()) return std::nullopt;
        const auto rowIndex = static_cast<std::size_t>(channel) * sourceHeight + y;
        const auto bytes = u16(rowTable + rowIndex * 2);
        if (bytes > encoded.size() - offset) return std::nullopt;
        offset += bytes;
      }
    }
  }

  const int width = static_cast<int>(sourceWidth), height = static_cast<int>(sourceHeight);
  ImageRowReducer reducer(width, height, options);
  std::vector<unsigned char> nativeRow(compression ? rowBytes : 0);
  std::vector<std::array<unsigned char, 4>> rgbaRow(sourceWidth);
  for (int y = 0; y < height; ++y) {
    if (options.stop.stop_requested()) return std::nullopt;
    std::fill(rgbaRow.begin(), rgbaRow.end(), std::array<unsigned char, 4>{0, 0, 0, 255});
    for (unsigned channel = 0; channel < std::min(channels, 4U); ++channel) {
      if (options.stop.stop_requested()) return std::nullopt;
      if (compression) {
        const auto rowIndex = static_cast<std::size_t>(channel) * sourceHeight + y;
        const auto bytes = u16(rowTable + rowIndex * 2);
        if (!decodePsdPackBitsRow(encoded.subspan(channelOffsets[channel], bytes),
                                   nativeRow, options)) return std::nullopt;
        channelOffsets[channel] += bytes;
        for (int x = 0; x < width; ++x) {
          rgbaRow[x][channel] = nativeRow[static_cast<std::size_t>(x) * sampleBytes];
        }
      } else {
        const auto row = encoded.subspan(channelOffsets[channel], rowBytes);
        channelOffsets[channel] += rowBytes;
        for (int x = 0; x < width; ++x) {
          rgbaRow[x][channel] = std::to_integer<unsigned char>(
              row[static_cast<std::size_t>(x) * sampleBytes]);
        }
      }
    }
    for (int x = 0; x < width; ++x) {
      if (options.stop.stop_requested()) return std::nullopt;
      auto &pixel = rgbaRow[x];
      // Match stb's removal of the white matte, after selecting the high byte
      // of 16-bit input. Zero/full alpha needs no adjustment.
      if (channels >= 4 && pixel[3] != 0 && pixel[3] != 255) {
        const float alpha = pixel[3] / 255.0f;
        const float reciprocal = 1.0f / alpha;
        const float inverse = 255.0f * (1 - reciprocal);
        for (int channel = 0; channel < 3; ++channel) {
          pixel[channel] = static_cast<unsigned char>(
              static_cast<int>(pixel[channel] * reciprocal + inverse));
        }
      }
      reducer.add(x, y, pixel);
    }
  }
  return reducer.finish();
}

} // namespace image_decode::detail
