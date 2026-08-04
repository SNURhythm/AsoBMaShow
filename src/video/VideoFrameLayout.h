#pragma once

#include <array>
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

struct VideoQuadPoint {
  float x = 0.0F;
  float y = 0.0F;
};

struct VideoQuadTint {
  float r = 1.0F;
  float g = 1.0F;
  float b = 1.0F;
  float a = 1.0F;
};

struct EmbeddedYuvQuadVertex {
  float x = 0.0F;
  float y = 0.0F;
  float u = 0.0F;
  float v = 0.0F;
  float r = 1.0F;
  float g = 1.0F;
  float b = 1.0F;
  float a = 1.0F;
};

struct EmbeddedYuvQuadLayout {
  std::array<EmbeddedYuvQuadVertex, 4> vertices{};
  std::array<std::uint16_t, 6> indices{};
};

// Destination and UV points use the stable TL, TR, BL, BR order.
[[nodiscard]] std::optional<EmbeddedYuvQuadLayout>
makeEmbeddedYuvQuadLayout(const std::array<VideoQuadPoint, 4> &destinations,
                          const std::array<VideoQuadPoint, 4> &uvs,
                          VideoQuadTint tint) noexcept;

} // namespace video
