#pragma once

#include "../../CoursePlaySession.h"
#include "../../ReplayData.h"
#include "Pacemaker.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Scene;

struct StartOptions {
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  bool autoPlay = false;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  bool gaugeAutoShift = false;
  std::shared_ptr<ReplayData> replayData = nullptr;
  std::shared_ptr<ReplayData> gbattleRecordData = nullptr;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  int longNoteMode = 0;
  std::string assistOption = assist_options::kOff;
  std::string pacemakerTarget = pacemaker::kTargetBest;
  std::shared_ptr<CoursePlaySession> courseSession = nullptr;
  CourseConstraintRules courseConstraints;
  bool ownsChart = false;
  bool practiceMode = false;
  unsigned long long practiceLeadInMicros = 0;
  Scene *returnScene = nullptr;
  std::optional<bool> touchVisualizationEnabled;
  std::optional<bool> replayGhostRenderingEnabled;
  std::function<void(const ReplayData &)> practiceGhostCallback;
  std::vector<InputDeviceCategory> inputDeviceCategories;
  std::optional<RulesetDescriptor> rulesetDescriptor;
};

[[nodiscard]] inline ScoreProvenance captureScoreProvenanceAtPlayStart(
    const StartOptions &options, const bms_parser::ChartMeta &chartMeta,
    const std::map<Judgement, std::pair<long long, long long>>
        &effectiveJudgeWindows) {
  const int chartLongNoteMode =
      normalizeChartLongNoteModeValue(chartMeta.LnMode);
  ScoreProvenanceBuildInput input;
  input.chartMeta = chartMeta;
  input.longNoteMode =
      chartLongNoteMode > 0
          ? chartLongNoteMode
          : normalizeChartLongNoteModeValue(options.longNoteMode);
  input.judgeRankSource =
      options.courseConstraints.judgement == CourseJudgementConstraint::None
          ? JudgeRankSource::Chart
          : JudgeRankSource::CourseConstraint;
  input.sourceJudgeRank = chartMeta.Rank;
  input.effectiveJudgeWindows = effectiveJudgeWindows;
  input.gaugeType = options.gaugeType;
  input.gaugeProfile = options.gaugeProfile;
  input.gaugeAutoShift = options.gaugeAutoShift;
  input.player1 = {.option = options.playOption.value_or("NORMAL"),
                   .seed = options.playOptionSeed};
  input.player2 = {.option = options.playOption2.value_or("NORMAL"),
                   .seed = options.playOption2Seed};
  input.assistOption = options.assistOption;
  input.inputDevices = options.inputDeviceCategories;
  input.autoPlay = options.autoPlay;
  input.practice = options.practiceMode;
  input.ruleset =
      options.courseSession != nullptr
          ? options.courseSession->rulesetDescriptor
          : options.rulesetDescriptor.value_or(RulesetDescriptor::Current());
  return makeScoreProvenance(input);
}

inline StartOptions makeCourseReplayStageStartOptions(
    const std::shared_ptr<CoursePlaySession> &session,
    const std::shared_ptr<ReplayData> &stageReplay) {
  StartOptions options;
  options.startPosition = 0;
  options.autoKeySound = false;
  options.autoPlay = false;
  options.ownsChart = true;
  if (session != nullptr) {
    options.gaugeType = session->gaugeType;
    options.gaugeProfile = session->gaugeProfile;
    options.gaugeAutoShift = session->gaugeAutoShift;
    options.courseSession = session;
    options.courseConstraints = session->constraints;
    options.touchVisualizationEnabled =
        session->replayTouchVisualizationEnabled;
    options.replayGhostRenderingEnabled = session->replayGhostRenderingEnabled;
  }
  if (stageReplay != nullptr) {
    options.replayData = stageReplay;
    options.playOption = stageReplay->playOption;
    options.playOptionSeed = stageReplay->playOptionSeed;
    options.playOption2 = stageReplay->playOption2;
    options.playOption2Seed = stageReplay->playOption2Seed;
    options.longNoteMode =
        normalizeChartLongNoteModeValue(stageReplay->chartMeta.LnMode);
    options.assistOption = stageReplay->assistOption;
  }
  return options;
}
