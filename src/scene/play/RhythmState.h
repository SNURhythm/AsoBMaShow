#pragma once
#include "../../audio/PlaybackRate.h"
#include "../../bms_parser.hpp"
#include "Judge.h"
#include <algorithm>
#include <array>
#include <string>

enum class GaugeType { AssistedEasy, Easy, Normal, Hard, ExHard };

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
};

enum class ClearType {
  Failed,
  AssistedEasyClear,
  EasyClear,
  NormalClear,
  HardClear,
  ExHardClear,
};

inline constexpr int kNoClearTypeRank = -1;
inline constexpr int kClearTypeFailedRank = 0;
inline constexpr int kClearTypeAssistedEasyClearRank = 100;
inline constexpr int kClearTypeEasyClearRank = 200;
inline constexpr int kClearTypeNormalClearRank = 300;
inline constexpr int kClearTypeHardClearRank = 400;
inline constexpr int kClearTypeExHardClearRank = 500;
inline constexpr int kClearTypeFullComboRank = 600;
inline constexpr size_t kGaugeTypeCount = 5;

namespace clear_policy {
[[nodiscard]] inline bool
assistClearRequired(const audio::PlaybackRate &playback) noexcept {
  return !playback.neutral();
}

[[nodiscard]] inline int
capRankForPlayback(int rank, const audio::PlaybackRate &playback) noexcept {
  if (!assistClearRequired(playback) ||
      rank < kClearTypeAssistedEasyClearRank) {
    return rank;
  }
  return kClearTypeAssistedEasyClearRank;
}

[[nodiscard]] inline int
fullComboRankForPlayback(int rank, bool fullComboAchieved,
                         const audio::PlaybackRate &playback) noexcept {
  if (fullComboAchieved && rank >= kClearTypeAssistedEasyClearRank) {
    rank = std::max(rank, kClearTypeFullComboRank);
  }
  return capRankForPlayback(rank, playback);
}
} // namespace clear_policy

struct JudgementFastSlowCount {
  int fast = 0;
  int slow = 0;
};

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
    return "EXH-CLASS";
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  default:
    return "CLASS";
  }
}

inline const char *gaugeDisplayShortLabel(GaugeType gaugeType,
                                          GaugeProfile profile) {
  return gaugeProfileIsCourse(profile) ? courseGaugeTypeToShortLabel(gaugeType)
                                       : gaugeTypeToShortLabel(gaugeType);
}

inline float gaugeInitialValue(GaugeType gaugeType,
                               GaugeProfile profile = GaugeProfile::Standard) {
  if (gaugeProfileIsCourse(profile)) {
    return 100.0f;
  }
  if (profile == GaugeProfile::Standard9Keys) {
    switch (gaugeType) {
    case GaugeType::AssistedEasy:
    case GaugeType::Easy:
    case GaugeType::Normal:
      return 30.0f;
    case GaugeType::Hard:
    case GaugeType::ExHard:
    default:
      return 100.0f;
    }
  }
  if (profile == GaugeProfile::Standard24Keys &&
      gaugeType == GaugeType::AssistedEasy) {
    return 30.0f;
  }
  switch (gaugeType) {
  case GaugeType::Hard:
  case GaugeType::ExHard:
    return 100.0f;
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  default:
    return 20.0f;
  }
}

inline float gaugeMaximumValue(GaugeType gaugeType,
                               GaugeProfile profile = GaugeProfile::Standard) {
  if (profile == GaugeProfile::Standard9Keys &&
      (gaugeType == GaugeType::AssistedEasy ||
       gaugeType == GaugeType::Easy || gaugeType == GaugeType::Normal)) {
    return 120.0f;
  }
  return 100.0f;
}

inline float gaugeMinimumValue(GaugeType gaugeType,
                               GaugeProfile profile = GaugeProfile::Standard) {
  if (gaugeProfileIsCourse(profile)) {
    return 0.0f;
  }
  switch (gaugeType) {
  case GaugeType::Hard:
  case GaugeType::ExHard:
    return 0.0f;
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  default:
    return 2.0f;
  }
}

