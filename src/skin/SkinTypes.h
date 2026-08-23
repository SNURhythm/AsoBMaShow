#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/SkinGameplayGraphState.h"
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
  std::optional<int> badPoints;
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
  std::string courseTitle;
  std::vector<std::string> courseTitles;
  std::string skinName;
  std::string skinAuthor;
  std::optional<float> playLevelOverride;
  std::optional<int> keyModeOverride;
  std::string chartMd5;
  std::string chartSha256;
  bool autoPlayResult = false;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<ResultPreviousBestData> previousLampBest;
  std::optional<ResultPacemakerData> pacemaker;
  const ResultPresentationModel *presentation = nullptr;
  // Captured at the gameplay-to-result boundary. The shared immutable graph
  // snapshots keep every graph-capable Beatoraja result object on the same
  // authoritative data that gameplay rendered on its final frame.
  SkinGameplayGraphState gameplayGraph;
};
