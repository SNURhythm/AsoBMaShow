#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"
#include "../context.h"

#include <optional>
#include <string>

class View;
struct ResultPresentationModel;

struct ResultPreviousBestData {
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
};

struct ResultPacemakerData {
  std::string label;
  int targetScore = 0;
  int delta = 0;
  bool usesReplayProgression = false;
};

struct ResultSkinData {
  const RhythmState *state;
  const bms_parser::ChartMeta *meta;
  ApplicationContext *context;
  View **outGraphPlaceholder = nullptr;
  bool showControls = true;
  bool showTimingAnalytics = false;
  bool showResultGraph = true;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<ResultPacemakerData> pacemaker;
  const ResultPresentationModel *presentation = nullptr;
};
