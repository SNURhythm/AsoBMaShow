#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"
#include "../context.h"

#include <optional>
#include <string>

class View;

struct ResultPreviousBestData {
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
};

struct ResultSkinData {
  const RhythmState *state;
  const bms_parser::ChartMeta *meta;
  ApplicationContext *context;
  View **outGraphPlaceholder = nullptr;
  bool showControls = true;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<ResultPreviousBestData> previousBest;
};
