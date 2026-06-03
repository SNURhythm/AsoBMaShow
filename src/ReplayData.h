#pragma once

#include "bms_parser.hpp"
#include "scene/play/Judge.h"
#include "scene/play/RhythmState.h"

#include <optional>
#include <string>
#include <vector>

enum class ReplayEventAction {
  Press = 0,
  Release = 1,
  Miss = 2,
};

struct ReplayEvent {
  ReplayEventAction action = ReplayEventAction::Press;
  int lane = -1;
  long long noteTimeMicros = -1;
  long long songTimeMicros = 0;
  long long judgeTimeMicros = 0;
  Judgement judgement = None;
  long long diffMicros = 0;
  float gauge = 0.0f;
  GaugeType gaugeType = GaugeType::Normal;
  int combo = 0;
  int score = 0;
};

struct ReplayData {
  int id = 0;
  bms_parser::ChartMeta chartMeta;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  GaugeType initialGaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  int finalScore = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  std::vector<ReplayEvent> events;
};
