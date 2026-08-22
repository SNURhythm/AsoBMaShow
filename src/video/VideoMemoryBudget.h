#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace video {

struct VideoDecodedMemoryLayout {
  std::size_t rgbaBytes = 0;
  std::size_t packedYuvBytes = 0;
  std::size_t alignedYuvFrameBytes = 0;
  std::size_t residentBytes = 0;
};

// Three queued frames, two recycled frames, and one conversion target can be
// resident together. The three GPU planes add one packed YUV frame, while an
// RGBA-sized allowance bounds decoder/native working storage.
inline constexpr std::size_t maximumResidentCpuYuvFrames = 6;

[[nodiscard]] inline std::optional<VideoDecodedMemoryLayout>
videoDecodedMemoryLayout(std::int64_t width, std::int64_t height) noexcept {
  constexpr std::uint64_t maximumDimension =
      std::numeric_limits<std::uint16_t>::max();
  if (width <= 0 || height <= 0 ||
      static_cast<std::uint64_t>(width) > maximumDimension ||
      static_cast<std::uint64_t>(height) > maximumDimension) {
    return std::nullopt;
  }
  const auto checkedMultiply = [](std::uint64_t left, std::uint64_t right,
                                  std::uint64_t &result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
      return false;
    }
    result = left * right;
    return true;
  };
  const auto checkedAdd = [](std::uint64_t &target, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - target) {
      return false;
    }
    target += value;
    return true;
  };
  const auto align32 = [](std::uint64_t value) {
    return (value + 31U) & ~std::uint64_t{31U};
  };

  const std::uint64_t w = static_cast<std::uint64_t>(width);
  const std::uint64_t h = static_cast<std::uint64_t>(height);
  const std::uint64_t chromaWidth = w / 2U + w % 2U;
  const std::uint64_t chromaHeight = h / 2U + h % 2U;
  std::uint64_t rgbaBytes = 0;
  std::uint64_t yBytes = 0;
  std::uint64_t chromaBytes = 0;
  std::uint64_t alignedYBytes = 0;
  std::uint64_t alignedChromaBytes = 0;
  if (!checkedMultiply(w, h, rgbaBytes) ||
      !checkedMultiply(rgbaBytes, 4U, rgbaBytes) ||
      !checkedMultiply(w, h, yBytes) ||
      !checkedMultiply(chromaWidth, chromaHeight, chromaBytes) ||
      !checkedMultiply(align32(w), h, alignedYBytes) ||
      !checkedMultiply(align32(chromaWidth), chromaHeight,
                       alignedChromaBytes)) {
    return std::nullopt;
  }
  std::uint64_t packedYuvBytes = yBytes;
  std::uint64_t alignedYuvBytes = alignedYBytes;
  if (!checkedAdd(packedYuvBytes, chromaBytes) ||
      !checkedAdd(packedYuvBytes, chromaBytes) ||
      !checkedAdd(alignedYuvBytes, alignedChromaBytes) ||
      !checkedAdd(alignedYuvBytes, alignedChromaBytes)) {
    return std::nullopt;
  }
  std::uint64_t residentCpuBytes = 0;
  if (!checkedMultiply(alignedYuvBytes, maximumResidentCpuYuvFrames,
                       residentCpuBytes)) {
    return std::nullopt;
  }
  std::uint64_t residentBytes = rgbaBytes;
  if (!checkedAdd(residentBytes, residentCpuBytes) ||
      !checkedAdd(residentBytes, packedYuvBytes) ||
      residentBytes > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return VideoDecodedMemoryLayout{
      .rgbaBytes = static_cast<std::size_t>(rgbaBytes),
      .packedYuvBytes = static_cast<std::size_t>(packedYuvBytes),
      .alignedYuvFrameBytes = static_cast<std::size_t>(alignedYuvBytes),
      .residentBytes = static_cast<std::size_t>(residentBytes)};
}

} // namespace video
