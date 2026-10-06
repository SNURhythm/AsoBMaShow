#pragma once

#include <string>
#include <cmath>

namespace lane_cover_number {

inline std::string whiteNumberLabel(float noteStartPositionPercent) {
  return std::to_string(std::lround(noteStartPositionPercent * 10));
}

struct PairLayout {
  int whiteX = 0;
  int greenX = 0;
  int whiteWidth = 0;
  int greenWidth = 0;
  int gap = 0;

  [[nodiscard]] int right() const { return greenX + greenWidth; }
  [[nodiscard]] int center() const { return (whiteX + right()) / 2; }
};

inline PairLayout centerPair(int centerX, int whiteWidth, int greenWidth,
                             int gap) {
  const int totalWidth = whiteWidth + gap + greenWidth;
  const int whiteX = centerX - totalWidth / 2;
  return {.whiteX = whiteX,
          .greenX = whiteX + whiteWidth + gap,
          .whiteWidth = whiteWidth,
          .greenWidth = greenWidth,
          .gap = gap};
}

} // namespace lane_cover_number
