#pragma once

#include <array>

namespace player_settings {
enum class PresentationOrientation { Landscape = 0, Portrait = 1 };
inline constexpr std::array kPresentationOrientations = {
    PresentationOrientation::Landscape, PresentationOrientation::Portrait};
inline constexpr const char *presentationOrientationName(PresentationOrientation orientation) {
  return orientation == PresentationOrientation::Portrait ? "portrait" : "landscape";
}
} // namespace player_settings
