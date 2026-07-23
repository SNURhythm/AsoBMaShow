#pragma once

#include <array>

enum class ImageFadeDirection {
  LeftToRight,
  RightToLeft,
  TopToBottom,
  BottomToTop,
};

struct ImageFade {
  ImageFadeDirection direction = ImageFadeDirection::LeftToRight;
  float strength = 0.0F;

  bool operator==(const ImageFade &) const = default;
};

ImageFade makeImageFade(ImageFadeDirection direction, float strength);
std::array<float, 4> imageFadeShaderParameters(const ImageFade &fade);
