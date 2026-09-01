#pragma once

#include "../skin/GameplaySkinActivationRequest.h"

#include <optional>
#include <vector>

enum class MusicSelectLaunchKind { BuiltIn, SelectedSkin, Error };

struct MusicSelectLaunchDecision {
  MusicSelectLaunchKind kind = MusicSelectLaunchKind::BuiltIn;
  std::optional<skin::GameplaySkinActivationRequest> request;
  std::vector<skin::SkinDiagnostic> diagnostics;
};

[[nodiscard]] MusicSelectLaunchDecision
decideMusicSelectLaunch(skin::GameplaySkinAcquisition);
