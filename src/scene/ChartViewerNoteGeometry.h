#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace chart_viewer_note_geometry {

inline constexpr float kInvisibleNoteBorderHeightRatio = 0.15F;

struct Rectangle {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

struct Rectangles {
  std::array<Rectangle, 4> rectangles{};
  std::size_t count = 0;
};

inline Rectangles invisibleNoteRectangles(
    float x, float y, float width, float height, float borderThickness,
    bool isLongNote) {
  Rectangles result;
  if (width <= 0.0F || height <= 0.0F) {
    return result;
  }
  if (isLongNote) {
    result.rectangles[0] = {x, y, width, height};
    result.count = 1;
    return result;
  }

  const float border =
      std::min({borderThickness, width * 0.5F, height * 0.5F});
  if (border <= 0.0F) {
    return result;
  }
  result.rectangles = {
      Rectangle{x, y, width, border},
      Rectangle{x, y + height - border, width, border},
      Rectangle{x, y, border, height},
      Rectangle{x + width - border, y, border, height}};
  result.count = result.rectangles.size();
  return result;
}

} // namespace chart_viewer_note_geometry
