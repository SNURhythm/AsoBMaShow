#pragma once
#include "../../bms_parser.hpp"
#include "Judge.h"
class RhythmState {
public:
  bool isPlaying = false;

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
  int fastCount = 0;
  int slowCount = 0;

  ~RhythmState() {}

private:
  long long firstTiming = 0;
};