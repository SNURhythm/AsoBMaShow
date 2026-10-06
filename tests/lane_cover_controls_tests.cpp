#include "LaneCover.h"

#include <cmath>
#include <iostream>
#include <limits>

int main() {
  int failures = 0;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
  };
  const auto near = [](float a, float b) { return std::abs(a - b) < 0.00001F; };
  lane_cover::State state{.laneCoverPercent = 20, .laneCoverEnabled = true,
                          .liftEnabled = true, .liftRatio = 0.25F,
                          .hiddenEnabled = true, .hiddenRatio = 0.1F};
  check(lane_cover::adjust(state, 0.1F, true) && near(state.laneCoverPercent, 20.1F) &&
            near(state.liftRatio, 0.25F), "sudden takes priority and retains fine scratch steps");
  state.laneCoverEnabled = false;
  check(lane_cover::adjust(state, 1, true) && near(state.liftRatio, 0.24F),
        "down moves the lifted line down when sudden is disabled");
  check(lane_cover::adjust(state, -1, false) && near(state.hiddenRatio, 0.11F),
        "START+SELECT can select hidden instead of lift");
  check(lane_cover::adjust(state, -1000, false) && state.hiddenRatio == 1 &&
            !lane_cover::adjust(state, -1, false), "hidden clamps without reporting phantom changes");
  check(!lane_cover::adjust(state, std::numeric_limits<float>::quiet_NaN(), false),
        "nonfinite input cannot poison live geometry");
  state = {.laneCoverPercent = 20, .laneCoverEnabled = true,
           .liftEnabled = true, .liftRatio = 0.25F,
           .hiddenEnabled = true, .hiddenRatio = 0.1F};
  const auto geometry = lane_cover::geometry(state, 10, 110);
  check(near(geometry.judgeY, 35) && near(geometry.suddenY, 95) &&
            near(geometry.hiddenY, 42.5F), "covers use the lane remaining above lift");
  return failures ? 1 : 0;
}
