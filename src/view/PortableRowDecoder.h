#pragma once

#include "ImageRowReducer.h"

#include <climits>
#include <cstring>
#include <string_view>

namespace image_decode::detail {

inline bool isPnm(std::span<const std::byte> encoded) {
  return encoded.size() >= 2 && encoded[0] == std::byte{'P'} &&
         (encoded[1] == std::byte{'5'} || encoded[1] == std::byte{'6'});
}

inline bool isHdr(std::span<const std::byte> encoded) {
  if (encoded.empty()) return false;
  const std::string_view bytes(reinterpret_cast<const char *>(encoded.data()),
                               encoded.size());
  return bytes.starts_with("#?RADIANCE\n") || bytes.starts_with("#?RGBE\n");
}

inline bool portableDimensionsValid(int width, int height,
                                     const ImageDecodeOptions &options) {
  if (width <= 0 || height <= 0 || options.maximumDimension <= 0 ||
      width > options.maximumDimension || height > options.maximumDimension) {
    return false;
  }
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  return pixels <= SIZE_MAX / 4 && pixels <= options.maximumDecodedBytes / 4;
}

inline bool portableWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\n' ||
         value == '\r' || value == '\v' || value == '\f';
}

inline bool portablePositiveInteger(std::string_view bytes,
                                    std::size_t &position, int &value,
                                    std::stop_token stop) {
  value = 0;
  const auto begin = position;
  while (position < bytes.size() && bytes[position] >= '0' &&
         bytes[position] <= '9') {
    if ((position & 4095) == 0 && stop.stop_requested()) return false;
    const int digit = bytes[position++] - '0';
    if (value > (INT_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return position != begin && value > 0;
}

inline std::optional<DecodedImageData>
decodePnmRows(std::span<const std::byte> encoded,
              const ImageDecodeOptions &options) {
  if (options.stop.stop_requested() || !isPnm(encoded) ||
      encoded.size() > options.maximumEncodedBytes) return std::nullopt;
  const std::string_view bytes(reinterpret_cast<const char *>(encoded.data()),
                               encoded.size());
  std::size_t position = 2;
  const auto skip = [&]() {
    while (position < bytes.size()) {
      if (options.stop.stop_requested()) return false;
      if (portableWhitespace(bytes[position])) {
        ++position;
      } else if (bytes[position] == '#') {
        do {
          if ((position & 4095) == 0 && options.stop.stop_requested()) return false;
          ++position;
        } while (position < bytes.size() && bytes[position] != '\n' &&
                 bytes[position] != '\r');
      } else {
        return true;
      }
    }
    return false;
  };
  int width = 0, height = 0, maximum = 0;
  if (!skip() || !portablePositiveInteger(bytes, position, width, options.stop) ||
      !skip() || !portablePositiveInteger(bytes, position, height, options.stop) ||
      !skip() || !portablePositiveInteger(bytes, position, maximum, options.stop) ||
      maximum > 65535 || position == bytes.size() ||
      !portableWhitespace(bytes[position]) ||
      !portableDimensionsValid(width, height, options)) return std::nullopt;
  // stb consumes exactly the one separator after MAXVAL. Further whitespace
  // belongs to the raster, and sample values are not rescaled by MAXVAL.
  ++position;
  const int channels = bytes[1] == '6' ? 3 : 1;
  const int sampleBytes = maximum > 255 ? 2 : 1;
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  const auto pixelBytes = static_cast<unsigned>(channels * sampleBytes);
  if (pixels > (encoded.size() - position) / pixelBytes) return std::nullopt;
  ImageRowReducer reducer(width, height, options);
  for (int y = 0; y < height; ++y) {
    if (options.stop.stop_requested()) return std::nullopt;
    for (int x = 0; x < width; ++x) {
      if ((x & 4095) == 0 && options.stop.stop_requested()) return std::nullopt;
      std::array<unsigned char, 4> rgba{0, 0, 0, 255};
      for (int channel = 0; channel < channels; ++channel) {
        if (sampleBytes == 2) {
          // Preserve bundled stb's native-endian PNM16 conversion, including
          // its high-byte truncation, instead of changing existing artwork.
          std::uint16_t sample = 0;
          std::memcpy(&sample, encoded.data() + position, sizeof(sample));
          rgba[channel] = static_cast<unsigned char>(sample >> 8);
        } else {
          rgba[channel] = std::to_integer<unsigned char>(encoded[position]);
        }
        position += sampleBytes;
      }
      if (channels == 1) rgba[1] = rgba[2] = rgba[0];
      reducer.add(x, y, rgba);
    }
  }
  return reducer.finish();
}

inline std::array<unsigned char, 4>
hdrRgba(const std::array<unsigned char, 4> &rgbe) {
  std::array<unsigned char, 4> rgba{0, 0, 0, 255};
  if (rgbe[3] == 0) return rgba;
  const float scale = static_cast<float>(std::ldexp(1.0, rgbe[3] - 136));
  for (int channel = 0; channel < 3; ++channel) {
    const float linear = rgbe[channel] * scale;
    // Match stb's default HDR-to-LDR gamma and float rounding before the
    // area reducer; averaging the linear HDR values would change output.
    const float value = static_cast<float>(std::pow(
                            static_cast<double>(linear),
                            static_cast<double>(1.0f / 2.2f))) * 255 + 0.5f;
    rgba[channel] = static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f));
  }
  return rgba;
}

inline std::optional<DecodedImageData>
decodeHdrRows(std::span<const std::byte> encoded,
              const ImageDecodeOptions &options) {
  if (options.stop.stop_requested() || !isHdr(encoded) ||
      encoded.size() > options.maximumEncodedBytes) return std::nullopt;
  const std::string_view bytes(reinterpret_cast<const char *>(encoded.data()),
                               encoded.size());
  std::size_t position = 0;
  const auto line = [&]() -> std::optional<std::string_view> {
    const auto start = position;
    while (position < bytes.size()) {
      if ((position & 4095) == 0 && options.stop.stop_requested()) return std::nullopt;
      if (bytes[position++] == '\n') return bytes.substr(start, position - start - 1);
    }
    return std::nullopt;
  };
  if (!line()) return std::nullopt;
  bool format = false;
  for (;;) {
    const auto token = line();
    if (!token) return std::nullopt;
    if (token->empty()) break;
    if (*token == "FORMAT=32-bit_rle_rgbe") format = true;
  }
  const auto layout = line();
  if (!format || !layout || !layout->starts_with("-Y ")) return std::nullopt;
  std::size_t cursor = 3;
  const auto dimension = [&](int &value) {
    while (cursor < layout->size() && portableWhitespace((*layout)[cursor])) ++cursor;
    if (cursor < layout->size() && (*layout)[cursor] == '+') ++cursor;
    return portablePositiveInteger(*layout, cursor, value, options.stop);
  };
  int width = 0, height = 0;
  if (!dimension(height)) return std::nullopt;
  while (cursor < layout->size() && (*layout)[cursor] == ' ') ++cursor;
  if (!layout->substr(cursor).starts_with("+X ")) return std::nullopt;
  cursor += 3;
  if (!dimension(width) || !portableDimensionsValid(width, height, options)) return std::nullopt;
  ImageRowReducer reducer(width, height, options);
  const auto flat = [&]() -> std::optional<DecodedImageData> {
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > (encoded.size() - position) / 4) return std::nullopt;
    for (int y = 0; y < height; ++y) {
      if (options.stop.stop_requested()) return std::nullopt;
      for (int x = 0; x < width; ++x) {
        if ((x & 4095) == 0 && options.stop.stop_requested()) return std::nullopt;
        std::array<unsigned char, 4> rgbe;
        for (auto &channel : rgbe) channel = std::to_integer<unsigned char>(encoded[position++]);
        reducer.add(x, y, hdrRgba(rgbe));
      }
    }
    return reducer.finish();
  };
  // Radiance only permits scanline RLE in this width range. Wider images use
  // flat RGBE and still stream directly into the reduced output.
  if (width < 8 || width >= 32768) return flat();
  std::vector<unsigned char> row;
  for (int y = 0; y < height; ++y) {
    if (options.stop.stop_requested() || encoded.size() - position < 4) return std::nullopt;
    const auto a = std::to_integer<unsigned>(encoded[position]);
    const auto b = std::to_integer<unsigned>(encoded[position + 1]);
    const auto high = std::to_integer<unsigned>(encoded[position + 2]);
    if (a != 2 || b != 2 || (high & 128) != 0) {
      // stb switches to a complete flat raster at the first non-RLE marker,
      // even after preceding scanlines; discard their accumulated output.
      if (y != 0) reducer = ImageRowReducer(width, height, options);
      return flat();
    }
    const auto length = (high << 8) | std::to_integer<unsigned>(encoded[position + 3]);
    position += 4;
    if (length != static_cast<unsigned>(width)) return std::nullopt;
    if (row.empty()) row.resize(static_cast<std::size_t>(width) * 4);
    for (int channel = 0; channel < 4; ++channel) {
      int x = 0;
      while (x < width) {
        if (options.stop.stop_requested() || position == encoded.size()) return std::nullopt;
        const auto code = std::to_integer<unsigned>(encoded[position++]);
        const auto count = code > 128 ? code - 128 : code;
        if (count == 0 || count > static_cast<unsigned>(width - x)) return std::nullopt;
        if (code > 128) {
          if (position == encoded.size()) return std::nullopt;
          const auto value = std::to_integer<unsigned char>(encoded[position++]);
          for (unsigned i = 0; i < count; ++i) row[static_cast<std::size_t>(x++) * 4 + channel] = value;
        } else {
          if (count > encoded.size() - position) return std::nullopt;
          for (unsigned i = 0; i < count; ++i) row[static_cast<std::size_t>(x++) * 4 + channel] =
              std::to_integer<unsigned char>(encoded[position++]);
        }
      }
    }
    for (int x = 0; x < width; ++x) {
      if ((x & 4095) == 0 && options.stop.stop_requested()) return std::nullopt;
      const auto offset = static_cast<std::size_t>(x) * 4;
      reducer.add(x, y, hdrRgba({row[offset], row[offset + 1], row[offset + 2], row[offset + 3]}));
    }
  }
  return reducer.finish();
}

} // namespace image_decode::detail
