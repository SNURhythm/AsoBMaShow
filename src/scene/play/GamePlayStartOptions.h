#pragma once

#include "../../CoursePlaySession.h"
#include "../../analysis/JudgedPlaybackData.h"
#include "../../analysis/JudgedPlaybackContext.h"
#include "../../AppSettings.h"
#include "../../input/InputTypes.h"
#include "../../practice/PracticeSession.h"
#include "../../replay/ReplayPlaybackData.h"
#include "Pacemaker.h"
#include "GameplayCandidateSelection.h"
#include "GameplayRulesetPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

class Scene;

enum class PlayStartInputPlatform { Desktop, Mobile };

[[nodiscard]] inline InputDeviceCategory
playStartInputDeviceCategory(input::DeviceClass deviceClass) {
  switch (deviceClass) {
  case input::DeviceClass::Keyboard:
    return InputDeviceCategory::Keyboard;
  case input::DeviceClass::GameController:
    return InputDeviceCategory::GameController;
  case input::DeviceClass::Joystick:
    return InputDeviceCategory::Joystick;
  case input::DeviceClass::Touch:
    return InputDeviceCategory::Touch;
  case input::DeviceClass::Midi:
    return InputDeviceCategory::Midi;
  case input::DeviceClass::Gyroscope:
    return InputDeviceCategory::Gyroscope;
  }
  return InputDeviceCategory::Unknown;
}

[[nodiscard]] inline std::vector<InputDeviceCategory>
collectPlayStartInputDeviceCategories(
    std::span<const input::DeviceClass> resolverDeviceClasses,
    PlayStartInputPlatform platform) {
  std::vector<InputDeviceCategory> categories;
  categories.reserve(resolverDeviceClasses.size() + 1);
  if (platform == PlayStartInputPlatform::Mobile) {
    categories.push_back(InputDeviceCategory::Touch);
  }
  for (const auto deviceClass : resolverDeviceClasses) {
    categories.push_back(playStartInputDeviceCategory(deviceClass));
  }
  std::ranges::sort(categories);
  categories.erase(std::unique(categories.begin(), categories.end()),
                   categories.end());
  if (categories.empty()) {
    categories.push_back(InputDeviceCategory::Keyboard);
  }
  return categories;
}

struct StartOptions {
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  bool autoPlay = false;
  GaugeType gaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::shared_ptr<JudgedPlaybackData> replayData = nullptr;
  std::shared_ptr<const replay::ReplayPlaybackData> replayPlayback = nullptr;
  std::shared_ptr<JudgedPlaybackData> gbattleRecordData = nullptr;
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
  std::shared_ptr<practice::Session> practiceSession = nullptr;
  bool practiceMode = false;
  unsigned long long practiceLeadInMicros = 0;
  audio::PlaybackRate playback;
  bool clubMode = false;
  int judgeWindowScalePercent = 100;
  std::optional<int> startingGaugePercent;
  Scene *returnScene = nullptr;
  std::optional<bool> touchVisualizationEnabled;
  std::optional<bool> replayGhostRenderingEnabled;
  std::function<void(const JudgedPlaybackData &)> practiceGhostCallback;
  std::vector<InputDeviceCategory> inputDeviceCategories;
  GameplayRuleset ruleset = kDefaultGameplayRuleset;
  std::optional<RulesetDescriptor> requiredRulesetDescriptor;
  std::optional<ScoreStageProvenance> replayRulesetOverride;
  std::optional<gameplay::CandidateSelectionMode> replayCandidateSelection;
};

namespace play_start_detail {
[[nodiscard]] inline std::optional<
    std::map<Judgement, std::pair<long long, long long>>>
validatedJudgeWindows(const ScoreStageProvenance &stage) {
  constexpr std::array expected = {PGreat, Great, Good, Bad, Kpoor};
  std::map<Judgement, std::pair<long long, long long>> result;
  for (const auto &window : stage.effectiveJudgeWindows) {
    if (window.context != gameplay::JudgeWindowContext::Normal) {
      continue;
    }
    if (std::ranges::find(expected, window.judgement) == expected.end() ||
        window.earlyMicros > 0 || window.lateMicros < 0 ||
        window.earlyMicros > window.lateMicros ||
        !result
             .emplace(window.judgement,
                      std::pair<long long, long long>(window.earlyMicros,
                                                      window.lateMicros))
             .second) {
      return std::nullopt;
    }
  }
  if (result.size() != expected.size()) {
    return std::nullopt;
  }
  return result;
}

} // namespace play_start_detail

