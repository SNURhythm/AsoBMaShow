#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"
#include "../context.h"

#include <optional>
#include <string>
#include <vector>

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
  std::optional<std::string> attemptId;
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
  bool irOnline = false;
  View **outGraphPlaceholder = nullptr;
  bool showControls = true;
  bool showTimingAnalytics = false;
  bool showResultGraph = true;
  std::string playerName;
  std::string tableName;
  std::string tableLevel;
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::vector<std::string> courseTitles;
  std::string skinName;
  std::string skinAuthor;
  std::optional<float> playLevelOverride;
  std::string chartMd5;
  std::string chartSha256;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<ResultPreviousBestData> previousLampBest;
  std::optional<ResultPacemakerData> pacemaker;
  const ResultPresentationModel *presentation = nullptr;
};
