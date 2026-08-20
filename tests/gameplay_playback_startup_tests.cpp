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

bool testSavedChartRandomBranchAuthority() {
  bms_parser::ChartMeta identity;
  identity.MD5 = std::string(32, 'b');
  identity.SHA256 = std::string(64, 'a');
  ScoreProvenance provenance;
  provenance.stages = {{.chartMd5 = identity.MD5,
                        .chartSha256 = identity.SHA256,
                        .chartRandomSeed = 42,
                        .chartRandomPrng =
                            bms_parser::Parser::RandomPrngId,
                        .chartRandomValues = {3, 1, 4}}};

  std::string diagnostic;
  const auto setup = score_provenance::savedChartRandomParseSetup(
      provenance, identity, diagnostic);
  if (!expect(setup.has_value() && diagnostic.empty() &&
                  setup->randomSeed == 42 &&
                  setup->randomPrng == bms_parser::Parser::RandomPrngId &&
                  setup->randomValues ==
                      std::optional<std::vector<int>>({3, 1, 4}),
              "saved result provenance restores the authored random branch")) {
    return false;
  }

  provenance.stages.push_back(provenance.stages.front());
  return expect(!score_provenance::savedChartRandomParseSetup(
                     provenance, identity, diagnostic)
                     .has_value() &&
                    !diagnostic.empty(),
                "ambiguous random-branch provenance fails closed");
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
                     ModernCourseFile,
             "eligible live course completion uses modern file persistence") &&
         expect(
             gameplay_startup::completedAttemptPersistenceRoute(false, false) ==
                 gameplay_startup::CompletedAttemptPersistenceRoute::None,
             "ineligible attempts do not acquire a persistence route");
}

bool testPracticeMenuSamePatternSeedSelection() {
  const std::optional<std::string> configuredOption = "RANDOM";
  const std::optional<long long> configuredSeed = 1234;
  return expect(practiceMenuSelectedOptionSeed(
                    true, configuredOption, configuredSeed, "RANDOM") ==
                    configuredSeed,
                "Same Pattern keeps an unchanged practice random seed") &&
         expect(!practiceMenuSelectedOptionSeed(
                     true, configuredOption, configuredSeed, "MIRROR")
                     .has_value(),
                "changing the practice menu option drops the old random seed") &&
         expect(!practiceMenuSelectedOptionSeed(
                     false, configuredOption, configuredSeed, "RANDOM")
                     .has_value(),
                "new-pattern practice retries do not reuse random seeds");
}

bool testBuiltInPracticeAppliesConfiguredViewerModifiers() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure;
  auto *timeline = new bms_parser::TimeLine(8, false);
  auto *note = new bms_parser::Note(1);
  timeline->Timing = 1'000'000;
  timeline->SetNote(0, note);
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  StartOptions options;
  options.playOption = "MIRROR";

  return expect(applyPracticePlayOptions(chart, options, "practice fallback"),
                "built-in practice accepts the configured viewer modifier") &&
         expect(timeline->Notes[0] == nullptr &&
                    timeline->Notes[6] == note && note->Lane == 6 &&
                    options.playOption == "MIRROR",
                "built-in practice applies the viewer modifier before play");
}

bool testCourseRetrySameUsesValidatedSetupWithoutReplayInput() {
  auto session = std::make_shared<CoursePlaySession>();
  session->autoKeySound = true;
  session->gaugeType = GaugeType::Hard;
  session->gaugeProfile = GaugeProfile::Standard;
  session->gaugeAutoShift = GaugeAutoShiftMode::Continue;
  session->gaugeAutoShiftLowerBound = GaugeType::Easy;
  session->ruleset = GameplayRuleset::Beatoraja;
  session->rulesetDescriptor =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);

  ReplayData setup;
  setup.chartMeta.LnMode = 2;
  setup.playOption = "RANDOM";
  setup.playOptionSeed = 123;
  setup.playOption2 = "MIRROR";
  setup.playOption2Seed = 456;
  setup.assistOption = assist_options::kOff;
  setup.gaugeAutoShiftLowerBound = GaugeType::Easy;
  setup.provenance = ScoreProvenance::Legacy();
  setup.provenance.gaugeType = GaugeType::Hard;
  setup.provenance.gaugeProfile = GaugeProfile::Standard;
  setup.provenance.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  setup.provenance.playback = {.percent = 75,
                               .mode = audio::PlaybackMode::PitchShift};
  setup.events.push_back({});

  const StartOptions options =
      makeCourseRetrySameStageStartOptions(session, setup);
  return expect(options.courseSession == session &&
                    options.replayData == nullptr && !options.autoPlay &&
                    options.autoKeySound &&
                    options.playOption == setup.playOption &&
                    options.playOptionSeed == setup.playOptionSeed &&
                    options.playOption2 == setup.playOption2 &&
                    options.playOption2Seed == setup.playOption2Seed,
                "course Retry Same applies validated setup without replay input") &&
         expect(options.playback == course_rules::kRequiredPlaybackRate &&
                    options.gaugeType == GaugeType::Hard &&
                    options.gaugeAutoShift == GaugeAutoShiftMode::Continue &&
                    options.gaugeAutoShiftLowerBound == GaugeType::Easy,
                "course Retry Same uses the shared course setup authority");
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

  if (!testLocalReplaySetupCapture() ||
      !testSavedChartRandomBranchAuthority() ||
      !testCompletionPersistenceRoutes() ||
      !testPracticeMenuSamePatternSeedSelection() ||
      !testBuiltInPracticeAppliesConfiguredViewerModifiers() ||
      !testCourseRetrySameUsesValidatedSetupWithoutReplayInput()) {
    return 1;
  }

  return 0;
}
