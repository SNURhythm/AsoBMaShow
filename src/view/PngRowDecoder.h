#pragma once

#include "ImageRowReducer.h"

#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "../../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

#include <climits>
#include <cstdlib>

// Internal row decoder shared by file and archive-backed image reads.
namespace image_decode::detail {

inline std::uint32_t pngU32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<unsigned char>(bytes[offset + i]);
  return value;
}

inline bool isPng(std::span<const std::byte> encoded) {
  constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  return encoded.size() >= signature.size() &&
         std::equal(signature.begin(), signature.end(), encoded.begin(),
                     [](auto a, auto b) { return a == std::to_integer<unsigned char>(b); });
}

struct PngRows {
  int width = 0, height = 0, depth = 0, color = 0, channels = 0;
  bool interlaced = false, iphone = false;
  std::span<const std::byte> palette, transparency;
  std::size_t firstData = 0, dataBytes = 0;
};

inline std::optional<PngRows> pngRowsHeader(std::span<const std::byte> encoded,
                                           const ImageDecodeOptions &options) {
  PngRows image;
  bool header = false, data = false, dataEnded = false;
  for (std::size_t offset = 8; offset <= encoded.size() && encoded.size() - offset >= 12;) {
    if (options.stop.stop_requested()) return std::nullopt;
    const auto length = pngU32(encoded, offset);
    if (length > encoded.size() - offset - 12) return std::nullopt;
    const auto type = pngU32(encoded, offset + 4);
    const auto payload = encoded.subspan(offset + 8, length);
    mz_ulong crc = 0;
    const auto checked = encoded.subspan(offset + 4, static_cast<std::size_t>(length) + 4);
    for (std::size_t position = 0; position < checked.size();) {
      if (options.stop.stop_requested()) return std::nullopt;
      const auto count = std::min<std::size_t>(64 * 1024, checked.size() - position);
      crc = mz_crc32(crc, reinterpret_cast<const unsigned char *>(checked.data() + position), count);
      position += count;
    }
    if (crc != pngU32(encoded, offset + 8 + length)) return std::nullopt;
    if (data && type != 0x49444154) dataEnded = true; // IDAT must be consecutive.
    if (type == 0x43674249 && !header && !image.iphone) { // Apple's raw-deflate CgBI.
      image.iphone = true;
    } else if (type == 0x49484452) { // IHDR
      if (header || length != 13) return std::nullopt;
      const auto width = pngU32(payload, 0), height = pngU32(payload, 4);
      if (width == 0 || height == 0 || options.maximumDimension <= 0 ||
          width > static_cast<unsigned>(options.maximumDimension) ||
          height > static_cast<unsigned>(options.maximumDimension) ||
          static_cast<std::uint64_t>(width) * height > options.maximumDecodedBytes / 4 ||
          static_cast<std::uint64_t>(width) * height > SIZE_MAX / 4) return std::nullopt;
      image.width = static_cast<int>(width);
      image.height = static_cast<int>(height);
      image.depth = std::to_integer<int>(payload[8]);
      image.color = std::to_integer<int>(payload[9]);
      const bool packed = image.depth == 1 || image.depth == 2 || image.depth == 4;
      if ((image.color != 0 && image.color != 2 && image.color != 3 &&
           image.color != 4 && image.color != 6) ||
          (!packed && image.depth != 8 && image.depth != 16) ||
          (packed && image.color != 0 && image.color != 3) ||
          (image.color == 3 && image.depth == 16) ||
          payload[10] != std::byte{0} || payload[11] != std::byte{0} ||
          std::to_integer<int>(payload[12]) > 1) return std::nullopt;
      image.channels = image.color == 3 ? 1 : (image.color & 2 ? 3 : 1) + (image.color & 4 ? 1 : 0);
      image.interlaced = payload[12] == std::byte{1};
      header = true;
    } else if (!header) {
      return std::nullopt;
    } else if (type == 0x504c5445) { // PLTE
      if (data || !image.palette.empty() || length == 0 || length > 768 || length % 3 != 0) return std::nullopt;
      image.palette = payload;
    } else if (type == 0x74524e53) { // tRNS
      if (data || !image.transparency.empty() ||
          (image.color == 0 ? length != 2 : image.color == 2 ? length != 6 :
           image.color == 3 ? length == 0 || length > image.palette.size() / 3 : true)) return std::nullopt;
      image.transparency = payload;
    } else if (type == 0x49444154) { // IDAT
      if (dataEnded || (image.color == 3 && image.palette.empty())) return std::nullopt;
      if (!data) image.firstData = offset;
      image.dataBytes += length;
      data = true;
    } else if (type == 0x49454e44) { // IEND
      return data && length == 0 && image.dataBytes != 0 ? std::optional(image) : std::nullopt;
    } else if ((type & 0x20000000U) == 0) {
      return std::nullopt; // Unknown critical chunk.
    }
    offset += static_cast<std::size_t>(length) + 12;
  }
  return std::nullopt;
}

