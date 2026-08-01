#include "VideoFrameLayout.h"

#include <limits>

namespace video {
namespace {

bool resolvePitch(std::int64_t requested, std::uint16_t minimum,
                  std::uint16_t &resolved) noexcept {
  if (requested == 0) {
    resolved = minimum;
    return true;
  }
  if (requested < minimum ||
      requested > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  resolved = static_cast<std::uint16_t>(requested);
  return true;
}

bool planeBytes(std::uint16_t pitch, std::uint16_t rows,
                std::uint32_t &bytes) noexcept {
  if (pitch > std::numeric_limits<std::uint32_t>::max() / rows) {
    return false;
  }
  bytes = static_cast<std::uint32_t>(pitch) * rows;
  return true;
}

} // namespace

std::optional<Yuv420FrameLayout>
makeYuv420FrameLayout(std::int64_t width, std::int64_t height,
                      std::int64_t yPitch, std::int64_t uPitch,
                      std::int64_t vPitch) noexcept {
  constexpr auto kMaximumDimension =
      static_cast<std::int64_t>(std::numeric_limits<std::uint16_t>::max());
  if (width <= 0 || height <= 0 || width > kMaximumDimension ||
      height > kMaximumDimension) {
    return std::nullopt;
  }

  Yuv420FrameLayout layout{
      .width = static_cast<std::uint16_t>(width),
      .height = static_cast<std::uint16_t>(height),
      .chromaWidth = static_cast<std::uint16_t>(width / 2 + width % 2),
      .chromaHeight = static_cast<std::uint16_t>(height / 2 + height % 2),
  };
  if (!resolvePitch(yPitch, layout.width, layout.yPitch) ||
      !resolvePitch(uPitch, layout.chromaWidth, layout.uPitch) ||
      !resolvePitch(vPitch, layout.chromaWidth, layout.vPitch) ||
      !planeBytes(layout.yPitch, layout.height, layout.yBytes) ||
      !planeBytes(layout.uPitch, layout.chromaHeight, layout.uBytes) ||
      !planeBytes(layout.vPitch, layout.chromaHeight, layout.vBytes)) {
    return std::nullopt;
  }

  layout.totalBytes = static_cast<std::uint64_t>(layout.yBytes) +
                      layout.uBytes + layout.vBytes;
  return layout;
}

} // namespace video
