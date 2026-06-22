#pragma once
#include "../../bms_parser.hpp"
#include "Judge.h"
#include <algorithm>
#include <array>
#include <string>

enum class GaugeType { AssistedEasy, Easy, Normal, Hard, ExHard };

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
inline constexpr size_t kGaugeTypeCount = 5;

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

inline float gaugeInitialValue(GaugeType gaugeType) {
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

inline float gaugeMinimumValue(GaugeType gaugeType) {
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

inline float gaugeBorderValue(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return 60.0f;
  case GaugeType::Easy:
  case GaugeType::Normal:
    return 80.0f;
  case GaugeType::Hard:
  case GaugeType::ExHard:
  default:
    return 0.0f;
  }
}

inline bool gaugeIsSurvival(GaugeType gaugeType) {
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

inline float gaugeBaseDeltaForJudgement(GaugeType gaugeType,
                                        Judgement judgement) {
  const int index = gaugeJudgementIndex(judgement);
  if (index < 0) {
    return 0.0f;
  }
  switch (gaugeType) {
  case GaugeType::AssistedEasy: {
    constexpr std::array<float, 6> values = {1.0f, 1.0f, 0.5f,
                                             -1.5f, -3.0f, -0.5f};
    return values[index];
  }
  case GaugeType::Easy: {
    constexpr std::array<float, 6> values = {1.0f, 1.0f, 0.5f,
                                             -1.5f, -4.5f, -1.0f};
    return values[index];
  }
  case GaugeType::Hard: {
    constexpr std::array<float, 6> values = {0.15f, 0.12f, 0.03f,
                                             -5.0f, -10.0f, -5.0f};
    return values[index];
  }
  case GaugeType::ExHard: {
    constexpr std::array<float, 6> values = {0.15f, 0.06f, 0.0f,
                                             -8.0f, -16.0f, -8.0f};
    return values[index];
  }
  case GaugeType::Normal:
  default: {
    constexpr std::array<float, 6> values = {1.0f, 1.0f, 0.5f,
                                             -3.0f, -6.0f, -2.0f};
    return values[index];
  }
  }
}

inline float beatorajaHardRecoveryMultiplier(double total, int totalNotes) {
  const float noteCount = static_cast<float>(std::max(1, totalNotes));
  const float pg = std::max(
      std::min(0.15f, static_cast<float>((2.0 * total - 320.0) / noteCount)),
      0.0f);
  return pg / 0.15f;
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

inline float gaugeDeltaForJudgement(GaugeType gaugeType, Judgement judgement,
                                    int totalNotes, double total,
                                    float currentGauge) {
  float delta = gaugeBaseDeltaForJudgement(gaugeType, judgement);
  if (delta > 0.0f) {
    if (gaugeIsSurvival(gaugeType)) {
      delta *= beatorajaHardRecoveryMultiplier(total, totalNotes);
    } else {
      delta *= static_cast<float>(total) /
               static_cast<float>(std::max(1, totalNotes));
    }
  }
  if (gaugeType == GaugeType::Hard) {
    delta = applyHardGaugeGuts(currentGauge, delta);
  }
  return delta;
}

inline ClearType clearTypeForGauge(GaugeType gaugeType, float gaugeValue,
                                   bool survivalFailed) {
  if (survivalFailed || gaugeValue <= 0.0f ||
      gaugeValue < gaugeBorderValue(gaugeType)) {
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
    gaugeTotal = Chart != nullptr ? Chart->Meta.Total : 100.0;
    for (int i = 0; i < JudgementCount; i++) {
      judgeCount[static_cast<Judgement>(i)] = 0;
      judgementFastSlowCount[static_cast<Judgement>(i)] = {};
    }
    configureGauge(GaugeType::Normal, false);
  }

  int getScore() const {
    // PGreat * 2 + Great
    return judgeCount.at(PGreat) * 2 + judgeCount.at(Great);
  }

  std::vector<float> gaugeHistory;
  float currentGauge = 100.0f;
  GaugeType gaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  bool assistClearMark = false;
  std::array<float, kGaugeTypeCount> gaugeValues{};
  std::array<bool, kGaugeTypeCount> gaugeSurvivalFailed{};
  int fastCount = 0;
  int slowCount = 0;

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

  void configureGauge(GaugeType selectedGaugeType, bool autoShift) {
    gaugeAutoShift = autoShift;
    gaugeType = selectedGaugeType;
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      gaugeValues[i] = gaugeInitialValue(gaugeTypeAtIndex(i));
      gaugeSurvivalFailed[i] = false;
    }
    if (gaugeAutoShift) {
      gaugeType = bestAdmittedGaugeType();
    }
    currentGauge = gaugeValues[gaugeTypeIndex(gaugeType)];
    gaugeHistory.clear();
  }

  void setAssistClearMark(bool enabled) { assistClearMark = enabled; }

  void applyGaugeJudgement(Judgement judgement) {
    for (int i = 0; i < static_cast<int>(kGaugeTypeCount); i++) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!gaugeAutoShift && type != gaugeType) {
        continue;
      }
      if (gaugeIsSurvival(type) && gaugeSurvivalFailed[i]) {
        gaugeValues[i] = 0.0f;
        continue;
      }

      const float delta = gaugeDeltaForJudgement(
          type, judgement, gaugeTotalNotes, gaugeTotal, gaugeValues[i]);
      if (gaugeValues[i] > 0.0f) {
        gaugeValues[i] =
            std::clamp(gaugeValues[i] + delta, gaugeMinimumValue(type), 100.0f);
      }
      if (gaugeIsSurvival(type) && gaugeValues[i] <= 0.0f) {
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
      if (gaugeIsSurvival(type) && gaugeSurvivalFailed[i]) {
        gaugeValues[i] = 0.0f;
        continue;
      }

      if (gaugeValues[i] > 0.0f) {
        gaugeValues[i] =
            std::clamp(gaugeValues[i] + delta, gaugeMinimumValue(type), 100.0f);
      }
      if (gaugeIsSurvival(type) && gaugeValues[i] <= 0.0f) {
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
                               gaugeSurvivalFailed[gaugeTypeIndex(gaugeType)]);
    }

    ClearType best = ClearType::Failed;
    for (int i = static_cast<int>(kGaugeTypeCount) - 1; i >= 0; i--) {
      const ClearType clearType =
          clearTypeForGauge(gaugeTypeAtIndex(i), gaugeValues[i],
                            gaugeSurvivalFailed[i]);
      if (clearType != ClearType::Failed) {
        return clearType;
      }
    }
    return best;
  }

  [[nodiscard]] GaugeType bestAdmittedGaugeType() const {
    for (int i = static_cast<int>(kGaugeTypeCount) - 1; i >= 0; i--) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (clearTypeForGauge(type, gaugeValues[i], gaugeSurvivalFailed[i]) !=
          ClearType::Failed) {
        return type;
      }
    }
    return bestSurvivingGaugeType();
  }

  [[nodiscard]] GaugeType bestSurvivingGaugeType() const {
    for (int i = static_cast<int>(kGaugeTypeCount) - 1; i >= 0; i--) {
      const GaugeType type = gaugeTypeAtIndex(i);
      if (!gaugeIsSurvival(type) || !gaugeSurvivalFailed[i]) {
        return type;
      }
    }
    return GaugeType::AssistedEasy;
  }

  long long firstTiming = 0;
  int gaugeTotalNotes = 0;
  double gaugeTotal = 100.0;
};