inline float gaugeBorderValue(GaugeType gaugeType,
                              GaugeProfile profile = GaugeProfile::Standard) {
  if (gaugeProfileIsCourse(profile)) {
    return 0.0f;
  }
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    switch (profile) {
    case GaugeProfile::Standard5Keys:
    case GaugeProfile::Standard24Keys:
      return 50.0f;
    case GaugeProfile::Standard9Keys:
      return 65.0f;
    default:
      return 60.0f;
    }
  case GaugeType::Easy:
    if (profile == GaugeProfile::Standard5Keys) {
      return 75.0f;
    }
    if (profile == GaugeProfile::Standard9Keys) {
      return 85.0f;
    }
    if (profile == GaugeProfile::Standard24Keys) {
      return 70.0f;
    }
    return 80.0f;
  case GaugeType::Normal:
    if (profile == GaugeProfile::Standard5Keys) {
      return 75.0f;
    }
    if (profile == GaugeProfile::Standard9Keys) {
      return 85.0f;
    }
    if (profile == GaugeProfile::Standard24Keys) {
      return 70.0f;
    }
    return 80.0f;
  case GaugeType::Hard:
  case GaugeType::ExHard:
  default:
    return 0.0f;
  }
}

inline bool gaugeIsSurvival(GaugeType gaugeType,
                            GaugeProfile profile = GaugeProfile::Standard) {
  if (gaugeProfileIsCourse(profile)) {
    return true;
  }
  return gaugeType == GaugeType::Hard || gaugeType == GaugeType::ExHard;
}

inline int gaugeJudgementIndex(Judgement judgement) {
  switch (judgement) {
  case PGreat:
    return 0;
  case Great:
    return 1;
  case Good:
    return 2;
  case Bad:
    return 3;
  case Poor:
    return 4;
  case Kpoor:
    return 5;
  case None:
  default:
    return -1;
  }
}

inline float gaugeBaseDeltaForJudgement(
    GaugeType gaugeType, Judgement judgement,
    GaugeProfile profile = GaugeProfile::Standard) {
  const int index = gaugeJudgementIndex(judgement);
  if (index < 0) {
    return 0.0f;
  }
  using GaugeTable = std::array<std::array<float, 6>, kGaugeTypeCount>;
  constexpr GaugeTable sevenKeysTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.5f, -3.0f, -0.5f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.5f, -4.5f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -3.0f, -6.0f, -2.0f},
      std::array<float, 6>{0.15f, 0.12f, 0.03f, -5.0f, -10.0f, -5.0f},
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -8.0f, -16.0f, -8.0f},
  };
  constexpr GaugeTable fiveKeysTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.5f, -3.0f, -0.5f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.5f, -4.5f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -3.0f, -6.0f, -2.0f},
      std::array<float, 6>{0.0f, 0.0f, 0.0f, -5.0f, -10.0f, -5.0f},
      std::array<float, 6>{0.0f, 0.0f, 0.0f, -10.0f, -20.0f, -10.0f},
  };
  constexpr GaugeTable pmsTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -2.0f, -2.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -3.0f, -3.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -2.0f, -6.0f, -6.0f},
      std::array<float, 6>{0.15f, 0.12f, 0.03f, -5.0f, -10.0f, -10.0f},
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -10.0f, -15.0f, -15.0f},
  };
  constexpr GaugeTable keyboardTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -2.0f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -3.0f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -2.0f, -4.0f, -2.0f},
      std::array<float, 6>{0.2f, 0.2f, 0.1f, -4.0f, -8.0f, -4.0f},
      std::array<float, 6>{0.2f, 0.1f, 0.0f, -6.0f, -12.0f, -6.0f},
  };

  const GaugeTable *table = &sevenKeysTable;
  switch (profile) {
  case GaugeProfile::Standard5Keys:
    table = &fiveKeysTable;
    break;
  case GaugeProfile::Standard9Keys:
    table = &pmsTable;
    break;
  case GaugeProfile::Standard24Keys:
    table = &keyboardTable;
    break;
  default:
    break;
  }
  return (*table)[gaugeTypeIndex(gaugeType)][index];
}

inline int courseGaugeClassIndexForType(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::Hard:
    return 1;
  case GaugeType::ExHard:
    return 2;
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  default:
    return 0;
  }
}

