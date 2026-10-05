#pragma once

#include "common.h"
#include "../targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <cmath>

namespace rendering {
struct UiSafeAreaInsets {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
};

inline UiSafeAreaInsets uiSafeAreaInsets() {
  UiSafeAreaInsets result;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto normalized = GetIOSSafeAreaInsetsNormalized();
  result.top = static_cast<int>(std::lround(normalized.top * window_height));
  result.right = static_cast<int>(std::lround(normalized.right * window_width));
  result.bottom = static_cast<int>(std::lround(normalized.bottom * window_height));
  result.left = static_cast<int>(std::lround(normalized.left * window_width));
#endif
  return result;
}
} // namespace rendering
