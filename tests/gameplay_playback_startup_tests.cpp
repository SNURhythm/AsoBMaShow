#include "PlayOptionUtils.h"
#include "replay/ReplaySetupAuthority.h"
#include "scene/play/GamePlayStartup.h"
#include "scene/play/GamePlayStartOptions.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  JudgedPlaybackData replay;
  replay.setup.playbackRatePercent = 75;
  replay.setup.playbackMode = audio::PlaybackMode::TimeStretch;
  replay.setup.clubMode = true;
  replay.setup.playbackRulesetId = "beatoraja";
  replay.setup.playbackRulesetRevision = 2;
  StartOptions replayOptions{.replayData =
                                 std::make_shared<JudgedPlaybackData>(replay)};
  applyJudgedPlaybackSetupToStartOptions(replayOptions, replay);

  const auto failure = gameplay_startup::playbackInitializationResult(
      false, "TimeStretch playback mode is not supported");
  bool recordingStarted = false;
  bool sessionStarted = false;
  if (failure.mayStartAttempt) {
    recordingStarted = true;
    sessionStarted = true;
  }
  if (!expect(replayOptions.playback.mode == audio::PlaybackMode::TimeStretch,
              "replay provenance retains the unsupported playback mode") ||
      !expect(replayOptions.clubMode,
              "replay provenance restores Club audio independently") ||
      !expect(!failure.mayStartAttempt,
              "failed playback initialization blocks attempt startup") ||
      !expect(failure.visibleStatus.find("TimeStretch") != std::string::npos,
              "failed playback initialization exposes a visible reason") ||
      !expect(!recordingStarted && !sessionStarted,
              "recording and practice session start remain after the gate") ||
      !expect(gameplay_startup::failureReturnTarget(true) ==
                  gameplay_startup::FailureReturnTarget::RequestedScene,
              "a live owning scene is preferred for failure return") ||
      !expect(gameplay_startup::failureReturnTarget(false) ==
                  gameplay_startup::FailureReturnTarget::MainMenu,
              "a missing owner falls back to the main menu")) {
    return 1;
  }

  const auto success = gameplay_startup::playbackInitializationResult(true, {});
  if (!expect(success.mayStartAttempt && success.visibleStatus.empty(),
              "successful PitchShift or normal playback may start")) {
    return 1;
  }

  bms_parser::ChartMeta captureMeta;
  captureMeta.MD5 = std::string(32, 'c');
  captureMeta.SHA256 = std::string(64, 'd');
  captureMeta.KeyMode = 14;
  captureMeta.IsDP = true;
  captureMeta.LnMode = 2;
  captureMeta.RandomSeed = 71U;
  captureMeta.RandomPrng = "std::mt19937_64";
  captureMeta.RandomValues = {4, 2};
  captureMeta.TotalNotes = 1;
  StartOptions captureOptions;
  captureOptions.gaugeType = GaugeType::Hard;
  captureOptions.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  captureOptions.gaugeAutoShiftLowerBound = GaugeType::Easy;
  captureOptions.playOption = "MIRROR";
  captureOptions.playOptionSeed = 31;
  captureOptions.playOption2 = "RANDOM";
  captureOptions.playOption2Seed = 37;
  captureOptions.doublePlayOption = replay::DoublePlayOption::Flip;
  captureOptions.assistOption = assist_options::kDrag;
  captureOptions.playback = {.percent = 75,
                             .mode = audio::PlaybackMode::TimeStretch};
  captureOptions.clubMode = true;
  captureOptions.judgeWindowScalePercent = 90;
  captureOptions.startingGaugePercent = 37;
  const auto capturePolicy = buildGameplayRulesetPolicyAtPlayStart(
      captureOptions, captureMeta, AppSettings::NotePriorityMode::Score);
  if (!expect(capturePolicy.policy.has_value(),
              "capture invariant fixture builds a gameplay policy")) {
    return 1;
  }
  const auto capturedSetup = capturePlaybackSetupAtPlayStart(
      captureOptions, captureMeta, *capturePolicy.policy);
  const auto capturedProvenance = captureScoreProvenanceAtPlayStart(
      captureOptions, captureMeta, *capturePolicy.policy, capturedSetup);
  if (!expect(
          capturedSetup.doublePlayOption == replay::DoublePlayOption::Flip &&
              capturedProvenance.stages.size() == 1 &&
              capturedProvenance.stages.front().doublePlayOption ==
                  replay::DoublePlayOption::Flip &&
              capturedSetup.playOption == capturedProvenance.player1.option &&
              capturedSetup.playOption2 == capturedProvenance.player2.option &&
              capturedSetup.randomValues ==
                  capturedProvenance.stages.front().chartRandomValues &&
              capturedSetup.playbackRatePercent ==
                  capturedProvenance.playback.percent,
          "one captured setup feeds raw replay and provenance facts")) {
    return 1;
  }
  const result_persistence::ChartScoreWrite capturedScore{
      .chartMd5 = captureMeta.MD5,
      .chartSha256 = captureMeta.SHA256,
      .longNoteMode = capturedSetup.longNoteMode,
      .provenance = capturedProvenance,
  };
  const auto capturedResolution = replay::setup_authority::resolveForResult(
      capturedSetup, capturedScore, captureMeta.KeyMode,
      replay::setup_authority::Source::CapturedAttempt, false);
  if (!expect(capturedResolution.resolved(),
              "captured setup and provenance satisfy the persistence "
              "authority without reconstruction")) {
    return 1;
  }

  bms_parser::ChartMeta undefinedLongNoteMeta = captureMeta;
  undefinedLongNoteMeta.LnMode = long_note_mode::kUnknownValue;
  undefinedLongNoteMeta.TotalLongNotes = 1;
  StartOptions undefinedLongNoteOptions = captureOptions;
  undefinedLongNoteOptions.longNoteMode = long_note_mode::kHcnValue;
  undefinedLongNoteOptions.hasUndefinedLongNotes = true;
  bms_parser::ChartMeta materializedUndefinedLongNoteMeta =
      undefinedLongNoteMeta;
  materializedUndefinedLongNoteMeta.LnMode = long_note_mode::kHcnValue;
  const auto undefinedLongNotePolicy = buildGameplayRulesetPolicyAtPlayStart(
      undefinedLongNoteOptions, materializedUndefinedLongNoteMeta,
      AppSettings::NotePriorityMode::Score);
  if (!expect(undefinedLongNotePolicy.policy.has_value(),
              "undefined-LN capture fixture builds a gameplay policy")) {
    return 1;
  }
  const auto undefinedLongNoteSetup = capturePlaybackSetupAtPlayStart(
      undefinedLongNoteOptions, materializedUndefinedLongNoteMeta,
      *undefinedLongNotePolicy.policy);
  if (!expect(undefinedLongNoteSetup.hasUndefinedLongNotes &&
                  undefinedLongNoteSetup.longNoteMode ==
                      long_note_mode::kHcnValue,
              "capture retains that LN mode was selected for an undefined-LN "
              "chart before chart metadata is materialized")) {
    return 1;
  }

  auto raw = std::make_shared<replay::ReplayPlaybackData>();
  raw->setup.keyMode = 14;
  raw->setup.longNoteMode = 2;
  raw->setup.playOption = "R-RANDOM";
  raw->setup.playOptionSeed = 17;
  raw->setup.playOption2 = "MIRROR";
  raw->setup.playOption2Seed = 29;
  raw->setup.doublePlayOption = replay::DoublePlayOption::Flip;
  raw->setup.assistOption = "AUTO-SCRATCH";
  raw->setup.initialGaugeType = GaugeType::ExHard;
  raw->setup.gaugeProfile = GaugeProfile::Standard;
  raw->setup.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  raw->setup.gaugeAutoShiftLowerBound = GaugeType::Easy;
  raw->setup.playbackRulesetId = "beatoraja";
  raw->setup.playbackRulesetRevision = 2;
  raw->setup.playbackRatePercent = 125;
  raw->setup.playbackMode = audio::PlaybackMode::TimeStretch;
  raw->setup.candidateSelection = gameplay::CandidateSelectionMode::Score;
  raw->setup.judgeWindowScalePercent = 90;
  raw->setup.startingGaugePercent = 42.0F;
  GaugeStateSnapshot rawStartingGauge{
      .gaugeType = GaugeType::Hard,
      .selectedGaugeType = GaugeType::ExHard,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .currentGauge = 100.0F,
  };
  rawStartingGauge.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 20.0F;
  rawStartingGauge.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] = 100.0F;
  raw->setup.startingGaugeState = rawStartingGauge;
  raw->setup.clubMode = true;

  auto gbattleRecord = std::make_shared<JudgedPlaybackData>();
  gbattleRecord->chartMeta.LnMode = 2;
  gbattleRecord->setup = raw->setup;
  gbattleRecord->setup.playOption = "S-RANDOM";
  gbattleRecord->setup.playOptionSeed = 41;
  gbattleRecord->setup.playOption2 = "MIRROR";
  gbattleRecord->setup.playOption2Seed = 43;
  gbattleRecord->setup.assistOption = "AUTO-SCRATCH";

  StartOptions materializedGBattleOptions{
      .gaugeType = GaugeType::Easy,
      .playback = {.percent = 150},
  };
  applyGBattleReplayChartSetupToStartOptions(materializedGBattleOptions,
                                             *gbattleRecord);

  StartOptions legacyGBattleOptions;
  applyGBattleReplayChartSetupToStartOptions(legacyGBattleOptions,
                                             *gbattleRecord);

  const bool materializedGBattlePreservesFlip =
      expect(materializedGBattleOptions.doublePlayOption ==
                 replay::DoublePlayOption::Flip,
             "materialized raw G-Battle preserves its recorded DP FLIP option");
  const bool legacyGBattlePreservesFlip = expect(
      legacyGBattleOptions.doublePlayOption == replay::DoublePlayOption::Flip,
      "legacy-adapted raw G-Battle preserves its recorded DP FLIP option");
  if (!materializedGBattlePreservesFlip || !legacyGBattlePreservesFlip ||
      !expect(materializedGBattleOptions.playOption == "S-RANDOM" &&
                  materializedGBattleOptions.playOptionSeed == 41 &&
                  materializedGBattleOptions.playOption2 == "MIRROR" &&
                  materializedGBattleOptions.playOption2Seed == 43 &&
                  materializedGBattleOptions.gaugeType == GaugeType::Easy &&
                  materializedGBattleOptions.playback.percent == 150,
              "G-Battle keeps chart setup from judged playback without "
              "overwriting the live player's gauge or playback")) {
    return 1;
  }

  StartOptions rawOptions;
  applyReplayPlaybackToStartOptions(rawOptions, raw);
  auto rawAnalysis = std::make_shared<JudgedPlaybackData>();
  rawAnalysis->finalScore = 1234;
  rawOptions.replayAnalysis = rawAnalysis;
  rawOptions.replayResultContext = ReplayResultContext{
      .resultId = 73,
      .attemptId = "123e4567-e89b-42d3-a456-426614174073",
      .createdAt = "2026-07-25 01:02:03",
  };
  if (!expect(rawOptions.replayPlayback == raw,
              "raw replay ownership is attached to play startup") ||
      !expect(rawOptions.replayData == nullptr,
              "raw playback does not manufacture judged replay data") ||
      !expect(replayAnalysisSource(rawOptions) == rawAnalysis.get(),
              "raw playback exposes its separate non-authoritative analysis "
              "projection") ||
      !expect(rawOptions.gaugeType == GaugeType::ExHard &&
                  rawOptions.gaugeAutoShift == GaugeAutoShiftMode::BestClear &&
                  rawOptions.gaugeAutoShiftLowerBound == GaugeType::Easy,
              "raw playback restores its gauge setup") ||
      !expect(rawOptions.playOption == "R-RANDOM" &&
                  rawOptions.playOptionSeed == 17 &&
                  rawOptions.playOption2 == "MIRROR" &&
                  rawOptions.playOption2Seed == 29 &&
                  rawOptions.doublePlayOption == replay::DoublePlayOption::Flip,
              "raw playback restores DP FLIP and chart randomization") ||
      !expect(rawOptions.playback.percent == 125 &&
                  rawOptions.playback.mode ==
                      audio::PlaybackMode::TimeStretch &&
                  rawOptions.judgeWindowScalePercent == 90 &&
                  rawOptions.startingGaugePercent == 42 && rawOptions.clubMode,
              "raw playback restores timing and audio setup") ||
      !expect(rawOptions.startingGaugeState.has_value() &&
                  rawOptions.startingGaugeState->gaugeType == GaugeType::Hard &&
                  rawOptions.startingGaugeState
                          ->gaugeValues[gaugeTypeIndex(GaugeType::Normal)] ==
                      20.0F,
              "raw playback carries independent starting gauge state") ||
      !expect(rawOptions.ruleset == GameplayRuleset::Beatoraja &&
                  rawOptions.requiredRulesetDescriptor ==
                      RulesetDescriptor::For(GameplayRuleset::Beatoraja),
              "raw playback restores its ruleset identity")) {
    return 1;
  }
  if (!expect(rawReplayResultSource(rawOptions) == raw,
              "raw BRD playback remains available to the result scene") ||
      !expect(rawReplayResultSource(replayOptions) == nullptr,
              "legacy judged playback is not duplicated as a raw result "
              "source")) {
    return 1;
  }
  bms_parser::ChartMeta doublePlayMeta;
  doublePlayMeta.KeyMode = 14;
  doublePlayMeta.IsDP = true;
  const auto rawDisplay = play_options::formatReplayPlaybackModeDisplayLabel(
      doublePlayMeta, raw->setup);
  if (!expect(rawDisplay.mode == "FLIP + R-RANDOM #17 / MIRROR #29" &&
                  rawDisplay.laneOrder.empty(),
              "raw replay result labels DP FLIP and recorded player options")) {
    return 1;
  }
  const StartOptions resultReplayOptions =
      replayResultStartOptions(rawReplayResultSource(rawOptions), rawAnalysis,
                               rawOptions.replayResultContext);
  if (!expect(resultReplayOptions.replayPlayback == raw &&
                  resultReplayOptions.replayData == nullptr &&
                  resultReplayOptions.replayAnalysis == rawAnalysis &&
                  resultReplayOptions.replayResultContext ==
                      rawOptions.replayResultContext &&
                  resultReplayOptions.ownsChart &&
                  resultReplayOptions.gaugeType == GaugeType::ExHard &&
                  resultReplayOptions.playOption == "R-RANDOM" &&
                  resultReplayOptions.playOptionSeed == 17 &&
                  resultReplayOptions.playOption2 == "MIRROR" &&
                  resultReplayOptions.playOption2Seed == 29 &&
                  resultReplayOptions.doublePlayOption ==
                      replay::DoublePlayOption::Flip &&
                  resultReplayOptions.startingGaugeState ==
                      raw->setup.startingGaugeState,
              "raw replay result relaunch rebuilds startup from the retained "
              "BRD setup without promoting analysis to playback authority")) {
    return 1;
  }
  auto judgedResult = std::make_shared<JudgedPlaybackData>(replay);
  const StartOptions judgedResultOptions =
      replayResultStartOptions(judgedResult, rawOptions.replayResultContext);
  if (!expect(judgedResultOptions.replayData == judgedResult &&
                  judgedResultOptions.replayPlayback == nullptr &&
                  judgedResultOptions.replayResultContext ==
                      rawOptions.replayResultContext &&
                  judgedResultOptions.ownsChart &&
                  judgedResultOptions.playback.percent == 75,
              "judged and raw replay result relaunches share one startup "
              "contract")) {
    return 1;
  }
  if (!expect(effectiveNotePriorityModeAtPlayStart(
                  rawOptions, AppSettings::NotePriorityMode::Duration) ==
                  AppSettings::NotePriorityMode::Score,
              "raw playback candidate selection overrides current settings")) {
    return 1;
  }

  bms_parser::ChartMeta recalledResultMeta;
  recalledResultMeta.MD5 = std::string(32, 'a');
  recalledResultMeta.SHA256 = std::string(64, 'b');
  recalledResultMeta.KeyMode = 14;
  recalledResultMeta.IsDP = true;
  recalledResultMeta.RandomSeed = 999U;
  recalledResultMeta.RandomPrng = "stale-prng";
  recalledResultMeta.RandomValues = {9};

  ScoreProvenance recalledProvenance = ScoreProvenance::Legacy();
  recalledProvenance.ruleset = RulesetDescriptor::Current();
  recalledProvenance.stages = {{
      .chartMd5 = recalledResultMeta.MD5,
      .chartSha256 = recalledResultMeta.SHA256,
      .longNoteMode = 2,
      .chartRandomSeed = 123U,
      .chartRandomPrng = "std::mt19937_64",
      .chartRandomValues = {2, 1},
      .candidateSelection = gameplay::CandidateSelectionMode::Combo,
      .doublePlayOption = replay::DoublePlayOption::Flip,
  }};
  recalledProvenance.schemaVersion = ScoreProvenance::kSchemaVersion;
  recalledProvenance.gaugeType = GaugeType::Hard;
  recalledProvenance.gaugeProfile = GaugeProfile::Standard;
  recalledProvenance.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  recalledProvenance.gaugeAutoShiftLowerBound = GaugeType::Easy;
  recalledProvenance.player1 = {.option = "MIRROR", .seed = 17};
  recalledProvenance.player2 = {.option = "RANDOM", .seed = 29};
  recalledProvenance.assistOption = assist_options::kDrag;
  recalledProvenance.playback = {
      .percent = 75,
      .mode = audio::PlaybackMode::TimeStretch,
  };
  recalledProvenance.judgeWindowScalePercent = 90;
  recalledProvenance.startingGaugePercent = 37;
  recalledProvenance.clubMode = true;
  recalledProvenance.eligibility = ScoreEligibility::Modified;

  const JudgedPlaybackData recalledRetry = analysis::retrySourceFromProvenance(
      recalledResultMeta, recalledProvenance);
  const auto recalledDisplay =
      play_options::formatPlayModeDisplayLabel(recalledRetry);
  if (!expect(recalledRetry.setup.initialGaugeType == GaugeType::Hard &&
                  recalledRetry.setup.gaugeProfile == GaugeProfile::Standard &&
                  recalledRetry.setup.gaugeAutoShift ==
                      GaugeAutoShiftMode::BestClear &&
                  recalledRetry.setup.gaugeAutoShiftLowerBound ==
                      GaugeType::Easy &&
                  recalledRetry.setup.assistOption == assist_options::kDrag &&
                  recalledRetry.setup.doublePlayOption ==
                      replay::DoublePlayOption::Flip,
              "saved-result recall retains one complete setup including DP "
              "FLIP") ||
      !expect(recalledRetry.setup.playOption == "MIRROR" &&
                  recalledRetry.setup.playOptionSeed == 17 &&
                  recalledRetry.setup.playOption2 == "RANDOM" &&
                  recalledRetry.setup.playOption2Seed == 29 &&
                  recalledDisplay.mode == "FLIP + MIRROR #17 / RANDOM #29",
              "saved-result recall rebuilds its displayed and retried lane "
              "pattern from provenance") ||
      !expect(
          recalledRetry.setup.randomSeed == 123U &&
              recalledRetry.setup.randomPrng == "std::mt19937_64" &&
              recalledRetry.setup.randomValues == std::vector<int>({2, 1}) &&
              recalledRetry.chartMeta.RandomSeed == 123U &&
              recalledRetry.chartMeta.RandomPrng == "std::mt19937_64" &&
              recalledRetry.chartMeta.RandomValues == std::vector<int>({2, 1}),
          "Retry Same uses the persisted chart branch instead of the "
          "freshly parsed result chart") ||
      !expect(recalledRetry.setup.candidateSelection ==
                      gameplay::CandidateSelectionMode::Combo &&
                  recalledRetry.setup.playbackRatePercent ==
                      recalledProvenance.playback.percent &&
                  recalledRetry.setup.playbackMode ==
                      recalledProvenance.playback.mode &&
                  recalledRetry.setup.judgeWindowScalePercent == 90 &&
                  recalledRetry.context.startingGaugePercent == 37 &&
                  recalledRetry.setup.clubMode,
              "saved-result retry restores the persisted gameplay context")) {
    return 1;
  }

  ScoreProvenance schema4Provenance = recalledProvenance;
  schema4Provenance.schemaVersion = 4;
  schema4Provenance.stages.front().doublePlayOption.reset();
  const JudgedPlaybackData schema4Retry = analysis::retrySourceFromProvenance(
      recalledResultMeta, schema4Provenance);
  if (!expect(!schema4Provenance.stages.front().doublePlayOption.has_value() &&
                  schema4Retry.setup.doublePlayOption ==
                      replay::DoublePlayOption::Normal,
              "schema-v4 retry keeps DP provenance unknown and uses the "
              "documented runtime Normal fallback")) {
    return 1;
  }

  ScoreProvenance defaultHardProvenance = recalledProvenance;
  defaultHardProvenance.startingGaugePercent.reset();
  const JudgedPlaybackData defaultHardRetry =
      analysis::retrySourceFromProvenance(recalledResultMeta,
                                          defaultHardProvenance);
  StartOptions defaultHardRetryOptions;
  applyResultRetrySetupToStartOptions(defaultHardRetryOptions,
                                      defaultHardRetry);
  if (!expect(defaultHardRetry.setup.startingGaugePercent == 100.0F &&
                  !defaultHardRetryOptions.startingGaugePercent.has_value(),
              "a provenance-only Hard retry retains the ruleset default "
              "instead of forcing an explicit 20-percent start")) {
    return 1;
  }

  const auto samePatternAuthority =
      play_options::resultRetryPatternAuthority(recalledRetry, true);
  const auto newPatternAuthority =
      play_options::resultRetryPatternAuthority(recalledRetry, false);
  if (!expect(samePatternAuthority.chartRandomSeed == 123U &&
                  samePatternAuthority.chartRandomPrng == "std::mt19937_64" &&
                  samePatternAuthority.chartRandomValues ==
                      std::vector<int>({2, 1}) &&
                  samePatternAuthority.playOptionSeed == 17 &&
                  samePatternAuthority.playOption2Seed == 29,
              "Retry Same retains chart and MIRROR/RANDOM seed authority") ||
      !expect(
          !newPatternAuthority.chartRandomSeed.has_value() &&
              !newPatternAuthority.chartRandomPrng.has_value() &&
              !newPatternAuthority.chartRandomValues.has_value() &&
              !newPatternAuthority.playOptionSeed.has_value() &&
              !newPatternAuthority.playOption2Seed.has_value(),
          "Retry with a new pattern drops persisted chart and lane seeds")) {
    return 1;
  }

  StartOptions recalledRetryOptions;
  applyResultRetrySetupToStartOptions(recalledRetryOptions, recalledRetry);
  if (!expect(recalledRetryOptions.gaugeType == GaugeType::Hard &&
                  recalledRetryOptions.gaugeProfile == GaugeProfile::Standard &&
                  recalledRetryOptions.gaugeAutoShift ==
                      GaugeAutoShiftMode::BestClear &&
                  recalledRetryOptions.gaugeAutoShiftLowerBound ==
                      GaugeType::Easy &&
                  recalledRetryOptions.assistOption == assist_options::kDrag &&
                  recalledRetryOptions.replayCandidateSelection ==
                      gameplay::CandidateSelectionMode::Combo &&
                  recalledRetryOptions.playback ==
                      recalledProvenance.playback &&
                  recalledRetryOptions.judgeWindowScalePercent == 90 &&
                  recalledRetryOptions.startingGaugePercent == 37 &&
                  recalledRetryOptions.doublePlayOption ==
                      replay::DoublePlayOption::Flip &&
                  recalledRetryOptions.clubMode,
              "View Result retry startup applies the reconstructed provenance "
              "context")) {
    return 1;
  }

  auto unsupported = std::make_shared<replay::ReplayPlaybackData>();
  unsupported->setup.playbackRulesetId = "beatoraja";
  unsupported->setup.playbackRulesetRevision = 1;
  StartOptions unsupportedOptions;
  applyReplayPlaybackToStartOptions(unsupportedOptions, unsupported);
  const auto required = unsupportedOptions.requiredRulesetDescriptor;
  if (!expect(required.has_value() && required->id == "beatoraja" &&
                  required->version == 1 &&
                  !isSupportedRulesetDescriptor(*required),
              "raw replay preserves an unsupported recorded ruleset "
              "revision")) {
    return 1;
  }

  auto judgedLaneCover = std::make_shared<JudgedPlaybackData>();
  judgedLaneCover->setup.initialLaneCoverPercent = 64;
  judgedLaneCover->setup.laneCoverEnabled = true;
  StartOptions judgedLaneCoverOptions{.replayData = judgedLaneCover};
  if (!expect(replayInitialLaneCoverPercent(judgedLaneCoverOptions, 19) == 64,
              "judged replay watch starts from its retained lane cover")) {
    return 1;
  }
  judgedLaneCover->setup.laneCoverEnabled = false;
  if (!expect(replayInitialLaneCoverPercent(judgedLaneCoverOptions, 19) == 0,
              "a replay remembers a disabled nonzero lane cover")) {
    return 1;
  }
  judgedLaneCover->setup.initialLaneCoverPercent.reset();
  if (!expect(replayInitialLaneCoverPercent(judgedLaneCoverOptions, 19) == 19,
              "a provenance-only replay without lane-cover proof keeps the "
              "settings fallback")) {
    return 1;
  }

  return 0;
}
