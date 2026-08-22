#pragma once

#include "../../audio/PlaybackRate.h"
#include "../../AssistOptionUtils.h"

#include <algorithm>
#include <cstddef>

enum class GaugeType {
  AssistedEasy,
  Easy,
  Normal,
  Hard,
  ExHard,
  Hazard,
  Grade,
  ExGrade,
  ExHardGrade,
};

enum class GaugeAutoShiftMode {
  None = 0,
  SelectToUnder = 1,
  Continue = 2,
  SurvivalToGroove = 3,
  BestClear = 4,
};

inline bool gaugeAutoShiftEnabled(GaugeAutoShiftMode mode) {
  return mode != GaugeAutoShiftMode::None;
}

inline int gaugeAutoShiftModeValue(GaugeAutoShiftMode mode) {
  return static_cast<int>(mode);
}

inline GaugeAutoShiftMode gaugeAutoShiftModeFromValue(int value) {
  switch (value) {
  case 1:
    return GaugeAutoShiftMode::SelectToUnder;
  case 2:
    return GaugeAutoShiftMode::Continue;
  case 3:
    return GaugeAutoShiftMode::SurvivalToGroove;
  case 4:
    return GaugeAutoShiftMode::BestClear;
  default:
    return GaugeAutoShiftMode::None;
  }
}

inline const char *gaugeAutoShiftShortLabel(GaugeAutoShiftMode mode) {
  switch (mode) {
  case GaugeAutoShiftMode::SelectToUnder:
    return "GAS";
  case GaugeAutoShiftMode::Continue:
    return "CONT";
  case GaugeAutoShiftMode::SurvivalToGroove:
    return "S-G";
  case GaugeAutoShiftMode::BestClear:
    return "BEST";
  case GaugeAutoShiftMode::None:
  default:
    return "";
  }
}

inline const char *gaugeAutoShiftMenuLabel(GaugeAutoShiftMode mode) {
  switch (mode) {
  case GaugeAutoShiftMode::SelectToUnder:
    return "Select to Under";
  case GaugeAutoShiftMode::Continue:
    return "Continue at 0%";
  case GaugeAutoShiftMode::SurvivalToGroove:
    return "Survival to Groove";
  case GaugeAutoShiftMode::BestClear:
    return "Best Clear";
  case GaugeAutoShiftMode::None:
  default:
    return "Off";
  }
}

enum class GaugeProfile {
  Standard,
  CourseDefault,
  Course5Keys,
  Course7Keys,
  Course9Keys,
  Course24Keys,
  CourseLR2,
  Standard5Keys,
  Standard9Keys,
  Standard24Keys,
  StandardLr2,
};

enum class ClearType {
  Failed,
  AssistedEasyClear,
  LightAssistedEasyClear,
  EasyClear,
  NormalClear,
  HardClear,
  ExHardClear,
  FullCombo,
};

// Kept distinct from the selected gauge: Beatoraja's BPM Guide changes the
// result lamp, but does not substitute the Assisted Easy gauge rules.
enum class AssistClearMark {
  None,
  LightAssistedEasy,
  AssistedEasy,
};

inline constexpr int kNoClearTypeRank = -1;
inline constexpr int kClearTypeFailedRank = 0;
inline constexpr int kClearTypeAssistedEasyClearRank = 100;
inline constexpr int kClearTypeLightAssistedEasyClearRank = 150;
inline constexpr int kClearTypeEasyClearRank = 200;
inline constexpr int kClearTypeNormalClearRank = 300;
inline constexpr int kClearTypeHardClearRank = 400;
inline constexpr int kClearTypeExHardClearRank = 500;
inline constexpr int kClearTypeFullComboRank = 600;
inline constexpr std::size_t kGaugeTypeCount = 9;

