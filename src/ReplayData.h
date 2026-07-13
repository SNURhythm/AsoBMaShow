#pragma once

#include "AssistOptionUtils.h"
#include "CourseIdentity.h"
#include "ScoreProvenance.h"
#include "bms_parser.hpp"
#include "scene/play/Judge.h"
#include "scene/play/RhythmState.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class ReplayEventAction {
  Press = 0,
  Release = 1,
  Miss = 2,
  Mine = 3,
  Gauge = 4,
};

enum class ReplayTouchAction {
  Down = 0,
  Move = 1,
  Up = 2,
  Cancel = 3,
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

struct ReplayTouchSample {
  ReplayTouchAction action = ReplayTouchAction::Move;
  long long fingerId = 0;
  long long songTimeMicros = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct ReplayLaneCoverEvent {
  long long songTimeMicros = 0;
  int noteStartPositionPercent = 0;
  bool resetVisibleTimeReference = false;
};

struct ReplayData {
  int id = 0;
  bool autoPlay = false;
  bms_parser::ChartMeta chartMeta;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  std::vector<ReplayEvent> events;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
};

struct CourseReplayStageData {
  ReplayData replay;
  long long restMicrosAfterStage = 0;
};

namespace course_replay {

inline std::optional<CourseReplayStageData>
prepareStageForSave(const CourseReplayStageData &recorded,
                    const bms_parser::ChartMeta &expectedMeta) {
  const course_identity::ChartIdentity recordedIdentity{
      .sha256 = recorded.replay.chartMeta.SHA256,
      .md5 = recorded.replay.chartMeta.MD5};
  const course_identity::ChartIdentity expectedIdentity{
      .sha256 = expectedMeta.SHA256, .md5 = expectedMeta.MD5};
  if (recorded.replay.events.empty() ||
      !course_identity::sameChart(recordedIdentity, expectedIdentity)) {
    return std::nullopt;
  }

  CourseReplayStageData prepared = recorded;
  if (prepared.replay.chartMeta.BmsPath.empty()) {
    prepared.replay.chartMeta.BmsPath = expectedMeta.BmsPath;
  }
  if (prepared.replay.chartMeta.Title.empty()) {
    prepared.replay.chartMeta.Title = expectedMeta.Title;
  }
  if (prepared.replay.chartMeta.Artist.empty()) {
    prepared.replay.chartMeta.Artist = expectedMeta.Artist;
  }
  return prepared;
}

inline std::optional<std::vector<CourseReplayStageData>>
prepareCompletedPrefixForSave(
    std::span<const CourseReplayStageData> recordedStages,
    std::span<const bms_parser::ChartMeta> expectedMetas,
    std::size_t completedCharts) {
  if (completedCharts == 0 || completedCharts > recordedStages.size() ||
      completedCharts > expectedMetas.size()) {
    return std::nullopt;
  }

  std::vector<CourseReplayStageData> prepared;
  prepared.reserve(completedCharts);
  for (std::size_t index = 0; index < completedCharts; ++index) {
    auto stage = prepareStageForSave(recordedStages[index],
                                     expectedMetas[index]);
    if (!stage.has_value()) {
      return std::nullopt;
    }
    prepared.push_back(std::move(*stage));
  }
  return prepared;
}

} // namespace course_replay

struct CourseReplayData {
  int id = 0;
  int courseId = 0;
  std::string courseKey;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::string requestedPlayOption = "NORMAL";
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int longNoteMode = 0;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  int completedCharts = 0;
  int totalCharts = 0;
  std::string createdAt;
  std::vector<CourseReplayStageData> stages;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
};
