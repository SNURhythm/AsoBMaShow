#pragma once

#include "../skin/beatoraja/GameplaySkinEndAnimation.h"
#include <optional>

namespace settings_scene {

[[nodiscard]] inline std::int32_t previewPlaytimeMillis(long long lastNoteMicros) noexcept {
  return static_cast<std::int32_t>(
      skin::beatorajaGameplayStatePlayDeadlineMicros(lastNoteMicros) / 1'000);
}

// Track frames actually shown, so a slow frame cannot skip the skin's finish
// margin or fadeout. These are the same strict, millisecond transitions used
// by the gameplay skin bridge, with one rendered frame between transitions.
struct PreviewEndAnimation {
  std::optional<long long> musicEndMicros;
  std::optional<long long> fadeoutMicros;
  bool complete = false;

  void observeRenderedFrame(long long elapsedMicros, long long lastNoteMicros,
                            const skin::SkinGameplayTiming &timing) noexcept {
    if (!musicEndMicros) {
      if (skin::beatorajaGameplayStateFinished(
              elapsedMicros, previewPlaytimeMillis(lastNoteMicros)))
        musicEndMicros = elapsedMicros;
    } else if (!fadeoutMicros) {
      if ((elapsedMicros - *musicEndMicros) / 1'000 > timing.finishMarginMillis)
        fadeoutMicros = elapsedMicros;
    } else if ((elapsedMicros - *fadeoutMicros) / 1'000 > timing.fadeoutMillis) {
      complete = true;
    }
  }
};

} // namespace settings_scene
