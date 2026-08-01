#pragma once
#include "GameplayGaugeRules.h"
#include "GameplayGaugeTypes.h"
#include "Judgement.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct JudgementFastSlowCount {
  int fast = 0;
  int slow = 0;

  bool operator==(const JudgementFastSlowCount &) const = default;
};

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
    case GaugeType::Hazard:
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
  case GaugeType::Hazard:
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

inline float gaugeStartingMaximumValue(
    GaugeType selectedGaugeType, GaugeAutoShiftMode autoShift,
    GaugeType autoShiftLowerBound,
    GaugeProfile profile = GaugeProfile::Standard) {
  if (autoShift == GaugeAutoShiftMode::SurvivalToGroove) {
    const float selectedMaximum =
        gaugeMaximumValue(selectedGaugeType, profile);
    return gaugeProfileIsCourse(profile)
               ? selectedMaximum
               : std::max(selectedMaximum,
                          gaugeMaximumValue(GaugeType::Normal, profile));
  }
  if (autoShift != GaugeAutoShiftMode::BestClear &&
      autoShift != GaugeAutoShiftMode::SelectToUnder) {
    return gaugeMaximumValue(selectedGaugeType, profile);
  }

  const int upperIndex =
      autoShift == GaugeAutoShiftMode::BestClear
          ? gaugeTypeIndex(GaugeType::Hazard)
          : gaugeTypeIndex(selectedGaugeType);
  const int lowerIndex =
      std::min(gaugeTypeIndex(autoShiftLowerBound), upperIndex);
  float maximum = 0.0f;
  for (int index = lowerIndex; index <= upperIndex; ++index) {
    maximum = std::max(
        maximum, gaugeMaximumValue(gaugeTypeAtIndex(index), profile));
  }
  return maximum;
}

inline float gaugeMinimumValue(GaugeType gaugeType,
                               GaugeProfile profile = GaugeProfile::Standard) {
  if (gaugeProfileIsCourse(profile)) {
    return 0.0f;
  }
  switch (gaugeType) {
  case GaugeType::Hard:
  case GaugeType::ExHard:
  case GaugeType::Hazard:
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
  return gaugeType == GaugeType::Hard || gaugeType == GaugeType::ExHard ||
         gaugeType == GaugeType::Hazard;
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
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -100.0f, -100.0f, -10.0f},
  };
  constexpr GaugeTable fiveKeysTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.5f, -3.0f, -0.5f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.5f, -4.5f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -3.0f, -6.0f, -2.0f},
      std::array<float, 6>{0.0f, 0.0f, 0.0f, -5.0f, -10.0f, -5.0f},
      std::array<float, 6>{0.0f, 0.0f, 0.0f, -10.0f, -20.0f, -10.0f},
      std::array<float, 6>{0.0f, 0.0f, 0.0f, -100.0f, -100.0f, -100.0f},
  };
  constexpr GaugeTable pmsTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -2.0f, -2.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -3.0f, -3.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -2.0f, -6.0f, -6.0f},
      std::array<float, 6>{0.15f, 0.12f, 0.03f, -5.0f, -10.0f, -10.0f},
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -10.0f, -15.0f, -15.0f},
      std::array<float, 6>{0.15f, 0.06f, 0.0f, -100.0f, -100.0f, -100.0f},
  };
  constexpr GaugeTable keyboardTable = {
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -2.0f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -1.0f, -3.0f, -1.0f},
      std::array<float, 6>{1.0f, 1.0f, 0.5f, -2.0f, -4.0f, -2.0f},
      std::array<float, 6>{0.2f, 0.2f, 0.1f, -4.0f, -8.0f, -4.0f},
      std::array<float, 6>{0.2f, 0.1f, 0.0f, -6.0f, -12.0f, -6.0f},
      std::array<float, 6>{0.2f, 0.1f, 0.0f, -100.0f, -100.0f, -100.0f},
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
  case GaugeType::Hazard:
    return 2;
  case GaugeType::AssistedEasy:
  case GaugeType::Easy:
  case GaugeType::Normal:
  default:
    return 0;
  }
}