inline void applyPracticeConfigurationToStartOptions(
    StartOptions &options, const practice::Configuration &configuration) {
  options.startPosition =
      static_cast<unsigned long long>(std::max(0LL, configuration.startMicros));
  options.gaugeType = configuration.gaugeType;
  options.gaugeAutoShift = configuration.gaugeAutoShift;
  options.gaugeAutoShiftLowerBound = configuration.gaugeAutoShiftLowerBound;
  options.practiceMode = true;
  options.playback = configuration.playback;
  options.judgeWindowScalePercent = configuration.judge.scalePercent;
  options.startingGaugePercent = configuration.startingGaugePercent;
}

[[nodiscard]] inline audio::PlaybackRate resultRetryPlayback(
    const ScoreProvenance &attemptProvenance,
    const std::optional<practice::Configuration> &practiceConfiguration) {
  return practiceConfiguration.has_value() ? practiceConfiguration->playback
                                           : attemptProvenance.playback;
}

[[nodiscard]] inline int
resultRetryLongNoteMode(const bms_parser::ChartMeta &chartMeta,
                        const ScoreProvenance &attemptProvenance) {
  const int chartLongNoteMode =
      normalizeChartLongNoteModeValue(chartMeta.LnMode);
  if (chartLongNoteMode > long_note_mode::kUnknownValue) {
    return chartLongNoteMode;
  }
  const ScoreStageProvenance *stage =
      score_provenance::uniqueStageForChart(attemptProvenance, chartMeta);
  return stage == nullptr
             ? long_note_mode::kUnknownValue
             : normalizeChartLongNoteModeValue(stage->longNoteMode);
}

[[nodiscard]] inline Judge
makeEffectiveJudgeAtPlayStart(const StartOptions &options, int rank) {
  Judge judge(rank);
  judge.applyCourseJudgementConstraint(options.courseConstraints.judgement);
  judge.applyWindowScale(options.playback.percent,
                         options.judgeWindowScalePercent);
  return judge;
}

[[nodiscard]] inline Judge
makeEffectiveJudgeAtPlayStart(const StartOptions &options,
                              const bms_parser::ChartMeta &chartMeta) {
  Judge judge = makeEffectiveJudgeAtPlayStart(options, chartMeta.Rank);
  if (options.replayRulesetOverride.has_value() &&
      score_provenance::stageMatchesChart(*options.replayRulesetOverride,
                                          chartMeta)) {
    if (const auto windows = play_start_detail::validatedJudgeWindows(
            *options.replayRulesetOverride)) {
      judge.timingWindows = *windows;
    }
  }
  return judge;
}

inline void applyJudgedPlaybackContextToStartOptions(
    StartOptions &options, const JudgedPlaybackData &replay) {
  options.playback = replay.context.playback;
  options.replayCandidateSelection = replay.context.candidateSelection;
  options.judgeWindowScalePercent = replay.context.judgeWindowScalePercent;
  options.startingGaugePercent = replay.context.startingGaugePercent;
  options.clubMode = replay.context.clubMode;
  options.gaugeType = replay.initialGaugeType;
  options.gaugeProfile = replay.gaugeProfile;
  options.gaugeAutoShift = replay.gaugeAutoShift;
  options.gaugeAutoShiftLowerBound = replay.gaugeAutoShiftLowerBound;
  if (replay.context.ruleset == RulesetDescriptor::Legacy()) {
    options.ruleset = GameplayRuleset::Beatoraja;
    options.requiredRulesetDescriptor =
        RulesetDescriptor::For(GameplayRuleset::Beatoraja);
    options.replayRulesetOverride.reset();
  } else {
    options.requiredRulesetDescriptor = replay.context.ruleset;
    if (const auto recordedRuleset =
            gameplayRulesetFromId(replay.context.ruleset.id)) {
      options.ruleset = *recordedRuleset;
    }
    options.replayRulesetOverride =
        replay.context.policy.has_value()
            ? std::optional<ScoreStageProvenance>(
                  analysis::scoreStageFrom(*replay.context.policy))
            : std::nullopt;
  }
}

