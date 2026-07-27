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
#include "ReplayResultContext.h"

#include <algorithm>
#include <array>
#include <cctype>
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
  std::shared_ptr<const JudgedPlaybackData> replayAnalysis = nullptr;
  std::optional<ReplayResultContext> replayResultContext;
  std::shared_ptr<JudgedPlaybackData> gbattleRecordData = nullptr;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  replay::DoublePlayOption doublePlayOption = replay::DoublePlayOption::Normal;
  int longNoteMode = 0;
  // Preserved before chart materialization mutates ChartMeta::LnMode.
  std::optional<bool> hasUndefinedLongNotes;
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
  std::optional<GaugeStateSnapshot> startingGaugeState;
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
[[nodiscard]] inline bool
hasUndefinedLongNotes(const bms_parser::ChartMeta &chartMeta) noexcept {
  return replay::captureAuthoredReplayChartFacts(chartMeta)
      .hasUndefinedLongNotes;
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
  options.startingGaugeState.reset();
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

inline void
applyReplaySetupToStartOptions(StartOptions &options,
                               const replay::ChartPlaybackSetup &setup) {
  options.gaugeType = setup.initialGaugeType;
  options.gaugeProfile = setup.gaugeProfile;
  options.gaugeAutoShift = setup.gaugeAutoShift;
  options.gaugeAutoShiftLowerBound = setup.gaugeAutoShiftLowerBound;
  options.playOption = setup.playOption;
  options.playOptionSeed = setup.playOptionSeed;
  options.playOption2 = setup.playOption2;
  options.playOption2Seed = setup.playOption2Seed;
  options.doublePlayOption = setup.doublePlayOption;
  options.longNoteMode = setup.longNoteMode;
  options.hasUndefinedLongNotes = setup.hasUndefinedLongNotes;
  options.assistOption = setup.assistOption;
  options.playback.percent = setup.playbackRatePercent;
  options.playback.mode = setup.playbackMode;
  options.replayCandidateSelection = setup.candidateSelection;
  options.clubMode = setup.clubMode;
  options.judgeWindowScalePercent = setup.judgeWindowScalePercent;
  options.startingGaugePercent =
      static_cast<int>(std::lround(setup.startingGaugePercent));
  options.startingGaugeState = setup.startingGaugeState;
  if (const auto ruleset = gameplayRulesetFromId(setup.playbackRulesetId)) {
    options.ruleset = *ruleset;
  }
  options.requiredRulesetDescriptor = rulesetDescriptorFromReplayIdentity(
      setup.playbackRulesetId, setup.playbackRulesetRevision);
}

inline void
applyJudgedPlaybackSetupToStartOptions(StartOptions &options,
                                       const JudgedPlaybackData &replay) {
  applyReplaySetupToStartOptions(options, replay.setup);
  // Provenance distinguishes an explicit starting percentage from the
  // ruleset/gauge default. A judged projection without an exact raw gauge
  // snapshot must preserve that distinction instead of turning its concrete
  // display scalar into an explicit override.
  if (!replay.setup.startingGaugeState.has_value()) {
    options.startingGaugePercent = replay.context.startingGaugePercent;
  }
  if (isLegacyRulesetIdentity(replay.setup.playbackRulesetId,
                              replay.setup.playbackRulesetRevision)) {
    options.ruleset = GameplayRuleset::Beatoraja;
    options.requiredRulesetDescriptor =
        RulesetDescriptor::For(GameplayRuleset::Beatoraja);
    options.replayRulesetOverride.reset();
    return;
  }
  if (replay.context.ruleset.id == replay.setup.playbackRulesetId &&
      replay.context.ruleset.version == replay.setup.playbackRulesetRevision) {
    options.requiredRulesetDescriptor = replay.context.ruleset;
  }
  if (replay.context.policy.has_value()) {
    options.replayRulesetOverride =
        analysis::scoreStageFrom(*replay.context.policy);
  } else {
    options.replayRulesetOverride.reset();
  }
}

inline void
applyResultRetrySetupToStartOptions(StartOptions &options,
                                    const JudgedPlaybackData &retrySource) {
  applyJudgedPlaybackSetupToStartOptions(options, retrySource);
}

inline void applyReplayPlaybackToStartOptions(
    StartOptions &options,
    std::shared_ptr<const replay::ReplayPlaybackData> playback) {
  if (playback == nullptr) {
    return;
  }
  applyReplaySetupToStartOptions(options, playback->setup);
  options.replayPlayback = std::move(playback);
}

[[nodiscard]] inline int
replayInitialLaneCoverPercent(const StartOptions &options,
                              int settingsFallbackPercent) noexcept {
  if (options.replayPlayback != nullptr) {
    return replay::initialLaneCoverPercentForRendering(
        options.replayPlayback->setup, settingsFallbackPercent);
  }
  if (options.replayData != nullptr) {
    return replay::initialLaneCoverPercentForRendering(
        options.replayData->setup, settingsFallbackPercent);
  }
  return settingsFallbackPercent;
}

inline void applyGBattleReplayChartSetupToStartOptions(
    StartOptions &options, const JudgedPlaybackData &recordData) {
  const auto &setup = recordData.setup;
  options.playOption = setup.playOption;
  options.playOptionSeed = setup.playOptionSeed;
  options.playOption2 = setup.playOption2;
  options.playOption2Seed = setup.playOption2Seed;
  options.doublePlayOption = setup.doublePlayOption;
  options.longNoteMode = setup.longNoteMode;
  options.hasUndefinedLongNotes = setup.hasUndefinedLongNotes;
  options.assistOption = setup.assistOption;
}

[[nodiscard]] inline std::shared_ptr<const replay::ReplayPlaybackData>
rawReplayResultSource(const StartOptions &options) noexcept {
  return options.replayData == nullptr ? options.replayPlayback : nullptr;
}

[[nodiscard]] inline const JudgedPlaybackData *
replayAnalysisSource(const StartOptions &options) noexcept {
  return options.replayData != nullptr ? options.replayData.get()
                                       : options.replayAnalysis.get();
}

[[nodiscard]] inline StartOptions replayResultStartOptions(
    std::shared_ptr<const replay::ReplayPlaybackData> playback,
    std::shared_ptr<const JudgedPlaybackData> analysis = nullptr,
    std::optional<ReplayResultContext> resultContext = std::nullopt) {
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = false,
      .autoPlay = false,
      .replayAnalysis = std::move(analysis),
      .replayResultContext = std::move(resultContext),
      .ownsChart = true,
  };
  applyReplayPlaybackToStartOptions(options, std::move(playback));
  return options;
}

