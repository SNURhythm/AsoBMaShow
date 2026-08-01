#pragma once

#include <cstdint>
#include <optional>

namespace video {

struct Yuv420FrameLayout {
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint16_t chromaWidth = 0;
  std::uint16_t chromaHeight = 0;
  std::uint16_t yPitch = 0;
  std::uint16_t uPitch = 0;
  std::uint16_t vPitch = 0;
  std::uint32_t yBytes = 0;
  std::uint32_t uBytes = 0;
  std::uint32_t vBytes = 0;
  std::uint64_t totalBytes = 0;
};

[[nodiscard]] std::optional<Yuv420FrameLayout>
makeYuv420FrameLayout(std::int64_t width, std::int64_t height,
                      std::int64_t yPitch = 0, std::int64_t uPitch = 0,
                      std::int64_t vPitch = 0) noexcept;

} // namespace video
