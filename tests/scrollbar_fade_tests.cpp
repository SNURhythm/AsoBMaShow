#include "view/ScrollbarFade.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  try {
    auto state = scrollbarPresentation(0, 1, 1, false);
    require(state.alpha == 0 && state.animate,
            "a reveal on the initial SDL tick still schedules fade-in");
    state = scrollbarPresentation(1000, 1000, 1000, false);
    require(state.alpha == 0 && state.animate,
            "the initially transparent fade needs a following frame");
    state = scrollbarPresentation(1060, 1000, 1000, false);
    require(std::abs(state.alpha - 0.5f) < 0.001f && state.animate,
            "fade-in keeps its smooth midpoint and animation demand");
    state = scrollbarPresentation(1120, 1000, 1000, false);
    require(state.alpha == 1 && !state.animate && state.redrawAt == 1650,
            "the stationary hold sleeps until the fade-out boundary");
    state = scrollbarPresentation(1650, 1000, 1000, false);
    require(state.alpha == 1 && state.animate,
            "the exact hold boundary starts fade-out frame demand");
    state = scrollbarPresentation(1890, 1000, 1000, false);
    require(std::abs(state.alpha - 0.5f) < 0.001f && state.animate,
            "fade-out requests frames until its last visible sample");
    state = scrollbarPresentation(2130, 1000, 1000, false);
    require(state.alpha == 0 && !state.animate && !state.redrawAt,
            "an expired scrollbar has no future drawing obligation");
    state = scrollbarPresentation(3000, 1000, 1000, true);
    require(state.alpha == 1 && !state.animate && !state.redrawAt,
            "a stationary active drag remains visible without polling frames");
    state = scrollbarPresentation(1000, 0, 0, false);
    require(state.alpha == 0 && !state.animate && !state.redrawAt,
            "an unrevealed scrollbar stays idle");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
