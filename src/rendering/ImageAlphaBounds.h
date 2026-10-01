#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

namespace image_alpha {

// Normalized, exclusive edges in top-down image coordinates. Unknown bounds
// default to the full sprite; an entirely transparent sprite has zero area.
struct Bounds {
  double left = 0.0;
  double top = 0.0;
  double right = 1.0;
  double bottom = 1.0;
};

struct Rect {
  double x;
  double y;
  double width;
  double height;
};

inline Bounds visibleBounds(std::span<const unsigned char> rgba, int imageWidth,
                            int imageHeight, int x, int y, int width,
                            int height) noexcept {
  if (imageWidth <= 0 || imageHeight <= 0 || x < 0 || y < 0 ||
      width <= 0 || height <= 0 || width > imageWidth || height > imageHeight ||
      x > imageWidth - width || y > imageHeight - height ||
      static_cast<std::size_t>(imageWidth) >
          std::numeric_limits<std::size_t>::max() / 4 /
              static_cast<std::size_t>(imageHeight) ||
      rgba.size() < static_cast<std::size_t>(imageWidth) * imageHeight * 4) {
    return {};
  }
  int left = width;
  int top = height;
  int right = 0;
  int bottom = 0;
  for (int row = 0; row < height; ++row) {
    const auto offset =
        (static_cast<std::size_t>(y + row) * imageWidth + x) * 4;
    for (int column = 0; column < width; ++column) {
      if (rgba[offset + static_cast<std::size_t>(column) * 4 + 3] != 0) {
        left = std::min(left, column);
        top = std::min(top, row);
        right = std::max(right, column + 1);
        bottom = std::max(bottom, row + 1);
      }
    }
  }
  if (right == 0 || bottom == 0) {
    return {0.0, 0.0, 0.0, 0.0};
  }
  return {static_cast<double>(left) / width,
          static_cast<double>(top) / height,
          static_cast<double>(right) / width,
          static_cast<double>(bottom) / height};
}

inline Rect trimBottomUp(Rect rect, Bounds bounds) noexcept {
  return {.x = rect.x + rect.width * bounds.left,
          .y = rect.y + rect.height * (1.0 - bounds.bottom),
          .width = rect.width * (bounds.right - bounds.left),
          .height = rect.height * (bounds.bottom - bounds.top)};
}

} // namespace image_alpha