[[nodiscard]] inline StartOptions replayResultStartOptions(
    std::shared_ptr<JudgedPlaybackData> playback,
    std::optional<ReplayResultContext> resultContext = std::nullopt) {
  StartOptions options{
      .startPosition = 0,
      .autoKeySound = false,
      .autoPlay = false,
      .replayData = playback,
      .replayResultContext = std::move(resultContext),
      .ownsChart = true,
  };
  if (playback != nullptr) {
    applyJudgedPlaybackSetupToStartOptions(options, *playback);
  }
  return options;
}

inline void applyCourseReplayPlaybackToStartOptions(
    StartOptions &options,
    std::shared_ptr<const replay::ReplayPlaybackData> playback,
    const CourseConstraintRules &constraints) {
  applyReplayPlaybackToStartOptions(options, std::move(playback));
  options.courseConstraints = constraints;
}

[[nodiscard]] inline AppSettings::NotePriorityMode
effectiveNotePriorityModeAtPlayStart(const StartOptions &options,
                                     AppSettings::NotePriorityMode fallback) {
  return options.replayCandidateSelection.has_value()
             ? notePriorityForCandidateSelection(
                   *options.replayCandidateSelection)
             : fallback;
}

[[nodiscard]] inline gameplay::GameplayPolicyBuildOutcome
buildGameplayRulesetPolicyAtPlayStart(
    const StartOptions &options, const bms_parser::ChartMeta &chartMeta,
    AppSettings::NotePriorityMode notePriorityMode) {
  const GameplayRuleset selectedRuleset = options.courseSession != nullptr
                                              ? options.courseSession->ruleset
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
       .beatorajaCandidateSelection = options.replayCandidateSelection.value_or(
           candidateSelectionForNotePriority(notePriorityMode)),
       .requiredDescriptor = std::move(requiredDescriptor),
       .replaySnapshot = options.replayRulesetOverride});
  if (outcome.policy.has_value() && options.courseSession != nullptr &&
      !gameplay::courseSessionAcceptsPolicy(*options.courseSession,
                                            *outcome.policy)) {
    outcome.status = gameplay::GameplayPolicyBuildStatus::UnsupportedRuleset;
    outcome.policy.reset();
    outcome.diagnostic = "This course stage uses a different gameplay ruleset.";
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

[[nodiscard]] inline replay::ChartPlaybackSetup
capturePlaybackSetupAtPlayStart(const StartOptions &options,
                                const bms_parser::ChartMeta &chartMeta,
                                const gameplay::GameplayRulesetPolicy &policy) {
  replay::ChartPlaybackSetup setup;
  setup.chartMd5 = chartMeta.MD5;
  setup.chartSha256 = chartMeta.SHA256;
  std::ranges::transform(setup.chartMd5, setup.chartMd5.begin(),
                         [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
  std::ranges::transform(setup.chartSha256, setup.chartSha256.begin(),
                         [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
  setup.keyMode = chartMeta.KeyMode;
  const int authoredLongNoteMode =
      normalizeChartLongNoteModeValue(chartMeta.LnMode);
  setup.hasUndefinedLongNotes = options.hasUndefinedLongNotes.value_or(
      play_start_detail::hasUndefinedLongNotes(chartMeta));
  setup.longNoteMode =
      authoredLongNoteMode > long_note_mode::kUnknownValue
          ? authoredLongNoteMode
          : (setup.hasUndefinedLongNotes
                 ? long_note_mode::normalizeSelectedValue(options.longNoteMode)
                 : long_note_mode::kUnknownValue);
  setup.randomSeed = chartMeta.RandomSeed;
  setup.randomPrng = chartMeta.RandomPrng;
  setup.randomValues = chartMeta.RandomValues;
  setup.playOption = options.playOption;
  setup.playOptionSeed = options.playOptionSeed;
  setup.playOption2 = options.playOption2;
  setup.playOption2Seed = options.playOption2Seed;
  setup.doublePlayOption = options.doublePlayOption;
  setup.assistOption = assist_options::normalize(options.assistOption);
  setup.initialGaugeType = options.gaugeType;
  setup.gaugeProfile = policy.gauge.resolvedProfile;
  setup.gaugeAutoShift = options.gaugeAutoShift;
  setup.gaugeAutoShiftLowerBound = options.gaugeAutoShiftLowerBound;
  setup.playbackRulesetId = policy.descriptor.id;
  setup.playbackRulesetRevision = policy.descriptor.version;
  setup.playbackRatePercent = options.playback.percent;
  setup.playbackMode = options.playback.mode;
  setup.candidateSelection = policy.judge.rules().candidateSelection;
  setup.judgeWindowScalePercent = options.judgeWindowScalePercent;
  setup.startingGaugePercent =
      static_cast<float>(options.startingGaugePercent.value_or(20));
  setup.clubMode = options.clubMode;
  return setup;
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
  input.doublePlayOption = options.doublePlayOption;
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
    const replay::ChartPlaybackSetup &setup, bool policyCanonical = true) {
  ScoreProvenanceBuildInput input;
  input.chartMeta = chartMeta;
  input.longNoteMode = setup.longNoteMode;
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
  input.candidateSelection = setup.candidateSelection;
  input.policyCanonical = policy.canonical && policyCanonical;
  input.gaugeType = setup.initialGaugeType;
  input.gaugeProfile = setup.gaugeProfile;
  input.gaugeAutoShift = setup.gaugeAutoShift;
  input.gaugeAutoShiftLowerBound = setup.gaugeAutoShiftLowerBound;
  input.player1 = {.option = setup.playOption.value_or("NORMAL"),
                   .seed = setup.playOptionSeed};
  input.player2 = {.option = setup.playOption2.value_or("NORMAL"),
                   .seed = setup.playOption2Seed};
  input.doublePlayOption = setup.doublePlayOption;
  input.assistOption = setup.assistOption;
  input.inputDevices = options.inputDeviceCategories;
  input.autoPlay = options.autoPlay;
  input.practice = options.practiceMode;
  input.clubMode = setup.clubMode;
  input.playback = {.percent = setup.playbackRatePercent,
                    .mode = setup.playbackMode};
  input.judgeWindowScalePercent = setup.judgeWindowScalePercent;
  input.startingGaugePercent = options.startingGaugePercent;
  input.ruleset = policy.descriptor;
  return makeScoreProvenance(input);
}

[[nodiscard]] inline ScoreProvenance
captureScoreProvenanceAtPlayStart(const StartOptions &options,
                                  const bms_parser::ChartMeta &chartMeta,
                                  const gameplay::GameplayRulesetPolicy &policy,
                                  bool policyCanonical = true) {
  const auto setup =
      capturePlaybackSetupAtPlayStart(options, chartMeta, policy);
  return captureScoreProvenanceAtPlayStart(options, chartMeta, policy, setup,
                                           policyCanonical);
}

inline StartOptions
makeCourseStageStartOptions(const std::shared_ptr<CoursePlaySession> &session) {
  StartOptions options;
  options.startPosition = 0;
  options.autoPlay = false;
  options.ownsChart = true;
  if (session == nullptr) {
    return options;
  }
  options.autoKeySound = session->autoKeySound;
  options.gaugeType = session->gaugeType;
  options.gaugeProfile = session->gaugeProfile;
  options.gaugeAutoShift = session->gaugeAutoShift;
  options.gaugeAutoShiftLowerBound = session->gaugeAutoShiftLowerBound;
  options.playOption = session->playOption;
  options.playOptionSeed = session->playOptionSeed;
  options.playOption2 = session->playOption2;
  options.playOption2Seed = session->playOption2Seed;
  options.longNoteMode = session->longNoteMode;
  options.hasUndefinedLongNotes =
      session->entryHasUndefinedLongNotes(session->currentIndex);
  options.assistOption = session->assistOption;
  options.clubMode = session->clubMode;
  options.courseSession = session;
  options.courseConstraints = session->constraints;
  options.ruleset = session->ruleset;
  options.requiredRulesetDescriptor = session->rulesetDescriptor;
  return enforceCoursePlaybackRules(std::move(options));
}

inline StartOptions makeCourseReplayStageStartOptions(
    const std::shared_ptr<CoursePlaySession> &session,
    const std::shared_ptr<JudgedPlaybackData> &stageReplay) {
  StartOptions options;
  options.startPosition = 0;
  options.autoKeySound = false;
  options.autoPlay = false;
  options.ownsChart = true;
  if (stageReplay != nullptr) {
    options.replayData = stageReplay;
    applyJudgedPlaybackSetupToStartOptions(options, *stageReplay);
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
    options.replayGhostRenderingEnabled = session->replayGhostRenderingEnabled;
  }
  return enforceCoursePlaybackRules(std::move(options));
}

inline StartOptions makeCourseReplayStageStartOptions(
    const std::shared_ptr<CoursePlaySession> &session,
    const std::shared_ptr<const replay::ReplayPlaybackData> &stagePlayback,
    std::shared_ptr<const JudgedPlaybackData> stageAnalysis = nullptr) {
  StartOptions options;
  options.startPosition = 0;
  options.autoKeySound = false;
  options.autoPlay = false;
  options.ownsChart = true;
  if (stagePlayback != nullptr) {
    applyReplayPlaybackToStartOptions(options, stagePlayback);
    options.replayAnalysis = std::move(stageAnalysis);
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
    options.replayGhostRenderingEnabled =
        options.replayAnalysis != nullptr ? session->replayGhostRenderingEnabled
                                          : std::optional<bool>(false);
  }
  return enforceCoursePlaybackRules(std::move(options));
}
