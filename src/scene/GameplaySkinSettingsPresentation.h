#pragma once

#include "GameplaySkinSettingsController.h"

#include <string>

namespace skin {

struct GameplaySkinSettingsActionAvailability {
  bool ordinaryActions = false;
  bool canCancel = false;
  bool canEditPreparedName = false;
  bool canInstallPrepared = false;
};

[[nodiscard]] GameplaySkinSettingsActionAvailability
gameplaySkinSettingsActionAvailability(
    const GameplaySkinSettingsSnapshot &snapshot) noexcept;

// Returns a canonical, unambiguous encoding of every snapshot field that can
// affect the rendered settings tab or any action exposed by it.
[[nodiscard]] std::string gameplaySkinSettingsPresentationKey(
    const GameplaySkinSettingsSnapshot &snapshot);

[[nodiscard]] ViewportSettings
gameplaySkinViewportWithMode(ViewportSettings current,
                             ViewportMode mode) noexcept;

[[nodiscard]] ViewportSettings
gameplaySkinViewportWithCustomBase(ViewportSettings current,
                                   CustomViewportBase customBase) noexcept;

} // namespace skin