inline void applyReplayPlaybackToStartOptions(
    StartOptions &options,
    std::shared_ptr<const replay::ReplayPlaybackData> playback) {
  if (playback == nullptr) {
    return;
  }
  const auto &setup = playback->setup;
  options.replayPlayback = std::move(playback);
  options.gaugeType = setup.initialGaugeType;
  options.gaugeProfile = setup.gaugeProfile;
  options.gaugeAutoShift = setup.gaugeAutoShift;
  options.gaugeAutoShiftLowerBound = setup.gaugeAutoShiftLowerBound;
  options.playOption = setup.playOption;
  options.playOptionSeed = setup.playOptionSeed;
  options.playOption2 = setup.playOption2;
  options.playOption2Seed = setup.playOption2Seed;
  options.longNoteMode = setup.longNoteMode;
  options.assistOption = setup.assistOption;
  options.playback.percent = setup.playbackRatePercent;
  options.playback.mode = setup.playbackMode;
  options.replayCandidateSelection = setup.candidateSelection;
  options.clubMode = setup.clubMode;
  options.judgeWindowScalePercent = setup.judgeWindowScalePercent;
  options.startingGaugePercent =
      static_cast<int>(std::lround(setup.startingGaugePercent));
  if (const auto ruleset =
          gameplayRulesetFromId(setup.playbackRulesetId)) {
    options.ruleset = *ruleset;
    const auto descriptor = RulesetDescriptor::For(*ruleset);
    if (descriptor.version == setup.playbackRulesetRevision) {
      options.requiredRulesetDescriptor = descriptor;
    }
  }
}

inline void applyCourseReplayPlaybackToStartOptions(
    StartOptions &options,
    std::shared_ptr<const replay::ReplayPlaybackData> playback,
    const CourseConstraintRules &constraints) {
  applyReplayPlaybackToStartOptions(options, std::move(playback));
  options.courseConstraints = constraints;
}

[[nodiscard]] inline AppSettings::NotePriorityMode
effectiveNotePriorityModeAtPlayStart(
    const StartOptions &options, AppSettings::NotePriorityMode fallback) {
  return options.replayCandidateSelection.has_value()
             ? notePriorityForCandidateSelection(
                   *options.replayCandidateSelection)
             : fallback;
}

[[nodiscard]] inline gameplay::GameplayPolicyBuildOutcome
buildGameplayRulesetPolicyAtPlayStart(
    const StartOptions &options, const bms_parser::ChartMeta &chartMeta,
    AppSettings::NotePriorityMode notePriorityMode) {
  const GameplayRuleset selectedRuleset =
      options.courseSession != nullptr ? options.courseSession->ruleset
                                       : options.ruleset;
  std::optional<RulesetDescriptor> requiredDescriptor =
      options.requiredRulesetDescriptor;
  if (!requiredDescriptor.has_value() && options.courseSession != nullptr) {
    requiredDescriptor = options.courseSession->rulesetDescriptor;
  }
  const int sourceRank =
      options.replayRulesetOverride.has_value() &&
              options.replayRulesetOverride->sourceJudgeRank.has_value()
          ? *options.replayRulesetOverride->sourceJudgeRank
          : chartMeta.Rank;
  auto outcome = gameplay::buildGameplayRulesetPolicy(
      chartMeta,
      {.ruleset = selectedRuleset,
       .gaugeProfile = options.gaugeProfile,
       .sourceRank = sourceRank,
       .playbackRatePercent = options.playback.percent,
       .judgeScalePercent = options.judgeWindowScalePercent,
       .courseJudgement = options.courseConstraints.judgement,
       .beatorajaCandidateSelection =
           options.replayCandidateSelection.value_or(
               candidateSelectionForNotePriority(notePriorityMode)),
       .requiredDescriptor = std::move(requiredDescriptor),
       .replaySnapshot = options.replayRulesetOverride});
  if (outcome.policy.has_value() && options.courseSession != nullptr &&
      !gameplay::courseSessionAcceptsPolicy(*options.courseSession,
                                            *outcome.policy)) {
    outcome.status = gameplay::GameplayPolicyBuildStatus::UnsupportedRuleset;
    outcome.policy.reset();
    outcome.diagnostic =
        "This course stage uses a different gameplay ruleset.";
  }
  return outcome;
}

[[nodiscard]] inline StartOptions
enforceCoursePlaybackRules(StartOptions options) {
  if (options.courseSession != nullptr) {
    options.playback = course_rules::kRequiredPlaybackRate;
  }
  return options;
}

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
  input.totalNotes = chartMeta.TotalNotes;
  input.authoredGaugeTotal = chartMeta.HasTotal
                                 ? std::optional<double>(chartMeta.Total)
                                 : std::nullopt;
  input.effectiveGaugeTotal = resolveEffectiveGaugeTotal(
      options.courseSession != nullptr ? options.courseSession->ruleset
                                       : options.ruleset,
      chartMeta);
  input.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
  input.gaugeType = options.gaugeType;
  input.gaugeProfile =
      options.gaugeProfile == GaugeProfile::CourseDefault
          ? GaugeProfile::CourseDefault
          : resolveGaugeProfile(options.gaugeProfile, chartMeta.KeyMode);
  input.gaugeAutoShift = options.gaugeAutoShift;
  input.gaugeAutoShiftLowerBound = options.gaugeAutoShiftLowerBound;
  input.player1 = {.option = options.playOption.value_or("NORMAL"),
                   .seed = options.playOptionSeed};
  input.player2 = {.option = options.playOption2.value_or("NORMAL"),
                   .seed = options.playOption2Seed};
  input.assistOption = options.assistOption;
  input.inputDevices = options.inputDeviceCategories;
  input.autoPlay = options.autoPlay;
  input.practice = options.practiceMode;
  input.clubMode = options.clubMode;
  input.playback = options.playback;
  input.judgeWindowScalePercent = options.judgeWindowScalePercent;
  input.startingGaugePercent = options.startingGaugePercent;
  input.ruleset = options.courseSession != nullptr
                      ? options.courseSession->rulesetDescriptor
                      : RulesetDescriptor::For(options.ruleset);
  return makeScoreProvenance(input);
}

