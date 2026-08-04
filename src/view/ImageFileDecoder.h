#pragma once

#include "DecodedImageCache.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>

namespace image_decode {

// This decoder has no cache, thumbnail, archive, or UI side effects.  Package
// callers supply bytes obtained through their own containment boundary.
[[nodiscard]] std::optional<DecodedImageData>
decodeImageMemory(std::span<const std::byte> encoded,
                  int maximumDimension = 65535,
                  std::size_t maximumDecodedBytes =
                      static_cast<std::size_t>(UINT32_MAX));

[[nodiscard]] std::optional<DecodedImageData>
decodeImageFile(const std::filesystem::path &path,
                int maximumDimension = 65535,
                std::size_t maximumDecodedBytes =
                    static_cast<std::size_t>(UINT32_MAX));

} // namespace image_decode
