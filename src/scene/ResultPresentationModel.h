#pragma once

#include "../ir/IrRemoteScoreModels.h"
#include "../practice/PracticeResultModel.h"
#include "../rendering/Color.h"
#include "../skin/SkinTypes.h"
#include "ResultGaugeHistory.h"
#include "play/RhythmState.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ResultJudgementRow {
  std::string label;
  Color color;
  int total = 0;
  std::optional<int> early;
  std::optional<int> late;
};

struct ResultComparisonValue {
  std::string label;
  std::string value;
  std::string detail;
  Color accent;
};

struct ResultComparisonCard {
  std::string title;
  std::optional<ResultComparisonValue> target;
  ResultComparisonValue current;
  std::optional<std::string> delta;
};

struct ResultInfoTile {
  std::string label;
  std::string value;
  std::optional<std::string> detail;
  Color accent;
};

struct ResultGradeCard {
  std::string grade;
  std::string rate;
  Color accent;
};

struct ResultLocalPresentationOptions {
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<ResultPacemakerData> pacemaker;
  std::optional<practice::ResultModel> timingAnalytics;
};

struct ResultPresentationModel {
  std::string title;
  std::optional<std::string> artist;
  std::optional<std::string> difficulty;
  std::optional<std::string> playtype;
  std::optional<std::int64_t> achievedAtUnixMillis;
  std::optional<std::string> service;
  std::optional<std::string> client;
  std::optional<std::string> inputDevice;
  std::optional<std::string> random;
  std::optional<std::string> gaugeType;
  std::optional<int> score;
  std::optional<int> maxScore;
  std::optional<int> lampRank;
  std::optional<float> finalGauge;
  std::optional<int> maxCombo;
  std::optional<int> comboBreak;
  std::optional<int> badPoints;
  std::optional<ResultComparisonCard> scoreComparison;
  std::optional<ResultComparisonCard> lampComparison;
  std::optional<ResultComparisonCard> comboComparison;
  std::vector<ResultInfoTile> infoTiles;
  std::vector<ResultJudgementRow> judgements;
  std::optional<int> fast;
  std::optional<int> slow;
  std::vector<ResultGaugeSeries> gaugeSeries;
  std::optional<practice::ResultModel> timingAnalytics;
  bool readOnlyIrUploaded = false;
};

[[nodiscard]] bool hasGradeCard(const ResultPresentationModel &model) noexcept;
[[nodiscard]] bool
hasJudgementCard(const ResultPresentationModel &model) noexcept;
[[nodiscard]] bool
hasComboBreakCard(const ResultPresentationModel &model) noexcept;
[[nodiscard]] bool hasGaugeCard(const ResultPresentationModel &model) noexcept;

[[nodiscard]] std::optional<ResultGradeCard>
gradeCard(const ResultPresentationModel &model);
[[nodiscard]] std::vector<ResultJudgementRow>
timingRows(const ResultPresentationModel &model);

[[nodiscard]] ResultPresentationModel
makeLocalResultPresentation(const bms_parser::ChartMeta &meta,
                            const RhythmState &state,
                            ResultLocalPresentationOptions options);
[[nodiscard]] ResultPresentationModel
makeRemoteResultPresentation(const ir::IrRemoteScore &score);