inline float gaugeReducedDamageZoneUpperBound(
    GaugeType gaugeType,
    GaugeProfile profile = GaugeProfile::Standard) {
  if (!gaugeProfileIsCourse(profile)) {
    return gaugeType == GaugeType::Hard &&
                   profile != GaugeProfile::Standard5Keys
               ? 50.0f
               : 0.0f;
  }

  const int classIndex = courseGaugeClassIndexForType(gaugeType);
  if (profile == GaugeProfile::CourseLR2 && classIndex <= 1) {
    return 30.0f;
  }
  if (profile != GaugeProfile::Course5Keys &&
      profile != GaugeProfile::CourseLR2 && classIndex == 0) {
    return 25.0f;
  }
  return 0.0f;
}

inline float courseGaugeBaseDeltaForJudgement(GaugeProfile profile,
                                              GaugeType gaugeType,
                                              Judgement judgement) {
  const int judgementIndex = gaugeJudgementIndex(judgement);
  if (judgementIndex < 0) {
    return 0.0f;
  }
  const int classIndex = courseGaugeClassIndexForType(gaugeType);
  using CourseGaugeTable = std::array<std::array<float, 6>, 3>;

  constexpr CourseGaugeTable defaultTable = {
      std::array<float, 6>{0.15f, 0.12f, 0.06f, -1.5f, -3.0f, -1.5f},
      std::array<float, 6>{0.15f, 0.12f, 0.03f, -3.0f, -6.0f, -3.0f},
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -5.0f, -10.0f, -5.0f},
  };
  constexpr CourseGaugeTable fiveKeysTable = {
      std::array<float, 6>{0.01f, 0.01f, 0.0f, -0.5f, -1.0f, -0.5f},
      std::array<float, 6>{0.01f, 0.01f, 0.0f, -1.0f, -2.0f, -1.0f},
      std::array<float, 6>{0.01f, 0.01f, 0.0f, -2.5f, -5.0f, -2.5f},
  };
  constexpr CourseGaugeTable pmsTable = {
      std::array<float, 6>{0.15f, 0.12f, 0.06f, -1.5f, -3.0f, -3.0f},
      std::array<float, 6>{0.15f, 0.12f, 0.03f, -3.0f, -6.0f, -6.0f},
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -5.0f, -10.0f, -10.0f},
  };
  constexpr CourseGaugeTable keyboardTable = {
      std::array<float, 6>{0.2f, 0.2f, 0.1f, -1.5f, -3.0f, -1.5f},
      std::array<float, 6>{0.2f, 0.2f, 0.1f, -3.0f, -6.0f, -3.0f},
      std::array<float, 6>{0.2f, 0.1f, 0.0f, -5.0f, -10.0f, -5.0f},
  };
  constexpr CourseGaugeTable lr2Table = {
      std::array<float, 6>{0.10f, 0.10f, 0.05f, -2.0f, -3.0f, -2.0f},
      std::array<float, 6>{0.10f, 0.10f, 0.05f, -6.0f, -10.0f, -2.0f},
      std::array<float, 6>{0.10f, 0.10f, 0.05f, -12.0f, -20.0f, -2.0f},
  };

  const CourseGaugeTable *table = &defaultTable;
  switch (profile) {
  case GaugeProfile::Course5Keys:
    table = &fiveKeysTable;
    break;
  case GaugeProfile::Course9Keys:
    table = &pmsTable;
    break;
  case GaugeProfile::Course24Keys:
    table = &keyboardTable;
    break;
  case GaugeProfile::CourseLR2:
    table = &lr2Table;
    break;
  case GaugeProfile::Course7Keys:
  case GaugeProfile::CourseDefault:
  case GaugeProfile::Standard:
  default:
    table = &defaultTable;
    break;
  }
  return (*table)[classIndex][judgementIndex];
}

inline float beatorajaHardRecoveryMultiplier(double total, int totalNotes) {
  const float noteCount = static_cast<float>(std::max(1, totalNotes));
  const float pg = std::max(
      std::min(0.15f, static_cast<float>((2.0 * total - 320.0) / noteCount)),
      0.0f);
  return pg / 0.15f;
}

