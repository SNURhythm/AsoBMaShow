#pragma once

#include <algorithm>
#include <cmath>

namespace ios_drawable {

struct Size {
  int width = 0;
  int height = 0;
};

// UIScreen.currentMode and bounds * scale can describe a supersampled Display
// Zoom mode. Fullscreen Metal drawables should use nativeBounds, in the window's
// orientation. Windowed scenes retain SDL's pixel size.
inline Size fullscreenNativeSize(Size current, Size logical, Size window,
                                 Size native, bool fullscreen) {
  if (!fullscreen || logical.width <= 0 || logical.height <= 0 ||
      window.width <= 0 || window.height <= 0 ||
      native.width <= 0 || native.height <= 0 ||
      std::abs(logical.width - window.width) > 2 ||
      std::abs(logical.height - window.height) > 2) {
    return current;
  }
  if ((native.width >= native.height) != (window.width >= window.height)) {
    std::swap(native.width, native.height);
  }
  const double aspect = static_cast<double>(window.width) / window.height;
  const double nativeAspect = static_cast<double>(native.width) / native.height;
  if (std::abs(nativeAspect - aspect) > aspect * 0.02) {
    return current;
  }
  return native;
}

} // namespace ios_drawable
