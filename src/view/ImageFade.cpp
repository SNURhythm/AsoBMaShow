#include "ImageFade.h"

#include <algorithm>

ImageFade makeImageFade(ImageFadeDirection direction, float strength) {
  return {.direction = direction,
          .strength = std::clamp(strength, 0.0F, 1.0F)};
}

std::array<float, 4> imageFadeShaderParameters(const ImageFade &fade) {
  switch (fade.direction) {
  case ImageFadeDirection::LeftToRight:
    return {1.0F, 0.0F, 0.0F, fade.strength};
  case ImageFadeDirection::RightToLeft:
    return {-1.0F, 0.0F, 1.0F, fade.strength};
  case ImageFadeDirection::TopToBottom:
    return {0.0F, 1.0F, 0.0F, fade.strength};
  case ImageFadeDirection::BottomToTop:
    return {0.0F, -1.0F, 1.0F, fade.strength};
  }
  return {1.0F, 0.0F, 0.0F, fade.strength};
}
