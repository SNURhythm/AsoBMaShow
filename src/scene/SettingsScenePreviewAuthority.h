#pragma once

#include "../AppSettings.h"
#include "play/PlayfieldVisualState.h"

namespace settings_scene {

[[nodiscard]] inline GameplayLaneCoverAuthority
previewLaneCoverAuthority(const AppSettings &settings) noexcept {
  return gameplayLaneCoverAuthority(settings.presentation().noteStartPositionPercent,
                                    settings.presentation().laneCoverEnabled);
}

} // namespace settings_scene
