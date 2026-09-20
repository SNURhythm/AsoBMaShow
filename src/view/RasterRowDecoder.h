#pragma once

#include "ImageRowReducer.h"

#include <bit>
#include <limits>

namespace image_decode::detail {

inline unsigned rasterByte(std::span<const std::byte> bytes, std::size_t offset) {
  return std::to_integer<unsigned char>(bytes[offset]);
}

inline std::uint32_t rasterU16(std::span<const std::byte> bytes, std::size_t offset) {
  return rasterByte(bytes, offset) | (rasterByte(bytes, offset + 1) << 8);
}

inline std::uint32_t rasterU32(std::span<const std::byte> bytes, std::size_t offset) {
  return rasterU16(bytes, offset) | (rasterU16(bytes, offset + 2) << 16);
}

inline bool rasterDimensions(std::uint32_t width, std::uint32_t height,
                             const ImageDecodeOptions &options) {
  return width > 0 && height > 0 && options.maximumDimension > 0 &&
         width <= static_cast<unsigned>(options.maximumDimension) &&
         height <= static_cast<unsigned>(options.maximumDimension) &&
         static_cast<std::uint64_t>(width) * height <= options.maximumDecodedBytes / 4 &&
         static_cast<std::uint64_t>(width) * height <= SIZE_MAX / 4;
}

// Reduction commutes with reflection. Feed rows in file order so even bottom-up
// RLE images use only the sequential reducer's two rows, then reflect the small
// output in place instead of retaining source pixels or image-sized sums.
inline std::optional<DecodedImageData> finishRasterRows(
    ImageRowReducer &reducer, bool flipX, bool flipY,
    const ImageDecodeOptions &options, bool forceOpaque = false) {
  auto image = reducer.finish();
  if (!image) return std::nullopt;
  auto &pixels = *image->rgba;
  if (flipY) {
    const auto stride = static_cast<std::size_t>(image->width) * 4;
    for (int y = 0; y < image->height / 2; ++y) {
      if (options.stop.stop_requested()) return std::nullopt;
      const auto top = static_cast<std::size_t>(y) * stride;
      const auto bottom = static_cast<std::size_t>(image->height - 1 - y) * stride;
      for (std::size_t x = 0; x < stride; ++x) std::swap(pixels[top + x], pixels[bottom + x]);
    }
  }
  for (int y = 0; y < image->height && (flipX || forceOpaque); ++y) {
    if (options.stop.stop_requested()) return std::nullopt;
    const auto row = static_cast<std::size_t>(y) * image->width * 4;
    if (flipX) {
      for (int x = 0; x < image->width / 2; ++x) {
        const auto left = row + static_cast<std::size_t>(x) * 4;
        const auto right = row + static_cast<std::size_t>(image->width - 1 - x) * 4;
        for (int c = 0; c < 4; ++c) std::swap(pixels[left + c], pixels[right + c]);
      }
    }
    if (forceOpaque) {
      for (int x = 0; x < image->width; ++x) pixels[row + static_cast<std::size_t>(x) * 4 + 3] = 255;
    }
  }
  return options.stop.stop_requested() ? std::nullopt : std::move(image);
}

inline bool isBmp(std::span<const std::byte> encoded) {
  return encoded.size() >= 2 && encoded[0] == std::byte{'B'} && encoded[1] == std::byte{'M'};
}

inline bool rasterValidMask(std::uint32_t mask, unsigned depth) {
  if (mask == 0) return true;
  if (depth < 32 && (mask >> depth) != 0) return false;
  const auto normalized = mask >> std::countr_zero(mask);
  return (normalized & (normalized + 1U)) == 0;
}

inline unsigned char rasterMaskedChannel(std::uint32_t pixel, std::uint32_t mask) {
  if (mask == 0) return 255;
  const unsigned shift = std::countr_zero(mask);
  const auto maximum = mask >> shift;
  const auto value = (pixel & mask) >> shift;
  const unsigned bits = std::popcount(mask);
  // Match stb's fractional bit extension for 1-8 bit BMP channels.
  constexpr unsigned multiplier[]{0, 255, 85, 73, 17, 33, 65, 129, 1};
  constexpr unsigned divideShift[]{0, 0, 0, 1, 0, 2, 4, 6, 0};
  return static_cast<unsigned char>(bits <= 8
      ? (value * multiplier[bits]) >> divideShift[bits]
      : static_cast<std::uint64_t>(value) * 255 / maximum);
}

inline std::optional<DecodedImageData> decodeBmpRows(
    std::span<const std::byte> encoded, const ImageDecodeOptions &options) {
  if (!isBmp(encoded) || encoded.size() < 26 || options.stop.stop_requested() ||
      encoded.size() > options.maximumEncodedBytes) return std::nullopt;
  const std::size_t pixelOffset = rasterU32(encoded, 10);
  const auto headerSize = rasterU32(encoded, 14);
  if (headerSize != 12 && headerSize != 40 && headerSize != 52 &&
      headerSize != 56 && headerSize != 108 && headerSize != 124) return std::nullopt;
  if (headerSize > encoded.size() - 14 || pixelOffset > encoded.size() ||
      pixelOffset < 14 + headerSize) return std::nullopt;
  const bool core = headerSize == 12;
  const auto width = core ? rasterU16(encoded, 18) : rasterU32(encoded, 18);
  const auto rawHeight = core ? rasterU16(encoded, 20) : rasterU32(encoded, 22);
  const bool topDown = !core && (rawHeight & 0x80000000U) != 0;
  const auto height = topDown ? 0U - rawHeight : rawHeight;
  const unsigned depth = rasterU16(encoded, core ? 24 : 28);
  const unsigned compression = core ? 0 : rasterU32(encoded, 30);
  if (!rasterDimensions(width, height, options) ||
      rasterU16(encoded, core ? 22 : 26) != 1 ||
      (depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32) ||
      (compression != 0 && compression != 3 && compression != 6) ||
      (compression != 0 && depth != 16 && depth != 32)) return std::nullopt;

  std::size_t metadataEnd = 14 + headerSize;
  std::array<std::uint32_t, 4> masks{};
  bool defaultAlpha = false;
  if (depth == 16 || depth == 32) {
    if (compression == 0) {
      masks = depth == 16 ? std::array<std::uint32_t, 4>{0x7c00, 0x3e0, 0x1f, 0}
                          : std::array<std::uint32_t, 4>{0xff0000, 0xff00, 0xff, 0xff000000};
      defaultAlpha = depth == 32;
    } else {
      const std::size_t maskOffset = headerSize == 40 ? metadataEnd : 54;
      const unsigned maskCount = compression == 6 || headerSize >= 56 ? 4 : 3;
      if (maskOffset > pixelOffset || maskCount * 4 > pixelOffset - maskOffset ||
          (headerSize != 40 && maskCount * 4 > metadataEnd - maskOffset)) return std::nullopt;
      for (unsigned channel = 0; channel < maskCount; ++channel)
        masks[channel] = rasterU32(encoded, maskOffset + channel * 4);
      if (headerSize == 40) metadataEnd += maskCount * 4;
    }
    if (!masks[0] || !masks[1] || !masks[2] || (compression == 6 && !masks[3])) return std::nullopt;
    std::uint32_t used = 0;
    for (auto mask : masks) {
      if (!rasterValidMask(mask, depth) || (mask & used) != 0) return std::nullopt;
      used |= mask;
    }
  }

  std::size_t paletteCount = 0;
  const std::size_t paletteStride = core ? 3 : 4;
  if (depth < 16) {
    const auto declared = core ? 0 : rasterU32(encoded, 46);
    const auto maximum = std::size_t{1} << depth;
    paletteCount = declared ? declared : std::min(maximum, (pixelOffset - metadataEnd) / paletteStride);
    if (paletteCount == 0 || paletteCount > maximum ||
        paletteCount > (pixelOffset - metadataEnd) / paletteStride) return std::nullopt;
  }
  const auto stride64 = ((static_cast<std::uint64_t>(width) * depth + 31) / 32) * 4;
  if (stride64 > SIZE_MAX || height > (encoded.size() - pixelOffset) / stride64) return std::nullopt;
  const auto stride = static_cast<std::size_t>(stride64);
  ImageRowReducer reducer(static_cast<int>(width), static_cast<int>(height), options);
  bool anyAlpha = false;
  for (unsigned y = 0; y < height; ++y) {
    if (options.stop.stop_requested()) return std::nullopt;
    const auto row = pixelOffset + static_cast<std::size_t>(y) * stride;
    for (unsigned x = 0; x < width; ++x) {
      if ((x & 4095U) == 0 && options.stop.stop_requested()) return std::nullopt;
      std::array<unsigned char, 4> rgba{0, 0, 0, 255};
      if (depth < 16) {
        const auto bit = static_cast<std::size_t>(x) * depth;
        const unsigned index = (rasterByte(encoded, row + bit / 8) >> (8 - depth - bit % 8)) & ((1U << depth) - 1);
        if (index >= paletteCount) return std::nullopt;
        const auto entry = metadataEnd + static_cast<std::size_t>(index) * paletteStride;
        for (int c = 0; c < 3; ++c) rgba[c] = rasterByte(encoded, entry + 2 - c);
      } else if (depth == 24) {
        const auto pixel = row + static_cast<std::size_t>(x) * 3;
        for (int c = 0; c < 3; ++c) rgba[c] = rasterByte(encoded, pixel + 2 - c);
      } else {
        const auto pixel = row + static_cast<std::size_t>(x) * (depth / 8);
        const auto packed = depth == 16 ? rasterU16(encoded, pixel) : rasterU32(encoded, pixel);
        for (int c = 0; c < 4; ++c) rgba[c] = rasterMaskedChannel(packed, masks[c]);
        anyAlpha = anyAlpha || rgba[3] != 0;
      }
      reducer.add(static_cast<int>(x), static_cast<int>(y), rgba);
    }
  }
  return finishRasterRows(reducer, false, !topDown, options, defaultAlpha && !anyAlpha);
}

// TGA has no signature; use only after signature-based codecs. This mirrors
// stb's header probe. Unsupported interleaving is rejected by the decoder.
inline bool isTga(std::span<const std::byte> encoded) {
  if (encoded.size() < 18) return false;
  const auto indexed = rasterByte(encoded, 1), type = rasterByte(encoded, 2);
  const auto depth = rasterByte(encoded, 16);
  const auto colorDepth = indexed ? rasterByte(encoded, 7) : depth;
  if (indexed > 1 || rasterU16(encoded, 12) == 0 || rasterU16(encoded, 14) == 0) return false;
  if (indexed ? (type != 1 && type != 9) || (depth != 8 && depth != 16)
              : (type != 2 && type != 3 && type != 10 && type != 11)) return false;
  return colorDepth == 8 || colorDepth == 15 || colorDepth == 16 ||
         colorDepth == 24 || colorDepth == 32;
}

inline std::array<unsigned char, 4> rasterTgaColor(
    std::span<const std::byte> bytes, std::size_t offset, unsigned depth, bool gray) {
  std::array<unsigned char, 4> color{0, 0, 0, 255};
  if (depth == 8 || (gray && depth == 16)) {
    color[0] = color[1] = color[2] = rasterByte(bytes, offset);
    if (depth == 16) color[3] = rasterByte(bytes, offset + 1);
  } else if (depth == 15 || depth == 16) {
    const auto packed = rasterU16(bytes, offset);
    // As in stb, 15/16-bit true-color TGA treats the high bit as padding.
    color[0] = ((packed >> 10) & 31) * 255 / 31;
    color[1] = ((packed >> 5) & 31) * 255 / 31;
    color[2] = (packed & 31) * 255 / 31;
  } else {
    for (int c = 0; c < 3; ++c) color[c] = rasterByte(bytes, offset + 2 - c);
    if (depth == 32) color[3] = rasterByte(bytes, offset + 3);
  }
  return color;
}

inline std::optional<DecodedImageData> decodeTgaRows(
    std::span<const std::byte> encoded, const ImageDecodeOptions &options) {
  if (!isTga(encoded) || options.stop.stop_requested() ||
      encoded.size() > options.maximumEncodedBytes) return std::nullopt;
  const unsigned width = rasterU16(encoded, 12), height = rasterU16(encoded, 14);
  if (!rasterDimensions(width, height, options)) return std::nullopt;
  const bool indexed = rasterByte(encoded, 1) == 1;
  const unsigned type = rasterByte(encoded, 2), depth = rasterByte(encoded, 16);
  const bool rle = type >= 8, gray = type == 3 || type == 11;
  const unsigned descriptor = rasterByte(encoded, 17);
  if ((descriptor & 0xc0) != 0) return std::nullopt;
  const unsigned firstColor = rasterU16(encoded, 3), colorCount = rasterU16(encoded, 5);
  const unsigned paletteDepth = rasterByte(encoded, 7);
  const std::size_t paletteStride = (paletteDepth + 7) / 8;
  const std::size_t sampleBytes = (depth + 7) / 8;
  std::size_t offset = 18 + rasterByte(encoded, 0);
  if (offset > encoded.size()) return std::nullopt;
  const std::size_t paletteOffset = offset;
  if (indexed) {
    if (colorCount == 0 || firstColor + colorCount > 65536 ||
        colorCount > (encoded.size() - offset) / paletteStride) return std::nullopt;
    offset += static_cast<std::size_t>(colorCount) * paletteStride;
  }
  const auto totalPixels = static_cast<std::uint64_t>(width) * height;
  if (!rle && totalPixels > (encoded.size() - offset) / sampleBytes) return std::nullopt;
  ImageRowReducer reducer(static_cast<int>(width), static_cast<int>(height), options);
  std::array<unsigned char, 4> color{};
  unsigned remaining = 0;
  bool repeated = false;
  for (std::uint64_t pixel = 0; pixel < totalPixels; ++pixel) {
    if ((pixel & 4095U) == 0 && options.stop.stop_requested()) return std::nullopt;
    bool readPixel = !rle;
    if (rle) {
      if (remaining == 0) {
        if (offset == encoded.size()) return std::nullopt;
        const unsigned packet = rasterByte(encoded, offset++);
        remaining = (packet & 127) + 1;
        repeated = (packet & 128) != 0;
        if (remaining > totalPixels - pixel) return std::nullopt;
        readPixel = true;
      } else {
        readPixel = !repeated;
      }
      --remaining;
    }
    if (readPixel) {
      if (sampleBytes > encoded.size() - offset) return std::nullopt;
      if (indexed) {
        const unsigned index = depth == 8 ? rasterByte(encoded, offset) : rasterU16(encoded, offset);
        if (index < firstColor || index - firstColor >= colorCount) return std::nullopt;
        color = rasterTgaColor(encoded, paletteOffset + (index - firstColor) * paletteStride,
                                paletteDepth, false);
      } else {
        color = rasterTgaColor(encoded, offset, depth, gray);
      }
      offset += sampleBytes;
    }
    reducer.add(static_cast<int>(pixel % width), static_cast<int>(pixel / width), color);
  }
  return finishRasterRows(reducer, (descriptor & 0x10) != 0, (descriptor & 0x20) == 0, options);
}

} // namespace image_decode::detail
