#pragma once

#include "../skin/GameplaySkinActivationRequest.h"

#include <optional>
#include <string>
#include <vector>

enum class MusicSelectLaunchKind { BuiltIn, SelectedSkin, Error };

struct MusicSelectLaunchDecision {
  MusicSelectLaunchKind kind = MusicSelectLaunchKind::BuiltIn;
  std::string selectedSkinPath;
  std::optional<skin::GameplaySkinActivationRequest> request;
  std::vector<skin::SkinDiagnostic> diagnostics;
};

[[nodiscard]] MusicSelectLaunchDecision
decideMusicSelectLaunch(skin::GameplaySkinAcquisition);

[[nodiscard]] std::string
musicSelectSkinEntryPath(const skin::SkinEntryId &entry);

[[nodiscard]] std::string
musicSelectSkinFailureReason(std::size_t index,
                             const skin::SkinDiagnostic &diagnostic);