[[nodiscard]] inline ScoreProvenance captureScoreProvenanceAtPlayStart(
    const StartOptions &options, const bms_parser::ChartMeta &chartMeta,
    const gameplay::GameplayRulesetPolicy &policy,
    bool policyCanonical = true) {
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
  input.effectiveJudgeContexts = policy.judge.rules().contexts;
  input.totalNotes = policy.gauge.totalNotes;
  input.authoredGaugeTotal = chartMeta.HasTotal
                                 ? std::optional<double>(chartMeta.Total)
                                 : std::nullopt;
  input.effectiveGaugeTotal = policy.gauge.effectiveTotal;
  input.candidateSelection = policy.judge.rules().candidateSelection;
  input.policyCanonical = policy.canonical && policyCanonical;
  input.gaugeType = options.gaugeType;
  input.gaugeProfile = policy.gauge.resolvedProfile;
  input.gaugeAutoShift = options.gaugeAutoShift;
  input.gaugeAutoShiftLowerBound = options.gaugeAutoShiftLowerBound;
  input.player1 = {.option = options.playOption.value_or("NORMAL"),
                   .seed = options.playOptionSeed};
  input.player2 = {.option = options.playOption2.value_or("NORMAL"),
                   .seed = options.playOption2Seed};
  input.assistOption = options.assistOption;
  input.inputDevices = options.inputDeviceCategories;
  input.autoPlay = options.autoPlay;
  input.practice = options.practiceMode;
  input.clubMode = options.clubMode;
  input.playback = options.playback;
  input.judgeWindowScalePercent = options.judgeWindowScalePercent;
  input.startingGaugePercent = options.startingGaugePercent;
  input.ruleset = policy.descriptor;
  return makeScoreProvenance(input);
}

inline StartOptions makeCourseReplayStageStartOptions(
    const std::shared_ptr<CoursePlaySession> &session,
    const std::shared_ptr<JudgedPlaybackData> &stageReplay) {
  StartOptions options;
  options.startPosition = 0;
  options.autoKeySound = false;
  options.autoPlay = false;
  options.ownsChart = true;
  if (session != nullptr) {
    options.gaugeType = session->gaugeType;
    options.gaugeProfile = session->gaugeProfile;
    options.gaugeAutoShift = session->gaugeAutoShift;
    options.gaugeAutoShiftLowerBound = session->gaugeAutoShiftLowerBound;
    options.courseSession = session;
    options.courseConstraints = session->constraints;
    options.ruleset = session->ruleset;
    options.requiredRulesetDescriptor = session->rulesetDescriptor;
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
    applyJudgedPlaybackContextToStartOptions(options, *stageReplay);
  }
  return enforceCoursePlaybackRules(std::move(options));
}

inline StartOptions makeCourseReplayStageStartOptions(
    const std::shared_ptr<CoursePlaySession> &session,
    const std::shared_ptr<const replay::ReplayPlaybackData> &stagePlayback) {
  StartOptions options;
  options.startPosition = 0;
  options.autoKeySound = false;
  options.autoPlay = false;
  options.ownsChart = true;
  if (stagePlayback != nullptr) {
    applyReplayPlaybackToStartOptions(options, stagePlayback);
  }
  if (session != nullptr) {
    options.gaugeType = session->gaugeType;
    options.gaugeProfile = session->gaugeProfile;
    options.gaugeAutoShift = session->gaugeAutoShift;
    options.gaugeAutoShiftLowerBound = session->gaugeAutoShiftLowerBound;
    options.courseSession = session;
    options.courseConstraints = session->constraints;
    options.ruleset = session->ruleset;
    options.requiredRulesetDescriptor = session->rulesetDescriptor;
    options.touchVisualizationEnabled =
        session->replayTouchVisualizationEnabled;
    options.replayGhostRenderingEnabled = false;
  }
  return enforceCoursePlaybackRules(std::move(options));
}
