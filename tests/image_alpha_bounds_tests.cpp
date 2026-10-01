#include "rendering/ImageAlphaBounds.h"

#include <cmath>
#include <iostream>
#include <vector>

int main() {
  int failures = 0;
  const auto check = [&](bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
  };
  // A 10x8 sprite inside an atlas, with unequal padding on all four sides.
  // Nonzero RGB in transparent texels must not count as visible content.
  std::vector<unsigned char> rgba(16 * 12 * 4, 255);
  for (int y = 2; y < 10; ++y) {
    for (int x = 3; x < 13; ++x) {
      rgba[(y * 16 + x) * 4 + 3] = 0;
    }
  }
  rgba[(5 * 16 + 5) * 4 + 3] = 1;
  rgba[(8 * 16 + 9) * 4 + 3] = 255;
  const auto bounds = image_alpha::visibleBounds(rgba, 16, 12, 3, 2, 10, 8);
  check(bounds.left == 0.2 && bounds.top == 0.375 &&
            bounds.right == 0.7 && bounds.bottom == 0.875,
        "trim only zero alpha borders inside the selected atlas crop");
  const auto rect = image_alpha::trimBottomUp({100, 200, 100, 80}, bounds);
  check(rect.x == 120 && rect.y == 210 &&
            std::abs(rect.width - 50) < 0.00001 && rect.height == 40,
        "place the smaller outline over the visible pixels, including bottom padding");
  const auto lowerHalf = image_alpha::trimBottomUp(
      {100, 200, 100, 80}, {0, 0.5, 1, 1});
  check(lowerHalf.x == 100 && lowerHalf.y == 200 &&
            lowerHalf.width == 100 && lowerHalf.height == 40,
        "transparent top half moves the ghost center down by a quarter of full height");
  std::fill(rgba.begin(), rgba.end(), 0);
  const auto empty = image_alpha::visibleBounds(rgba, 16, 12, 3, 2, 10, 8);
  const auto invisible = image_alpha::trimBottomUp({0, 0, 100, 80}, empty);
  check(invisible.width == 0 && invisible.height == 0,
        "entirely transparent notes have no outline area");
  std::fill(rgba.begin(), rgba.end(), 255);
  const auto opaque = image_alpha::visibleBounds(rgba, 16, 12, 3, 2, 10, 8);
  check(opaque.left == 0 && opaque.top == 0 && opaque.right == 1 && opaque.bottom == 1,
        "opaque sprites keep their full bounds");
  return failures == 0 ? 0 : 1;
}
