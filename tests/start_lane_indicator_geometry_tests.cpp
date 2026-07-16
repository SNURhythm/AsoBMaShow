#include "scene/play/StartLaneIndicatorGeometry.h"

#include <cmath>
#include <iostream>

int main() {
  using namespace start_lane_indicator;
  int failures = 0;
  const auto check = [&](bool value, const char *label) {
    if (!value) {
      std::cerr << "FAIL: " << label << '\n';
      ++failures;
    }
  };

  check(colorRoleForKey(0, 7) == ColorRole::White, "7K edge is white");
  check(colorRoleForKey(1, 7) == ColorRole::Blue, "7K next key is blue");
  check(colorRoleForKey(3, 7) == ColorRole::Blue, "7K center is blue");
  check(colorRoleForScratch() == ColorRole::Red, "scratch is red");
  check(kIndicatorDepth > kLaneCoverDepth,
        "overlapping triangle renders above the lane cover");

  const auto roomy = placeTriangle(2.0F, 1.0F, 0.0F, 6.0F);
  check(std::fabs(roomy.baseY - (6.0F - kCoverGap)) < 0.0001F,
        "triangle base keeps the cover gap");
  check(roomy.tipY < roomy.baseY, "triangle points toward the judge line");
  check(!roomy.overlapsCover, "roomy triangle stays outside cover");

  const auto covered = placeTriangle(2.0F, 1.0F, 0.0F, 0.1F);
  check(covered.baseY > 0.1F, "triangle stops before crossing judge line");
  check(covered.overlapsCover, "long cover overlaps triangle geometry");
  return failures == 0 ? 0 : 1;
}
