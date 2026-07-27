#include "scene/play/GamePlayStartup.h"
#include "scene/play/GamePlayStartOptions.h"
#include "replay/ReplayKeyMode.h"
#include "replay/ReplaySetupProvenance.h"
#include "replay/ReplaySetup.h"

#include <iostream>
#include <numeric>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

} // namespace

bool testLocalReplaySetupCapture() {
  for (const auto &layout : replay::kReplayKeyModeLayouts) {
    ScoreProvenance provenance;
    provenance.ruleset = RulesetDescriptor::Current();
    provenance.stages = {
        {.chartMd5 = std::string(32, 'b'),
         .chartSha256 = std::string(64, 'a'),
         .longNoteMode = 2,
         .chartRandomSeed = 42,
         .chartRandomPrng = bms_parser::Parser::RandomPrngId,
         .chartRandomValues = {3, 1, 4},
         .candidateSelection = gameplay::CandidateSelectionMode::Combo}};
    provenance.gaugeType = GaugeType::Hard;
    provenance.gaugeProfile = GaugeProfile::Standard;
    provenance.gaugeAutoShift = GaugeAutoShiftMode::SelectToUnder;
    provenance.gaugeAutoShiftLowerBound = GaugeType::Easy;
    provenance.player1 = {.option = "RANDOM", .seed = 123};
    provenance.player2 = {.option = layout.players == 2 ? "MIRROR" : "NORMAL"};
    provenance.assistOption = assist_options::kOff;
    provenance.clubMode = true;
    provenance.playback = {.percent = 75,
                           .mode = audio::PlaybackMode::PitchShift};
    provenance.judgeWindowScalePercent = 80;
    provenance.doublePlayFlip = layout.supportsDoublePlayFlip;

    std::vector<int> identity(
        static_cast<std::size_t>(layout.stockShuffleWidth));
    std::iota(identity.begin(), identity.end(), 0);
    replay::LocalReplaySetupFacts facts{
        .chart = {.md5 = std::string(32, 'b'),
                  .sha256 = std::string(64, 'a'),
                  .keyMode = layout.keyMode},
        .longNoteMode = 2,
        .hasUndefinedLongNotes = true,
        .player1LaneShufflePattern = identity,
        .player2LaneShufflePattern =
            layout.players == 2 ? std::optional<std::vector<int>>(identity)
                                : std::nullopt,
        .initialLaneCoverPercent = 37,
        .laneCoverEnabled = true,
    };
    std::string diagnostic;
    const auto setup =
        replay::captureLocalReplaySetup(facts, provenance, diagnostic);
    if (!expect(setup.has_value() && diagnostic.empty(),
                "supported key mode captures a canonical replay setup") ||
        !expect(setup->chart == facts.chart && setup->longNoteMode == 2 &&
                    setup->hasUndefinedLongNotes &&
                    setup->chartRandomValues == std::vector<int>({3, 1, 4}) &&
                    setup->player1.option == "RANDOM" &&
                    setup->player1.laneShufflePattern == identity &&
                    setup->doublePlayOption ==
                        (layout.supportsDoublePlayFlip
                             ? replay::DoublePlayOption::Flip
                             : replay::DoublePlayOption::Normal) &&
                    setup->initialGaugeType == GaugeType::Hard &&
                    setup->startingGaugePercent == 100.0F &&
                    setup->initialLaneCoverPercent == 37 &&
                    setup->laneCoverEnabled && setup->clubMode,
                "setup projection preserves every shared live-attempt fact")) {
      return false;
    }
  }
  return true;
}

bool testCompletionPersistenceRoutes() {
  return expect(
             gameplay_startup::completedAttemptPersistenceRoute(true, false) ==
                 gameplay_startup::CompletedAttemptPersistenceRoute::
                     ModernChartFile,
             "eligible chart completion uses modern file persistence") &&
         expect(
             gameplay_startup::completedAttemptPersistenceRoute(true, true) ==
                 gameplay_startup::CompletedAttemptPersistenceRoute::
                     LegacyCourse,
             "course completion remains on its existing route") &&
         expect(
             gameplay_startup::completedAttemptPersistenceRoute(false, false) ==
                 gameplay_startup::CompletedAttemptPersistenceRoute::None,
             "ineligible attempts do not acquire a persistence route");
}

int main() {
  ReplayData replay;
  replay.provenance.playback = {
      .percent = 75,
      .mode = audio::PlaybackMode::TimeStretch,
  };
  replay.provenance.clubMode = true;
  StartOptions replayOptions{.replayData = std::make_shared<ReplayData>(replay)};
  applyReplayProvenanceToStartOptions(replayOptions, replay);

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

  if (!testLocalReplaySetupCapture() || !testCompletionPersistenceRoutes()) {
    return 1;
  }

  return 0;
}