namespace clear_policy {
[[nodiscard]] inline bool
assistClearRequired(const audio::PlaybackRate &playback) noexcept {
  return !playback.neutral();
}

[[nodiscard]] inline AssistClearMark assistClearMarkRequired(
    const std::string &assistOption, double minimumBpm, double maximumBpm,
    const audio::PlaybackRate &playback) noexcept {
  // Local assist options use BMSPlayer's `assist = max(assist, 1)` result
  // class. Altered playback has no upstream counterpart; we deliberately
  // assign it that same Light Assist Easy class.
  if (assistClearRequired(playback)) {
    return AssistClearMark::LightAssistedEasy;
  }
  if (assist_options::isDragMode(assistOption) ||
      assist_options::bpmGuideAffectsClear(assistOption, minimumBpm,
                                            maximumBpm)) {
    return AssistClearMark::LightAssistedEasy;
  }
  return AssistClearMark::None;
}

[[nodiscard]] inline int
capRankForPlayback(int rank, const audio::PlaybackRate &playback) noexcept {
  if (!assistClearRequired(playback) ||
      rank < kClearTypeAssistedEasyClearRank) {
    return rank;
  }
  return kClearTypeLightAssistedEasyClearRank;
}

[[nodiscard]] inline int
fullComboRankForPlayback(int rank, bool fullComboAchieved,
                         const audio::PlaybackRate &playback) noexcept {
  // Light Assist Easy is a final BMSPlayer result class. Its rank is distinct
  // from a player-selected Assisted Easy gauge, which may still earn FC.
  if (fullComboAchieved && rank >= kClearTypeAssistedEasyClearRank &&
      rank != kClearTypeLightAssistedEasyClearRank) {
    rank = std::max(rank, kClearTypeFullComboRank);
  }
  return capRankForPlayback(rank, playback);
}
} // namespace clear_policy

inline int gaugeTypeIndex(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return 0;
  case GaugeType::Easy:
    return 1;
  case GaugeType::Normal:
    return 2;
  case GaugeType::Hard:
    return 3;
  case GaugeType::ExHard:
    return 4;
  case GaugeType::Hazard:
    return 5;
  case GaugeType::Grade:
    return 6;
  case GaugeType::ExGrade:
    return 7;
  case GaugeType::ExHardGrade:
    return 8;
  default:
    return 2;
  }
}

inline GaugeType gaugeTypeAtIndex(int index) {
  switch (index) {
  case 0:
    return GaugeType::AssistedEasy;
  case 1:
    return GaugeType::Easy;
  case 2:
    return GaugeType::Normal;
  case 3:
    return GaugeType::Hard;
  case 4:
    return GaugeType::ExHard;
  case 5:
    return GaugeType::Hazard;
  case 6:
    return GaugeType::Grade;
  case 7:
    return GaugeType::ExGrade;
  case 8:
    return GaugeType::ExHardGrade;
  default:
    return GaugeType::Normal;
  }
}

inline const char *gaugeTypeToLabel(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return "ASSISTED EASY";
  case GaugeType::Easy:
    return "EASY";
  case GaugeType::Normal:
    return "NORMAL";
  case GaugeType::Hard:
    return "HARD";
  case GaugeType::ExHard:
    return "EX-HARD";
  case GaugeType::Hazard:
    return "HAZARD";
  case GaugeType::Grade:
    return "GRADE";
  case GaugeType::ExGrade:
    return "EX GRADE";
  case GaugeType::ExHardGrade:
    return "EXHARD GRADE";
  default:
    return "NORMAL";
  }
}

inline const char *gaugeTypeToShortLabel(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return "A-EASY";
  case GaugeType::Easy:
    return "EASY";
  case GaugeType::Normal:
    return "NORMAL";
  case GaugeType::Hard:
    return "HARD";
  case GaugeType::ExHard:
    return "EX-HARD";
  case GaugeType::Hazard:
    return "HAZARD";
  case GaugeType::Grade:
    return "CLASS";
  case GaugeType::ExGrade:
    return "EX-CLASS";
  case GaugeType::ExHardGrade:
    return "EXH-CLASS";
  default:
    return "NORMAL";
  }
}

inline int gaugeTypeToClearRank(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return kClearTypeAssistedEasyClearRank;
  case GaugeType::Easy:
    return kClearTypeEasyClearRank;
  case GaugeType::Normal:
    return kClearTypeNormalClearRank;
  case GaugeType::Hard:
    return kClearTypeHardClearRank;
  case GaugeType::ExHard:
    return kClearTypeExHardClearRank;
  case GaugeType::Hazard:
    return kClearTypeFullComboRank;
  case GaugeType::Grade:
    return kClearTypeNormalClearRank;
  case GaugeType::ExGrade:
    return kClearTypeHardClearRank;
  case GaugeType::ExHardGrade:
    return kClearTypeExHardClearRank;
  default:
    return kClearTypeNormalClearRank;
  }
}