class PngRowInflater {
public:
  PngRowInflater(std::span<const std::byte> encoded, const PngRows &image,
                 const ImageDecodeOptions &options)
      : encoded_(encoded), offset_(image.firstData), expectedBytes_(image.dataBytes),
        options_(options) {
    initialized_ = mz_inflateInit2(&stream_, image.iphone ? -MZ_DEFAULT_WINDOW_BITS :
                                                          MZ_DEFAULT_WINDOW_BITS) == MZ_OK;
  }
  ~PngRowInflater() { if (initialized_) mz_inflateEnd(&stream_); }

  bool read(std::span<unsigned char> output) {
    if (!initialized_ || finished_ || output.size() > UINT_MAX) return false;
    stream_.next_out = output.data();
    stream_.avail_out = static_cast<mz_uint>(output.size());
    while (stream_.avail_out != 0) {
      if (options_.stop.stop_requested() || !step()) return false;
      if (finished_) return stream_.avail_out == 0;
    }
    return true;
  }

  bool finish() {
    if (!initialized_) return false;
    // stb accepts extra inflated padding found in real PNGs (stb issue 276).
    // Bound only this non-pixel padding, not the declared image rows. A tiny
    // image must not make us inflate arbitrarily large trailing output.
    std::size_t remainingPadding = 64 * 1024;
    std::array<unsigned char, 64 * 1024> discard{};
    while (!finished_) {
      if (options_.stop.stop_requested()) return false;
      stream_.next_out = discard.data();
      // One extra byte distinguishes completion at the allowance from excess
      // output, including when the zlib trailer lives in the next IDAT chunk.
      const auto capacity = std::min(discard.size(), remainingPadding + 1);
      stream_.avail_out = static_cast<mz_uint>(capacity);
      if (!step()) return false;
      const auto produced = capacity - stream_.avail_out;
      if (produced > remainingPadding) return false;
      remainingPadding -= produced;
    }
    return stream_.total_in == expectedBytes_;
  }

private:
  bool step() {
    while (stream_.avail_in == 0 && offset_ + 12 <= encoded_.size() &&
           pngU32(encoded_, offset_ + 4) == 0x49444154) {
      if (options_.stop.stop_requested()) return false;
      const auto bytes = pngU32(encoded_, offset_);
      stream_.next_in = reinterpret_cast<const unsigned char *>(encoded_.data() + offset_ + 8);
      stream_.avail_in = bytes;
      offset_ += static_cast<std::size_t>(bytes) + 12;
    }
    const auto beforeIn = stream_.avail_in, beforeOut = stream_.avail_out;
    const int status = mz_inflate(&stream_, MZ_NO_FLUSH);
    finished_ = status == MZ_STREAM_END;
    return finished_ || ((status == MZ_OK || status == MZ_BUF_ERROR) &&
                         (stream_.avail_in != beforeIn || stream_.avail_out != beforeOut));
  }
  std::span<const std::byte> encoded_;
  std::size_t offset_, expectedBytes_;
  const ImageDecodeOptions &options_;
  mz_stream stream_{};
  bool initialized_ = false, finished_ = false;
};

