#pragma once

#include "DecodedImageCache.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>

namespace image_decode {

struct ImageDecodeOptions {
  int maximumDimension = 65535;
  std::size_t maximumEncodedBytes = static_cast<std::size_t>(UINT32_MAX);
  std::size_t maximumDecodedBytes = static_cast<std::size_t>(UINT32_MAX);
  int targetWidth = 0;
  int targetHeight = 0;
  std::stop_token stop{};
};

// This decoder has no cache, thumbnail, archive, or UI side effects.  Package
// callers supply bytes obtained through their own containment boundary.
[[nodiscard]] std::optional<DecodedImageData>
decodeImageMemory(std::span<const std::byte> encoded,
                  const ImageDecodeOptions &options);

[[nodiscard]] std::optional<DecodedImageData>
decodeImageMemory(std::span<const std::byte> encoded,
                  int maximumDimension = 65535,
                  std::size_t maximumDecodedBytes =
                      static_cast<std::size_t>(UINT32_MAX));

[[nodiscard]] std::optional<DecodedImageData>
decodeImageFile(const std::filesystem::path &path,
                const ImageDecodeOptions &options);

[[nodiscard]] std::optional<DecodedImageData>
decodeImageFile(const std::filesystem::path &path,
                int maximumDimension = 65535,
                std::size_t maximumDecodedBytes =
                    static_cast<std::size_t>(UINT32_MAX),
                std::size_t maximumEncodedBytes =
                    static_cast<std::size_t>(UINT32_MAX));

// Resizing is intentionally part of the decoder core so package adapters do
// not grow independent stbir paths.  Invalid requests fail rather than
// returning a partial image; a target that does not downsample returns source.
[[nodiscard]] std::optional<DecodedImageData>
resizeDecodedImage(const DecodedImageData &decoded,
                   const ImageDecodeOptions &options);

} // namespace image_decode