inline bool gaugeProfileIsCourse(GaugeProfile profile) {
  switch (profile) {
  case GaugeProfile::CourseDefault:
  case GaugeProfile::Course5Keys:
  case GaugeProfile::Course7Keys:
  case GaugeProfile::Course9Keys:
  case GaugeProfile::Course24Keys:
  case GaugeProfile::CourseLR2:
    return true;
  case GaugeProfile::Standard:
  case GaugeProfile::Standard5Keys:
  case GaugeProfile::Standard9Keys:
  case GaugeProfile::Standard24Keys:
  case GaugeProfile::StandardLr2:
  default:
    return false;
  }
}

inline GaugeProfile gaugeProfileForKeyMode(int keyMode, bool course) {
  switch (keyMode) {
  case 5:
  case 10:
    return course ? GaugeProfile::Course5Keys
                  : GaugeProfile::Standard5Keys;
  case 9:
  case 18:
    return course ? GaugeProfile::Course9Keys
                  : GaugeProfile::Standard9Keys;
  case 24:
  case 48:
    return course ? GaugeProfile::Course24Keys
                  : GaugeProfile::Standard24Keys;
  default:
    return course ? GaugeProfile::Course7Keys : GaugeProfile::Standard;
  }
}

inline GaugeProfile resolveGaugeProfile(GaugeProfile profile, int keyMode) {
  if (profile == GaugeProfile::Standard) {
    return gaugeProfileForKeyMode(keyMode, false);
  }
  if (profile == GaugeProfile::CourseDefault) {
    return gaugeProfileForKeyMode(keyMode, true);
  }
  return profile;
}

inline const char *courseGaugeTypeToShortLabel(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::Hard:
    return "EX-CLASS";
  case GaugeType::ExHard:
  case GaugeType::Hazard:
  case GaugeType::ExHardGrade:
    return "EXH-CLASS";
  case GaugeType::ExGrade:
    return "EX-CLASS";
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  case GaugeType::Grade:
  default:
    return "CLASS";
  }
}

inline const char *gaugeDisplayShortLabel(GaugeType gaugeType,
                                          GaugeProfile profile) {
  return gaugeProfileIsCourse(profile) ? courseGaugeTypeToShortLabel(gaugeType)
                                       : gaugeTypeToShortLabel(gaugeType);
}

inline int clearTypeToRank(ClearType clearType) {
  switch (clearType) {
  case ClearType::AssistedEasyClear:
    return kClearTypeAssistedEasyClearRank;
  case ClearType::LightAssistedEasyClear:
    return kClearTypeLightAssistedEasyClearRank;
  case ClearType::EasyClear:
    return kClearTypeEasyClearRank;
  case ClearType::NormalClear:
    return kClearTypeNormalClearRank;
  case ClearType::HardClear:
    return kClearTypeHardClearRank;
  case ClearType::ExHardClear:
    return kClearTypeExHardClearRank;
  case ClearType::FullCombo:
    return kClearTypeFullComboRank;
  case ClearType::Failed:
  default:
    return kClearTypeFailedRank;
  }
}

inline const char *clearTypeToLabel(ClearType clearType) {
  switch (clearType) {
  case ClearType::AssistedEasyClear:
    return "ASSIST EASY CLEAR";
  case ClearType::LightAssistedEasyClear:
    return "LIGHT ASSIST EASY CLEAR";
  case ClearType::EasyClear:
    return "EASY CLEAR";
  case ClearType::NormalClear:
    return "NORMAL CLEAR";
  case ClearType::HardClear:
    return "HARD CLEAR";
  case ClearType::ExHardClear:
    return "EX-HARD CLEAR";
  case ClearType::FullCombo:
    return "FULL COMBO";
  case ClearType::Failed:
  default:
    return "FAILED";
  }
}

inline const char *clearTypeRankToLabel(int rank) {
  if (rank >= kClearTypeFullComboRank) {
    return "FULL COMBO";
  }
  if (rank >= kClearTypeExHardClearRank) {
    return "EX-HARD CLEAR";
  }
  if (rank >= kClearTypeHardClearRank) {
    return "HARD CLEAR";
  }
  if (rank >= kClearTypeNormalClearRank) {
    return "NORMAL CLEAR";
  }
  if (rank >= kClearTypeEasyClearRank) {
    return "EASY CLEAR";
  }
  if (rank >= kClearTypeLightAssistedEasyClearRank) {
    return "LIGHT ASSIST EASY CLEAR";
  }
  if (rank >= kClearTypeAssistedEasyClearRank) {
    return "ASSIST EASY CLEAR";
  }
  if (rank == kNoClearTypeRank) {
    return "NO PLAY";
  }
  return "FAILED";
}
