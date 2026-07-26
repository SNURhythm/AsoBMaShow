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
  replay.context.playback = {
      .percent = 75,
      .mode = audio::PlaybackMode::TimeStretch,
  };
  replay.context.clubMode = true;
  StartOptions replayOptions{.replayData = std::make_shared<JudgedPlaybackData>(replay)};
  applyJudgedPlaybackContextToStartOptions(replayOptions, replay);

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

  auto raw = std::make_shared<replay::ReplayPlaybackData>();
  raw->setup.keyMode = 14;
  raw->setup.longNoteMode = 2;
  raw->setup.playOption = "R-RANDOM";
  raw->setup.playOptionSeed = 17;
  raw->setup.playOption2 = "MIRROR";
  raw->setup.playOption2Seed = 29;
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
  StartOptions rawOptions;
  applyReplayPlaybackToStartOptions(rawOptions, raw);
  if (!expect(rawOptions.replayPlayback == raw,
              "raw replay ownership is attached to play startup") ||
      !expect(rawOptions.replayData == nullptr,
              "raw playback does not manufacture judged replay data") ||
      !expect(rawOptions.gaugeType == GaugeType::ExHard &&
                  rawOptions.gaugeAutoShift == GaugeAutoShiftMode::BestClear &&
                  rawOptions.gaugeAutoShiftLowerBound == GaugeType::Easy,
              "raw playback restores its gauge setup") ||
      !expect(rawOptions.playOption == "R-RANDOM" &&
                  rawOptions.playOptionSeed == 17 &&
                  rawOptions.playOption2 == "MIRROR" &&
                  rawOptions.playOption2Seed == 29,
              "raw playback restores chart randomization") ||
      !expect(rawOptions.playback.percent == 125 &&
                  rawOptions.playback.mode == audio::PlaybackMode::TimeStretch &&
                  rawOptions.judgeWindowScalePercent == 90 &&
                  rawOptions.startingGaugePercent == 42 &&
                  rawOptions.clubMode,
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
  if (!expect(effectiveNotePriorityModeAtPlayStart(
                  rawOptions, AppSettings::NotePriorityMode::Duration) ==
                  AppSettings::NotePriorityMode::Score,
              "raw playback candidate selection overrides current settings")) {
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

  return 0;
}
