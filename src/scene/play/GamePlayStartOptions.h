#pragma once

#include "../../CoursePlaySession.h"
#include "../../ReplayData.h"
#include "../../AppSettings.h"
#include "../../input/InputTypes.h"
#include "../../practice/PracticeSession.h"
#include "Pacemaker.h"
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
  std::function<void(const ReplayData &)> practiceGhostCallback;
  std::vector<InputDeviceCategory> inputDeviceCategories;
  GameplayRuleset ruleset = kDefaultGameplayRuleset;
  std::optional<RulesetDescriptor> requiredRulesetDescriptor;
  std::optional<ScoreStageProvenance> replayRulesetOverride;
};

namespace play_start_detail {
[[nodiscard]] inline std::optional<
    std::array<gameplay::JudgeWindowSet, 4>>
validatedJudgeContexts(const ScoreStageProvenance &stage) {
  constexpr std::array contexts{
      gameplay::JudgeWindowContext::Normal,
      gameplay::JudgeWindowContext::Scratch,
      gameplay::JudgeWindowContext::LongNoteTail,
      gameplay::JudgeWindowContext::LongScratchTail,
  };
  constexpr std::array judgements{PGreat, Great, Good, Bad, Kpoor};
  std::array<gameplay::JudgeWindowSet, 4> result{};
  std::array<bool, contexts.size() * judgements.size()> found{};
  for (const auto &window : stage.effectiveJudgeWindows) {
    const auto context = std::ranges::find(contexts, window.context);
    const auto judgement = std::ranges::find(judgements, window.judgement);
    if (context == contexts.end() || judgement == judgements.end() ||
        window.earlyMicros > 0 || window.lateMicros < 0 ||
        window.earlyMicros > window.lateMicros ||
        window.earlyMicros < -2'000'000 || window.lateMicros > 2'000'000) {
      return std::nullopt;
    }
    const std::size_t contextIndex = static_cast<std::size_t>(
        std::distance(contexts.begin(), context));
    const std::size_t judgementIndex = static_cast<std::size_t>(
        std::distance(judgements.begin(), judgement));
    const std::size_t index =
        contextIndex * judgements.size() + judgementIndex;
    if (found[index]) {
      return std::nullopt;
    }
    found[index] = true;
    result[contextIndex].windows[judgementIndex] = {
        window.judgement, window.earlyMicros, window.lateMicros};
  }
  if (!std::ranges::all_of(found, [](bool value) { return value; })) {
    return std::nullopt;
  }
  return result;
}

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

[[nodiscard]] inline std::optional<ScoreStageProvenance>
replayJudgeOverrideForChart(const ScoreProvenance &provenance,
                            const bms_parser::ChartMeta &chartMeta) {
  const ScoreStageProvenance *matching =
      score_provenance::uniqueStageForChart(provenance, chartMeta);
  if (matching == nullptr) {
    return std::nullopt;
  }
  ScoreStageProvenance result = *matching;
  const bool missingLegacyGaugeProof =
      result.totalNotes == 0 && result.effectiveGaugeTotal == 0.0;
  if (missingLegacyGaugeProof &&
      provenance.ruleset ==
          RulesetDescriptor::For(GameplayRuleset::Beatoraja)) {
    result.totalNotes = chartMeta.TotalNotes;
    result.authoredGaugeTotal =
        chartMeta.HasTotal ? std::optional<double>(chartMeta.Total)
                           : std::nullopt;
    result.effectiveGaugeTotal =
        resolveEffectiveGaugeTotal(GameplayRuleset::Beatoraja, chartMeta);
    result.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
  }
  if (result.totalNotes <= 0 ||
      !std::isfinite(result.effectiveGaugeTotal) ||
      result.effectiveGaugeTotal <= 0.0 ||
      !validatedJudgeContexts(result).has_value()) {
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

inline void applyReplayProvenanceToStartOptions(StartOptions &options,
                                                const ReplayData &replay) {
  options.playback = replay.provenance.playback;
  options.clubMode = replay.provenance.clubMode;
  options.judgeWindowScalePercent = replay.provenance.judgeWindowScalePercent;
  options.startingGaugePercent = replay.provenance.startingGaugePercent;
  options.gaugeType = replay.provenance.gaugeType;
  options.gaugeProfile = replay.provenance.gaugeProfile;
  options.gaugeAutoShift = replay.provenance.gaugeAutoShift;
  options.gaugeAutoShiftLowerBound = replay.gaugeAutoShiftLowerBound;
  if (replay.provenance.ruleset == RulesetDescriptor::Legacy()) {
    options.ruleset = GameplayRuleset::Beatoraja;
    options.requiredRulesetDescriptor =
        RulesetDescriptor::For(GameplayRuleset::Beatoraja);
    options.replayRulesetOverride.reset();
  } else {
    options.requiredRulesetDescriptor = replay.provenance.ruleset;
    if (const auto recordedRuleset =
            gameplayRulesetFromId(replay.provenance.ruleset.id)) {
      options.ruleset = *recordedRuleset;
    }
    options.replayRulesetOverride =
        play_start_detail::replayJudgeOverrideForChart(replay.provenance,
                                                       replay.chartMeta);
  }
}

[[nodiscard]] inline gameplay::CandidateSelectionMode
candidateSelectionForNotePriority(AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Combo:
    return gameplay::CandidateSelectionMode::Combo;
  case AppSettings::NotePriorityMode::Duration:
    return gameplay::CandidateSelectionMode::Duration;
  case AppSettings::NotePriorityMode::Score:
    return gameplay::CandidateSelectionMode::Score;
  case AppSettings::NotePriorityMode::Lowest:
  default:
    return gameplay::CandidateSelectionMode::Lowest;
  }
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
           candidateSelectionForNotePriority(notePriorityMode),
       .requiredDescriptor = std::move(requiredDescriptor),
       .replaySnapshot = options.replayRulesetOverride});
  const bool legacyReplayFallback =
      options.replayData != nullptr &&
      options.replayData->provenance.ruleset == RulesetDescriptor::Legacy();
  if (outcome.built() && options.replayData != nullptr &&
      !options.replayRulesetOverride.has_value() && !legacyReplayFallback) {
    outcome.status =
        gameplay::GameplayPolicyBuildStatus::InvalidReplaySnapshot;
    outcome.policy.reset();
    outcome.diagnostic =
        "The replay does not contain a complete gameplay policy snapshot.";
  }
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
    applyReplayProvenanceToStartOptions(options, *stageReplay);
  }
  return enforceCoursePlaybackRules(std::move(options));
}
