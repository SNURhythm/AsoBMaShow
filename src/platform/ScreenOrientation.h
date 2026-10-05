#pragma once

#include "../targets.h"

namespace screen_orientation {

// Persisted values: keep existing entries stable.
enum class Mode { Auto = 0, Landscape = 1, Portrait = 2 };

inline Mode sanitize(Mode mode) {
  switch (mode) {
  case Mode::Auto:
  case Mode::Landscape:
  case Mode::Portrait:
    return mode;
  }
  return Mode::Landscape;
}

// The native implementation captures the interface orientation on the first
// lock request and retains it across background/resume and gameplay retries.
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR || TARGET_OS_ANDROID
void apply(Mode mode, bool lockCurrent);
#else
inline void apply(Mode, bool) {}
#endif

} // namespace screen_orientation