inline GaugeType gaugeClearTypeForProfile(GaugeType gaugeType,
                                          GaugeProfile profile) {
  if (gaugeProfileIsCourse(profile)) {
    switch (courseGaugeClassIndexForType(gaugeType)) {
    case 1:
      return GaugeType::Hard;
    case 2:
      return GaugeType::ExHard;
    default:
      return GaugeType::Normal;
    }
  }
  return gaugeType;
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
  while (note > totalNotes && note > 1) {
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
    if (gaugeType == GaugeType::Hard || gaugeType == GaugeType::ExHard) {
      delta *= beatorajaHardRecoveryMultiplier(total, totalNotes);
    } else if (gaugeType != GaugeType::Hazard) {
      delta *= static_cast<float>(total) /
               static_cast<float>(std::max(1, totalNotes));
    }
  }
  if (gaugeType == GaugeType::Hard &&
      gaugeReducedDamageZoneUpperBound(GameplayRuleset::Beatoraja, gaugeType,
                                       profile) > 0.0f) {
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
  switch (gaugeClearTypeForProfile(gaugeType, profile)) {
  case GaugeType::AssistedEasy:
    return ClearType::LightAssistedEasyClear;
  case GaugeType::Easy:
    return ClearType::EasyClear;
  case GaugeType::Hard:
    return ClearType::HardClear;
  case GaugeType::ExHard:
    return ClearType::ExHardClear;
  case GaugeType::Hazard:
    return ClearType::FullCombo;
  case GaugeType::Normal:
  default:
    return ClearType::NormalClear;
  }
}

struct GaugeStateSnapshot {
  GaugeType gaugeType = GaugeType::Normal;
  GaugeType selectedGaugeType = GaugeType::Normal;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  float currentGauge = 0.0f;
  std::array<float, kGaugeTypeCount> gaugeValues{};
  std::array<bool, kGaugeTypeCount> gaugeSurvivalFailed{};
};

struct GameplayScoreConfig {
  GameplayGaugeRules gaugeRules;
  int keyMode = 7;
};

using GaugeHistoryCollection =
    std::array<std::vector<float>, kGaugeTypeCount>;

class GameplayScoreState {
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

  explicit GameplayScoreState(GameplayScoreConfig config)
      : gaugeKeyMode(config.keyMode),
        gaugeRules_(std::move(config.gaugeRules)) {
    resetJudgeCounts();
    configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None,
                   gaugeRules_.resolvedProfile);
  }

  void setGaugeRules(GameplayGaugeRules rules, int keyMode) {
    gaugeRules_ = std::move(rules);
    gaugeKeyMode = keyMode;
  }

  [[nodiscard]] const GameplayGaugeRules &gaugeRules() const noexcept {
    return gaugeRules_;
  }

  void configureBoundedGaugeHistory(std::size_t capacity) {
    gaugeHistory.reserve(capacity);
    for (auto &history : gaugeHistories) {
      history.reserve(capacity);
    }
    gaugeHistoryCapacity_ = capacity;
    boundedGaugeHistory_ = true;
  }

  [[nodiscard]] bool gaugeHistoryOverflowed() const {
    return gaugeHistoryOverflowed_;
  }

  void commitJudge(const JudgeResult &judgeResult) {
    ++judgeCount[judgeResult.judgement];
    if (judgeResult.isComboBreak()) {
      combo = 0;
      ++comboBreak;
    } else if (judgeResult.judgement != Kpoor) {
      ++combo;
      maxCombo = std::max(maxCombo, combo);
    }
    recordFastSlow(judgeResult);
    applyGaugeJudgement(judgeResult.judgement);
  }

  int getScore() const {
    // PGreat * 2 + Great
    return judgeCount.at(PGreat) * 2 + judgeCount.at(Great);
  }

  std::vector<float> gaugeHistory;
  GaugeHistoryCollection gaugeHistories;
  float currentGauge = 100.0f;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeType selectedGaugeType = GaugeType::Normal;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  bool assistClearMark = false;
  std::array<float, kGaugeTypeCount> gaugeValues{};
  std::array<bool, kGaugeTypeCount> gaugeSurvivalFailed{};
  int fastCount = 0;
  int slowCount = 0;

  [[nodiscard]] std::vector<float> &gaugeHistoryFor(GaugeType type) {
    return gaugeHistories[gaugeTypeIndex(type)];
  }

  [[nodiscard]] const std::vector<float> &
  gaugeHistoryFor(GaugeType type) const {
    return gaugeHistories[gaugeTypeIndex(type)];
  }

  void resetJudgeCounts() {
    judgeCount.clear();
    judgementFastSlowCount.clear();
    for (int i = 0; i < JudgementCount; i++) {
      judgeCount[static_cast<Judgement>(i)] = 0;
      judgementFastSlowCount[static_cast<Judgement>(i)] = {};
    }
  }

  void addJudgeCountFrom(const GameplayScoreState &source,
                         Judgement judgement) {
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

  void configureGauge(GaugeType newSelectedGaugeType,
                      GaugeAutoShiftMode autoShift,
                      GaugeProfile selectedGaugeProfile =
                          GaugeProfile::Standard,
                      GaugeType autoShiftLowerBound =
                          GaugeType::AssistedEasy) {
    gaugeAutoShift = autoShift;
    selectedGaugeType = newSelectedGaugeType;
    gaugeAutoShiftLowerBound = autoShiftLowerBound;
    gaugeType = selectedGaugeType;
    (void)selectedGaugeProfile;
    gaugeProfile = gaugeRules_.resolvedProfile;
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      gaugeValues[i] = gaugeDefinition(gaugeTypeAtIndex(i)).initial;
      gaugeSurvivalFailed[i] = false;
    }
    if (gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
        gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder) {
      gaugeType = bestAdmittedGaugeType();
    }
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    gaugeHistory.clear();
    clearGaugeHistories();
  }

  void setStartingGaugePercent(int percent) {
    const float value = static_cast<float>(std::max(0, percent));
    const auto setStartingValue = [&](GaugeType type) {
      const int index = gaugeTypeIndex(type);
      gaugeValues[index] =
          std::clamp(value, 0.0f, gaugeDefinition(type).maximum);
      gaugeSurvivalFailed[index] =
          gaugeDefinition(type).survival &&
          survivalGaugeDies(type, gaugeValues[index]);
    };
    if (gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
        gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder) {
      for (int i = 0; i < static_cast<int>(kGaugeTypeCount); ++i) {
        setStartingValue(gaugeTypeAtIndex(i));
      }
    } else {
      setStartingValue(gaugeType);
      if (gaugeAutoShift == GaugeAutoShiftMode::SurvivalToGroove &&
          !gaugeProfileIsCourse(gaugeProfile)) {
        setStartingValue(GaugeType::Normal);
      }
    }
    updateAutoShiftGaugeType();
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
  }

  void restoreGaugeState(const GaugeStateSnapshot &snapshot) {
    gaugeAutoShift = snapshot.gaugeAutoShift;
    gaugeType = snapshot.gaugeType;
    selectedGaugeType = snapshot.selectedGaugeType;
    gaugeAutoShiftLowerBound = snapshot.gaugeAutoShiftLowerBound;
    gaugeProfile = snapshot.gaugeProfile;
    gaugeValues = snapshot.gaugeValues;
    gaugeSurvivalFailed = snapshot.gaugeSurvivalFailed;
    const int index = gaugeTypeIndex(gaugeType);
    const auto &definition = gaugeDefinition(gaugeType);
    currentGauge = std::clamp(snapshot.currentGauge, definition.minimum,
                              definition.maximum);
    if (index >= 0 && index < static_cast<int>(gaugeValues.size())) {
      gaugeValues[index] = currentGauge;
    }
    gaugeHistory.clear();
    clearGaugeHistories();
  }

  [[nodiscard]] GaugeStateSnapshot gaugeSnapshot() const {
    return GaugeStateSnapshot{
        .gaugeType = gaugeType,
        .selectedGaugeType = selectedGaugeType,
        .gaugeAutoShiftLowerBound = gaugeAutoShiftLowerBound,
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
      if (!tracksAllGaugeTypes() && type != gaugeType) {
        continue;
      }
      const auto &definition = gaugeDefinition(type);
      const bool survival = definition.survival;
      if (survival && gaugeSurvivalFailed[i]) {
        gaugeValues[i] = 0.0f;
        continue;
      }

      const float delta =
          gaugeRules_.delta(type, judgement, gaugeValues[i], rate);
      if (gaugeValues[i] > 0.0f || !survival) {
        gaugeValues[i] = std::clamp(gaugeValues[i] + delta,
                                    definition.minimum, definition.maximum);
      }
      if (survival && survivalGaugeDies(type, gaugeValues[i])) {
        gaugeValues[i] = 0.0f;
        gaugeSurvivalFailed[i] = true;
      }
    }

    updateAutoShiftGaugeType();
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    recordGaugeHistory(currentGauge);
  }

  void applyGaugeDelta(float delta) {
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!tracksAllGaugeTypes() && type != gaugeType) {
        continue;
      }
      const auto &definition = gaugeDefinition(type);
      const bool survival = definition.survival;
      if (survival && gaugeSurvivalFailed[i]) {
        gaugeValues[i] = 0.0f;
        continue;
      }

      if (gaugeValues[i] > 0.0f || !survival) {
        gaugeValues[i] = std::clamp(gaugeValues[i] + delta,
                                    definition.minimum, definition.maximum);
      }
      if (survival && survivalGaugeDies(type, gaugeValues[i])) {
        gaugeValues[i] = 0.0f;
        gaugeSurvivalFailed[i] = true;
      }
    }

    updateAutoShiftGaugeType();
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    recordGaugeHistory(currentGauge);
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

  [[nodiscard]] bool activeGaugeFailed() const {
    const int index = gaugeTypeIndex(gaugeType);
    if (gaugeAutoShift == GaugeAutoShiftMode::Continue) {
      return false;
    }
    return gaugeDefinition(gaugeType).survival &&
           gaugeSurvivalFailed[index];
  }

  ~GameplayScoreState() {}