inline double beatorajaDefaultGaugeTotal(int keyMode, int totalNotes) {
  const double notes = static_cast<double>(std::max(0, totalNotes));
  if (keyMode == 24 || keyMode == 48) {
    return std::max(300.0,
                    7.605 * (notes + 100.0) / (0.01 * notes + 6.5));
  }
  return std::max(260.0, 7.605 * notes / (0.01 * notes + 6.5));
}

inline float beatorajaDamageMultiplier(double total, int totalNotes) {
  constexpr std::array<double, 10> totalThresholds = {
      240.0, 230.0, 210.0, 200.0, 180.0,
      160.0, 150.0, 130.0, 120.0, 0.0,
  };
  constexpr std::array<float, 10> totalMultipliers = {
      1.0f, 1.11f, 1.25f, 1.5f, 1.666f,
      2.0f, 2.5f, 3.333f, 5.0f, 10.0f,
  };
  size_t totalIndex = 0;
  while (totalIndex + 1 < totalThresholds.size() &&
         total < totalThresholds[totalIndex]) {
    ++totalIndex;
  }

  float noteMultiplier = 1.0f;
  int note = 1000;
  float scale = 0.002f;
  while (note > totalNotes || note > 1) {
    noteMultiplier +=
        scale * static_cast<float>(
                    note - std::max(totalNotes, note / 2));
    note /= 2;
    scale *= 2.0f;
  }
  return std::max(totalMultipliers[totalIndex], noteMultiplier);
}

inline float applyHardGaugeGuts(float currentGauge, float delta) {
  if (delta >= 0.0f) {
    return delta;
  }
  constexpr std::array<float, 5> thresholds = {10.0f, 20.0f, 30.0f, 40.0f,
                                               50.0f};
  constexpr std::array<float, 5> multipliers = {0.4f, 0.5f, 0.6f, 0.7f,
                                                0.8f};
  for (size_t i = 0; i < thresholds.size(); i++) {
    if (currentGauge < thresholds[i]) {
      return delta * multipliers[i];
    }
  }
  return delta;
}

inline float applyCourseGaugeGuts(GaugeProfile profile, GaugeType gaugeType,
                                  float currentGauge, float delta) {
  if (delta >= 0.0f) {
    return delta;
  }
  const int classIndex = courseGaugeClassIndexForType(gaugeType);
  if ((profile == GaugeProfile::CourseLR2 && classIndex <= 1) ||
      (profile != GaugeProfile::Course5Keys && profile != GaugeProfile::CourseLR2 &&
       classIndex == 0)) {
    const std::array<float, 5> thresholds =
        profile == GaugeProfile::CourseLR2
            ? std::array<float, 5>{30.0f, 0.0f, 0.0f, 0.0f, 0.0f}
            : std::array<float, 5>{5.0f, 10.0f, 15.0f, 20.0f, 25.0f};
    const std::array<float, 5> multipliers =
        profile == GaugeProfile::CourseLR2
            ? std::array<float, 5>{0.6f, 1.0f, 1.0f, 1.0f, 1.0f}
            : std::array<float, 5>{0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    const size_t count = profile == GaugeProfile::CourseLR2 ? 1 : 5;
    for (size_t i = 0; i < count; i++) {
      if (currentGauge < thresholds[i]) {
        return delta * multipliers[i];
      }
    }
  }
  return delta;
}

inline float gaugeDeltaForJudgement(GaugeType gaugeType, Judgement judgement,
                                    int totalNotes, double total,
                                    float currentGauge,
                                    GaugeProfile profile =
                                        GaugeProfile::Standard) {
  if (gaugeProfileIsCourse(profile)) {
    const float delta =
        courseGaugeBaseDeltaForJudgement(profile, gaugeType, judgement);
    return applyCourseGaugeGuts(profile, gaugeType, currentGauge, delta);
  }

  float delta =
      gaugeBaseDeltaForJudgement(gaugeType, judgement, profile);
  if (profile == GaugeProfile::Standard5Keys &&
      gaugeType == GaugeType::ExHard && delta < 0.0f) {
    delta *= beatorajaDamageMultiplier(total, totalNotes);
  }
  if (delta > 0.0f) {
    if (gaugeIsSurvival(gaugeType)) {
      delta *= beatorajaHardRecoveryMultiplier(total, totalNotes);
    } else {
      delta *= static_cast<float>(total) /
               static_cast<float>(std::max(1, totalNotes));
    }
  }
  if (gaugeType == GaugeType::Hard &&
      gaugeReducedDamageZoneUpperBound(gaugeType, profile) > 0.0f) {
    delta = applyHardGaugeGuts(currentGauge, delta);
  }
  return delta;
}

inline ClearType clearTypeForGauge(GaugeType gaugeType, float gaugeValue,
                                   bool survivalFailed,
                                   GaugeProfile profile =
                                       GaugeProfile::Standard) {
  if (survivalFailed || gaugeValue <= 0.0f ||
      gaugeValue < gaugeBorderValue(gaugeType, profile)) {
    return ClearType::Failed;
  }
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return ClearType::AssistedEasyClear;
  case GaugeType::Easy:
    return ClearType::EasyClear;
  case GaugeType::Hard:
    return ClearType::HardClear;
  case GaugeType::ExHard:
    return ClearType::ExHardClear;
  case GaugeType::Normal:
  default:
    return ClearType::NormalClear;
  }
}

