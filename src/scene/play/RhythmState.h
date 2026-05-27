#pragma once
#include "../../bms_parser.hpp"
#include "Judge.h"
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
  JudgeResult latestJudgeResult = JudgeResult(None, 0);
  // judge count. default 0
  std::map<Judgement, int> judgeCount;

  explicit RhythmState(const bms_parser::Chart *Chart, bool addReadyMeasure) {
    for (int i = 0; i < JudgementCount; i++) {
      judgeCount[static_cast<Judgement>(i)] = 0;
    }
  }

  int getScore() const {
    // PGreat * 2 + Great
    return judgeCount.at(PGreat) * 2 + judgeCount.at(Great);
  }

  std::vector<float> gaugeHistory;
  float currentGauge = 100.0f;
  GaugeType gaugeType = GaugeType::Normal;
  int fastCount = 0;
  int slowCount = 0;

  [[nodiscard]] ClearType getClearType() const {
    switch (gaugeType) {
    case GaugeType::AssistedEasy:
      return currentGauge >= 60.0f ? ClearType::AssistedEasyClear
                                   : ClearType::Failed;
    case GaugeType::Easy:
      return currentGauge >= 80.0f ? ClearType::EasyClear
                                   : ClearType::Failed;
    case GaugeType::Hard:
      return currentGauge > 0.0f ? ClearType::HardClear : ClearType::Failed;
    case GaugeType::ExHard:
      return currentGauge > 0.0f ? ClearType::ExHardClear
                                 : ClearType::Failed;
    case GaugeType::Normal:
    default:
      return currentGauge >= 80.0f ? ClearType::NormalClear
                                   : ClearType::Failed;
    }
  }

  [[nodiscard]] int getClearTypeRank() const {
    return clearTypeToRank(getClearType());
  }

  [[nodiscard]] const char *getClearTypeLabel() const {
    return clearTypeToLabel(getClearType());
  }

  ~RhythmState() {}

private:
  long long firstTiming = 0;
};
