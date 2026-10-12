#pragma once

#include <algorithm>
#include <cmath>

namespace rendering {
// Built-in menus adapt to available density-independent space on every platform.
// Gameplay and skin scenes retain their original design coordinates.
inline bool responsiveMenuSceneActive = false;
inline bool compactMenuCanvasActive = false;

inline bool usesCompactMenuCanvas(int pixelWidth, int pixelHeight, float scale, bool menu) {
  return menu && scale > 0 && std::min(pixelWidth, pixelHeight) / scale < 600.0f;
}
inline constexpr int compactTouchTarget = 66; // 44 points at 1.5 UI units / point.

inline int responsiveMenuCanvasWidth(int drawableWidth, int drawableHeight,
                                     int logicalWidth, bool phoneMenu) {
  if (phoneMenu && logicalWidth > 0)
    return std::max(1, static_cast<int>(std::lround(logicalWidth * 1.5)));
  return drawableWidth < drawableHeight ? 1080 : 1920;
}
} // namespace rendering