inline int clearTypeToRank(ClearType clearType) {
  switch (clearType) {
  case ClearType::AssistedEasyClear:
    return kClearTypeAssistedEasyClearRank;
  case ClearType::EasyClear:
    return kClearTypeEasyClearRank;
  case ClearType::NormalClear:
    return kClearTypeNormalClearRank;
  case ClearType::HardClear:
    return kClearTypeHardClearRank;
  case ClearType::ExHardClear:
    return kClearTypeExHardClearRank;
  case ClearType::Failed:
  default:
    return kClearTypeFailedRank;
  }
}

inline const char *clearTypeToLabel(ClearType clearType) {
  switch (clearType) {
  case ClearType::AssistedEasyClear:
    return "ASSISTED EASY CLEAR";
  case ClearType::EasyClear:
    return "EASY CLEAR";
  case ClearType::NormalClear:
    return "NORMAL CLEAR";
  case ClearType::HardClear:
    return "HARD CLEAR";
  case ClearType::ExHardClear:
    return "EX-HARD CLEAR";
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
  if (rank >= kClearTypeAssistedEasyClearRank) {
    return "ASSISTED EASY CLEAR";
  }
  if (rank == kNoClearTypeRank) {
    return "NO PLAY";
  }
  return "FAILED";
}

struct GaugeStateSnapshot {
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  bool gaugeAutoShift = false;
  float currentGauge = 0.0f;
  std::array<float, kGaugeTypeCount> gaugeValues{};
  std::array<bool, kGaugeTypeCount> gaugeSurvivalFailed{};
};

class RhythmState {
public:
  bool isPlaying = false;
  bool isEnding = false;

  size_t passedMeasureCount = 0;
  size_t passedTimelineCount = 0;

  int combo = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  // judge count. default 0
  std::map<Judgement, int> judgeCount;
  std::map<Judgement, JudgementFastSlowCount> judgementFastSlowCount;

  explicit RhythmState(const bms_parser::Chart *Chart, bool addReadyMeasure) {
    gaugeTotalNotes = Chart != nullptr ? Chart->Meta.TotalNotes : 0;
    gaugeKeyMode = Chart != nullptr ? Chart->Meta.KeyMode : 7;
    gaugeTotal =
        Chart == nullptr
            ? 100.0
            : (Chart->Meta.HasTotal
                   ? Chart->Meta.Total
                   : beatorajaDefaultGaugeTotal(gaugeKeyMode,
                                                gaugeTotalNotes));
    resetJudgeCounts();
    configureGauge(GaugeType::Normal, false);
  }

  int getScore() const {
    // PGreat * 2 + Great
    return judgeCount.at(PGreat) * 2 + judgeCount.at(Great);
  }

  std::vector<float> gaugeHistory;
  float currentGauge = 100.0f;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  bool gaugeAutoShift = false;
  bool assistClearMark = false;
  std::array<float, kGaugeTypeCount> gaugeValues{};
  std::array<bool, kGaugeTypeCount> gaugeSurvivalFailed{};
  int fastCount = 0;
  int slowCount = 0;

  void resetJudgeCounts() {
    judgeCount.clear();
    judgementFastSlowCount.clear();
    for (int i = 0; i < JudgementCount; i++) {
      judgeCount[static_cast<Judgement>(i)] = 0;
      judgementFastSlowCount[static_cast<Judgement>(i)] = {};
    }
  }

  void addJudgeCountFrom(const RhythmState &source, Judgement judgement) {
    const auto count = source.judgeCount.find(judgement);
    if (count != source.judgeCount.end()) {
      judgeCount[judgement] += count->second;
    }
    const auto timing = source.judgementFastSlowCount.find(judgement);
    if (timing != source.judgementFastSlowCount.end()) {
      judgementFastSlowCount[judgement].fast += timing->second.fast;
      judgementFastSlowCount[judgement].slow += timing->second.slow;
    }
  }

  void recordFastSlow(const JudgeResult &judgeResult) {
    if (judgeResult.judgement == None || judgeResult.judgement == Kpoor) {
      return;
    }
    if (judgeResult.Diff < 0) {
      fastCount++;
      judgementFastSlowCount[judgeResult.judgement].fast++;
    } else if (judgeResult.Diff > 0) {
      slowCount++;
      judgementFastSlowCount[judgeResult.judgement].slow++;
    }
  }

  void configureGauge(GaugeType selectedGaugeType, bool autoShift,
                      GaugeProfile selectedGaugeProfile =
                          GaugeProfile::Standard) {
    gaugeAutoShift = autoShift;
    gaugeType = selectedGaugeType;
    gaugeProfile = resolveGaugeProfile(selectedGaugeProfile, gaugeKeyMode);
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      gaugeValues[i] = gaugeInitialValue(gaugeTypeAtIndex(i), gaugeProfile);
      gaugeSurvivalFailed[i] = false;
    }
    if (gaugeAutoShift) {
      gaugeType = bestAdmittedGaugeType();
    }
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    gaugeHistory.clear();
  }

  void setStartingGaugePercent(int percent) {
    const float value = static_cast<float>(std::max(0, percent));
    if (gaugeAutoShift) {
      for (int i = 0; i < static_cast<int>(kGaugeTypeCount); ++i) {
        const GaugeType type = gaugeTypeAtIndex(i);
        gaugeValues[i] =
            std::clamp(value, gaugeMinimumValue(type, gaugeProfile),
                       gaugeMaximumValue(type, gaugeProfile));
      }
      gaugeType = bestAdmittedGaugeType();
    } else {
      gaugeValues[gaugeTypeIndex(gaugeType)] =
          std::clamp(value, gaugeMinimumValue(gaugeType, gaugeProfile),
                     gaugeMaximumValue(gaugeType, gaugeProfile));
    }
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
  }

  void restoreGaugeState(const GaugeStateSnapshot &snapshot) {
    gaugeAutoShift = snapshot.gaugeAutoShift;
    gaugeType = snapshot.gaugeType;
    gaugeProfile = snapshot.gaugeProfile;
    gaugeValues = snapshot.gaugeValues;
    gaugeSurvivalFailed = snapshot.gaugeSurvivalFailed;
    const int index = gaugeTypeIndex(gaugeType);
    currentGauge = std::clamp(
        snapshot.currentGauge, gaugeMinimumValue(gaugeType, gaugeProfile),
        gaugeMaximumValue(gaugeType, gaugeProfile));
    if (index >= 0 && index < static_cast<int>(gaugeValues.size())) {
      gaugeValues[index] = currentGauge;
    }
    gaugeHistory.clear();
  }

  [[nodiscard]] GaugeStateSnapshot gaugeSnapshot() const {
    return GaugeStateSnapshot{
        .gaugeType = gaugeType,
        .gaugeProfile = gaugeProfile,
        .gaugeAutoShift = gaugeAutoShift,
        .currentGauge = currentGauge,
        .gaugeValues = gaugeValues,
        .gaugeSurvivalFailed = gaugeSurvivalFailed,
    };
  }

  void setAssistClearMark(bool enabled) { assistClearMark = enabled; }

  void applyGaugeJudgement(Judgement judgement) {
    applyGaugeJudgementRate(judgement, 1.0f);
  }

  void applyGaugeJudgementRate(Judgement judgement, float rate) {
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!gaugeAutoShift && type != gaugeType) {
        continue;
      }
      const bool survival = gaugeIsSurvival(type, gaugeProfile);
      if (survival && gaugeSurvivalFailed[i]) {
        gaugeValues[i] = 0.0f;
        continue;
      }

      const float delta = gaugeDeltaForJudgement(
          type, judgement, gaugeTotalNotes, gaugeTotal, gaugeValues[i],
          gaugeProfile) *
                          rate;
      if (gaugeValues[i] > 0.0f || !survival) {
        gaugeValues[i] = std::clamp(gaugeValues[i] + delta,
                                    gaugeMinimumValue(type, gaugeProfile),
                                    gaugeMaximumValue(type, gaugeProfile));
      }
      if (survival && gaugeValues[i] <= 0.0f) {
        gaugeValues[i] = 0.0f;
        gaugeSurvivalFailed[i] = true;
      }
    }

    if (gaugeAutoShift) {
      gaugeType = bestAdmittedGaugeType();
    }
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    gaugeHistory.push_back(currentGauge);
  }

  void applyGaugeDelta(float delta) {
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!gaugeAutoShift && type != gaugeType) {
        continue;
      }
      const bool survival = gaugeIsSurvival(type, gaugeProfile);
      if (survival && gaugeSurvivalFailed[i]) {
        gaugeValues[i] = 0.0f;
        continue;
      }

      if (gaugeValues[i] > 0.0f || !survival) {
        gaugeValues[i] = std::clamp(gaugeValues[i] + delta,
                                    gaugeMinimumValue(type, gaugeProfile),
                                    gaugeMaximumValue(type, gaugeProfile));
      }
      if (survival && gaugeValues[i] <= 0.0f) {
        gaugeValues[i] = 0.0f;
        gaugeSurvivalFailed[i] = true;
      }
    }

    if (gaugeAutoShift) {
      gaugeType = bestAdmittedGaugeType();
    }
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    gaugeHistory.push_back(currentGauge);
  }

  [[nodiscard]] ClearType getClearType() const {
    const ClearType gaugeClearType = getGaugeClearType();
    if (assistClearMark) {
      return gaugeClearType == ClearType::Failed
                 ? ClearType::Failed
                 : ClearType::AssistedEasyClear;
    }
    return gaugeClearType;
  }

  [[nodiscard]] int getClearTypeRank() const {
    return clearTypeToRank(getClearType());
  }

  [[nodiscard]] const char *getClearTypeLabel() const {
    return clearTypeToLabel(getClearType());
  }

  ~RhythmState() {}