inline int pngPaeth(int a, int b, int c) {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

inline std::optional<DecodedImageData> decodePngRows(std::span<const std::byte> encoded,
                                                    const ImageDecodeOptions &options) {
  const auto header = pngRowsHeader(encoded, options);
  if (!header) return std::nullopt;
  const auto &image = *header;
  ImageRowReducer reducer(image.width, image.height, options, image.interlaced);
  PngRowInflater inflater(encoded, image, options);
  constexpr int xStarts[]{0, 4, 0, 2, 0, 1, 0}, yStarts[]{0, 0, 4, 0, 2, 0, 1};
  constexpr int xSteps[]{8, 8, 4, 4, 2, 2, 1}, ySteps[]{8, 8, 8, 4, 4, 2, 2};
  for (int pass = 0; pass < (image.interlaced ? 7 : 1); ++pass) {
    const int xStart = image.interlaced ? xStarts[pass] : 0;
    const int yStart = image.interlaced ? yStarts[pass] : 0;
    const int xStep = image.interlaced ? xSteps[pass] : 1;
    const int yStep = image.interlaced ? ySteps[pass] : 1;
    const int width = static_cast<int>((static_cast<std::int64_t>(image.width) - xStart + xStep - 1) / xStep);
    const int height = static_cast<int>((static_cast<std::int64_t>(image.height) - yStart + yStep - 1) / yStep);
    if (width <= 0 || height <= 0) continue;
    const auto rowBytes = (static_cast<std::size_t>(width) * image.channels * image.depth + 7) / 8;
    const int stride = std::max(1, (image.channels * image.depth + 7) / 8);
    std::vector<unsigned char> row(rowBytes), prior(rowBytes);
    for (int y = 0; y < height; ++y) {
      unsigned char filter = 0;
      if (options.stop.stop_requested() || !inflater.read({&filter, 1}) ||
          filter > 4 || !inflater.read(row)) return std::nullopt;
      for (std::size_t i = 0; i < rowBytes; ++i) {
        const int a = i >= static_cast<std::size_t>(stride) ? row[i - stride] : 0;
        const int b = prior[i], c = i >= static_cast<std::size_t>(stride) ? prior[i - stride] : 0;
        const int prediction = filter == 1 ? a : filter == 2 ? b :
                               filter == 3 ? (a + b) / 2 : filter == 4 ? pngPaeth(a, b, c) : 0;
        row[i] = static_cast<unsigned char>(row[i] + prediction);
      }
      for (int x = 0; x < width; ++x) {
        std::array<unsigned, 4> sample{};
        for (int channel = 0; channel < image.channels; ++channel) {
          const auto bit = (static_cast<std::size_t>(x) * image.channels + channel) * image.depth;
          sample[channel] = image.depth == 16 ? (row[bit / 8] << 8) | row[bit / 8 + 1] :
                            (row[bit / 8] >> (8 - image.depth - bit % 8)) & ((1U << image.depth) - 1);
        }
        const auto byte = [&](unsigned value) -> unsigned char {
          return image.depth == 16 ? value >> 8 : value * 255 / ((1U << image.depth) - 1);
        };
        std::array<unsigned char, 4> rgba{0, 0, 0, 255};
        if (image.color == 3) {
          if (sample[0] >= image.palette.size() / 3) return std::nullopt;
          for (int channel = 0; channel < 3; ++channel) rgba[channel] =
              std::to_integer<unsigned char>(image.palette[sample[0] * 3 + channel]);
          if (sample[0] < image.transparency.size()) rgba[3] =
              std::to_integer<unsigned char>(image.transparency[sample[0]]);
        } else {
          rgba[0] = byte(sample[0]);
          rgba[1] = byte(sample[image.color & 2 ? 1 : 0]);
          rgba[2] = byte(sample[image.color & 2 ? 2 : 0]);
          if (image.color & 4) rgba[3] = byte(sample[image.channels - 1]);
          if (!image.transparency.empty()) {
            bool transparent = true;
            for (int channel = 0; channel < image.channels; ++channel) {
              const unsigned key = (std::to_integer<unsigned>(image.transparency[channel * 2]) << 8) |
                                    std::to_integer<unsigned>(image.transparency[channel * 2 + 1]);
              transparent = transparent && sample[channel] == key;
            }
            if (transparent) rgba[3] = 0;
          }
        }
        reducer.add(xStart + x * xStep, yStart + y * yStep, rgba);
      }
      row.swap(prior);
    }
  }
  if (!inflater.finish() || options.stop.stop_requested()) return std::nullopt;
  return reducer.finish();
}

} // namespace image_decode::detail
