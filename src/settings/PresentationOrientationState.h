#pragma once
#include "PresentationOrientation.h"

namespace player_settings {
class PresentationOrientationState {
public:
  bool updateViewport(int width, int height) {
    if (width > 0 && height > 0 && width != height) {
      pending_ = height > width ? PresentationOrientation::Portrait
                               : PresentationOrientation::Landscape;
    }
    const auto previous = orientation_;
    if (!locked_) orientation_ = pending_;
    return previous != orientation_;
  }
  void setGameplayLocked(bool locked) {
    locked_ = locked;
    if (!locked_) orientation_ = pending_;
  }
  PresentationOrientation orientation() const { return orientation_; }
private:
  PresentationOrientation orientation_ = PresentationOrientation::Landscape;
  PresentationOrientation pending_ = PresentationOrientation::Landscape;
  bool locked_ = false;
};
} // namespace player_settings
