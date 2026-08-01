#include "scene/play/LaneCoverNumberGeometry.h"

#include <iostream>

int main() {
  using namespace lane_cover_number;
  int failures = 0;
  const auto check = [&](bool value, const char *label) {
    if (!value) {
      std::cerr << "FAIL: " << label << '\n';
      ++failures;
    }
  };

  check(whiteNumberLabel(0) == "0", "zero percent renders as zero");
  check(whiteNumberLabel(37) == "370", "percentage is multiplied by ten");
  check(whiteNumberLabel(100) == "1000", "full cover renders as 1000");

  const auto centered = centerPair(500, 60, 80, 12);
  check(centered.whiteX == 424, "white number starts at pair left");
  check(centered.greenX == 496, "green number follows the gap");
  check(centered.right() == 576, "pair ends symmetrically around center");
  check(centered.center() == 500, "combined pair is centered");

  return failures == 0 ? 0 : 1;
}