private:
  [[nodiscard]] const CompiledGaugeDefinition &
  gaugeDefinition(GaugeType type) const noexcept {
    return gaugeRules_.gauges[gaugeTypeIndex(type)];
  }

  [[nodiscard]] bool survivalGaugeDies(GaugeType type,
                                       float value) const noexcept {
    const auto &definition = gaugeDefinition(type);
    if (!definition.survival) {
      return false;
    }
    return gaugeRules_.ruleset == GameplayRuleset::LR2
               ? value < definition.deathBelow
               : value <= definition.deathBelow;
  }

  [[nodiscard]] ClearType clearTypeForCompiledGauge(
      GaugeType type, float value, bool survivalFailed) const noexcept {
    const auto &definition = gaugeDefinition(type);
    if (survivalFailed || value <= 0.0F ||
        value < definition.clearBorder) {
      return ClearType::Failed;
    }
    switch (gaugeClearTypeForProfile(type, gaugeProfile)) {
    case GaugeType::AssistedEasy:
      return ClearType::LightAssistedEasyClear;
    case GaugeType::Easy:
      return ClearType::EasyClear;
    case GaugeType::Hard:
      return ClearType::HardClear;
    case GaugeType::ExHard:
      return ClearType::ExHardClear;
    case GaugeType::Hazard:
      return ClearType::FullCombo;
    case GaugeType::Normal:
    default:
      return ClearType::NormalClear;
    }
  }

  void recordGaugeHistory(float value) {
    if (!boundedGaugeHistory_ ||
        gaugeHistory.size() < gaugeHistoryCapacity_) {
      gaugeHistory.push_back(value);
      if (tracksAllGaugeTypes()) {
        for (int index = 0; index < static_cast<int>(kGaugeTypeCount);
             ++index) {
          gaugeHistories[index].push_back(gaugeValues[index]);
        }
      } else {
        gaugeHistories[gaugeTypeIndex(gaugeType)].push_back(value);
      }
      return;
    }
    gaugeHistoryOverflowed_ = true;
  }

  void clearGaugeHistories() {
    for (auto &history : gaugeHistories) {
      history.clear();
    }
  }

  [[nodiscard]] ClearType getGaugeClearType() const {
    if (gaugeAutoShift != GaugeAutoShiftMode::BestClear &&
        gaugeAutoShift != GaugeAutoShiftMode::SelectToUnder) {
      return clearTypeForCompiledGauge(
          gaugeType, currentGauge,
          gaugeSurvivalFailed[gaugeTypeIndex(gaugeType)]);
    }

    ClearType best = ClearType::Failed;
    for (int i = autoShiftUpperIndex(); i >= autoShiftLowerIndex(); i--) {
      const ClearType clearType = clearTypeForCompiledGauge(
          gaugeTypeAtIndex(i), gaugeValues[i], gaugeSurvivalFailed[i]);
      if (clearType != ClearType::Failed) {
        return clearType;
      }
    }
    return best;
  }

  [[nodiscard]] GaugeType bestAdmittedGaugeType() const {
    for (int i = autoShiftUpperIndex(); i >= autoShiftLowerIndex(); i--) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (clearTypeForCompiledGauge(type, gaugeValues[i],
                                    gaugeSurvivalFailed[i]) !=
          ClearType::Failed) {
        return type;
      }
    }
    return bestSurvivingGaugeType();
  }

  [[nodiscard]] GaugeType bestSurvivingGaugeType() const {
    for (int i = autoShiftUpperIndex(); i >= autoShiftLowerIndex(); i--) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!gaugeDefinition(type).survival || !gaugeSurvivalFailed[i]) {
        return type;
      }
    }
    return gaugeTypeAtIndex(autoShiftLowerIndex());
  }

  [[nodiscard]] int autoShiftLowerIndex() const {
    if (gaugeProfileIsCourse(gaugeProfile)) {
      return gaugeTypeIndex(GaugeType::Normal);
    }
    return std::min(gaugeTypeIndex(gaugeAutoShiftLowerBound),
                    autoShiftUpperIndex());
  }

  [[nodiscard]] int autoShiftUpperIndex() const {
    if (gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder) {
      return gaugeTypeIndex(selectedGaugeType);
    }
    return gaugeProfileIsCourse(gaugeProfile)
               ? gaugeTypeIndex(GaugeType::ExHard)
               : gaugeTypeIndex(GaugeType::Hazard);
  }

  [[nodiscard]] bool tracksAllGaugeTypes() const {
    return gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
           gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder ||
           gaugeAutoShift == GaugeAutoShiftMode::SurvivalToGroove;
  }

  void updateAutoShiftGaugeType() {
    if (gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
        gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder) {
      gaugeType = bestAdmittedGaugeType();
    } else if (gaugeAutoShift == GaugeAutoShiftMode::SurvivalToGroove &&
               !gaugeProfileIsCourse(gaugeProfile) &&
               gaugeDefinition(gaugeType).survival &&
               gaugeSurvivalFailed[gaugeTypeIndex(gaugeType)]) {
      gaugeType = GaugeType::Normal;
    }
  }

  long long firstTiming = 0;
  int gaugeKeyMode = 7;
  GameplayGaugeRules gaugeRules_;
  bool boundedGaugeHistory_ = false;
  std::size_t gaugeHistoryCapacity_ = 0;
  bool gaugeHistoryOverflowed_ = false;
};