private:
  [[nodiscard]] ClearType getGaugeClearType() const {
    if (!gaugeAutoShift) {
      return clearTypeForGauge(gaugeType, currentGauge,
                               gaugeSurvivalFailed[gaugeTypeIndex(gaugeType)],
                               gaugeProfile);
    }

    ClearType best = ClearType::Failed;
    for (int i = static_cast<int>(kGaugeTypeCount) - 1; i >= 0; i--) {
      const ClearType clearType =
          clearTypeForGauge(gaugeTypeAtIndex(i), gaugeValues[i],
                            gaugeSurvivalFailed[i], gaugeProfile);
      if (clearType != ClearType::Failed) {
        return clearType;
      }
    }
    return best;
  }

  [[nodiscard]] GaugeType bestAdmittedGaugeType() const {
    for (int i = static_cast<int>(kGaugeTypeCount) - 1; i >= 0; i--) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (clearTypeForGauge(type, gaugeValues[i], gaugeSurvivalFailed[i],
                            gaugeProfile) !=
          ClearType::Failed) {
        return type;
      }
    }
    return bestSurvivingGaugeType();
  }

  [[nodiscard]] GaugeType bestSurvivingGaugeType() const {
    for (int i = static_cast<int>(kGaugeTypeCount) - 1; i >= 0; i--) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!gaugeIsSurvival(type, gaugeProfile) || !gaugeSurvivalFailed[i]) {
        return type;
      }
    }
    return GaugeType::AssistedEasy;
  }

  long long firstTiming = 0;
  int gaugeTotalNotes = 0;
  int gaugeKeyMode = 7;
  double gaugeTotal = 100.0;
};
