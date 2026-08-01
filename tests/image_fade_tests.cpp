#include "view/ImageFade.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const auto leftToRight =
      makeImageFade(ImageFadeDirection::LeftToRight, 2.0F);
  require(leftToRight.strength == 1.0F,
          "fade strength clamps high values");
  require(imageFadeShaderParameters(leftToRight) ==
              std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F},
          "left-to-right maps x progress and clamped strength");

  const auto rightToLeft =
      makeImageFade(ImageFadeDirection::RightToLeft, 0.75F);
  require(imageFadeShaderParameters(rightToLeft) ==
              std::array<float, 4>{-1.0F, 0.0F, 1.0F, 0.75F},
          "right-to-left reverses x progress");

  const auto topToBottom =
      makeImageFade(ImageFadeDirection::TopToBottom, -1.0F);
  require(imageFadeShaderParameters(topToBottom) ==
              std::array<float, 4>{0.0F, 1.0F, 0.0F, 0.0F},
          "top-to-bottom maps y progress and clamps low strength");

  const auto bottomToTop =
      makeImageFade(ImageFadeDirection::BottomToTop, 0.5F);
  require(imageFadeShaderParameters(bottomToTop) ==
              std::array<float, 4>{0.0F, -1.0F, 1.0F, 0.5F},
          "bottom-to-top reverses y progress");

  return 0;
}
