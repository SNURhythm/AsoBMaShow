#pragma once

#include "scene/MainMenuProfileSelections.h"
#include "replay/ReplaySetupAdapter.h"
#include "replay/CourseReplayPersistence.h"
#include <tuple>

struct PreparationChartRecord { bms_parser::ChartMeta meta; };

std::unique_ptr<bms_parser::Chart> prepareNextDpCourse(
    const std::shared_ptr<CoursePlaySession> &, const StartOptions &, bool);
void prepareMainMenuDpCourse(bms_parser::Chart &, const std::shared_ptr<CoursePlaySession> &);
bool prepareResultDpRetry(bms_parser::Chart &, const ReplayData &, const ScoreProvenance &,
                          bool, bool, bool, StartOptions &);

class PreparedSelectorFixture {
public:
  struct { AppSettings settings; } context;
  std::mutex preloadMutex_;
  std::filesystem::path preloadedPath_;
  std::unique_ptr<bms_parser::Chart> preloadedChart_;
  play_options::PlayOptionReplayInfo lastPlayInfo;
  int lastLnMode = 0;
  bool reusePreloadedChart(const PreparationChartRecord &, bms_parser::Chart *&,
                          play_options::PlayOptionReplayInfo &, int &);
  std::unique_ptr<bms_parser::Chart> prepareSelected(const PreparationChartRecord &);
  std::unique_ptr<bms_parser::Chart> prepareCourse(const std::shared_ptr<CoursePlaySession> &);
};

using PreparedLaneFact = std::tuple<int, long long, int, int, bool>;

class RetrySameResultFixture {
public:
  struct {
    ReplayRepository &replayRepository;
    struct { bool inputKeysoundEnabled = true; } settings;
  } context;
  struct {
    struct {
      std::shared_ptr<CoursePlaySession> session;
      bool savedResultBrowsing = false;
    } courseOptions;
    bool courseTransitionStarted = false;
    RhythmState resultState{nullptr, false};
    struct { std::nullptr_t returnScene = nullptr; } practiceOptions;
  } local;
  std::shared_ptr<CoursePlaySession> restored;
  std::unique_ptr<bms_parser::Chart> launchedChart;
  StartOptions launchedOptions;
  explicit RetrySameResultFixture(ReplayRepository &repository) : context{repository} {}
  auto *localSource() { return &local; }
  bool isCourseFinalResult() const { return true; }
  bool isCourseStageResult() const { return true; }
  bool recordCourseStageRestTime() { return true; }
  void showCourseResult() { require(false, "T3-R1 cleared prefix must continue"); }
  void showSavedCourseStage() { require(false, "T3-R1 not browsing saved stages"); }
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession>) {
    require(false, "T3-R1 retry is live, not Watch");
  }
  void startModernCourseRetrySameStage(std::shared_ptr<CoursePlaySession> session) {
    restored = std::move(session);
  }
  void startModernCourseRetrySame();
  void continueCourse();
};

std::vector<PreparedLaneFact> preparedLaneFacts(const bms_parser::Chart &chart) {
  std::vector<PreparedLaneFact> facts;
  for (const auto *measure : chart.Measures) {
    for (const auto *timeline : measure->TimeLines) {
      const auto append = [&](const auto &notes, int kind) {
        for (std::size_t lane = 0; lane < notes.size(); ++lane) {
          bms_parser::Note *note = notes[lane];
          if (!note) {
            continue;
          }
          require(note->Lane == static_cast<int>(lane),
                  "GAME02 every transformed note's Lane agrees with its owning slot");
          facts.emplace_back(note->IsLandmineNote() ? 2 : kind, timeline->Timing, note->Wav, static_cast<int>(lane),
              note->IsLongNote() && static_cast<bms_parser::LongNote *>(note)->IsTail());
        }
      };
      append(timeline->Notes, 0);
      append(timeline->InvisibleNotes, 1);
      append(timeline->LandmineNotes, 2);
    }
  }
  std::ranges::sort(facts);
  return facts;
}

std::vector<PreparedLaneFact> expectedDpLanes(std::vector<PreparedLaneFact> facts,
                                            bool flip, bool mirrorPlayer1) {
  for (auto &fact : facts) {
    int &lane = std::get<3>(fact);
    if (flip) {
      lane = lane < 8 ? lane + 8 : lane - 8;
    }
    if (mirrorPlayer1 && lane < 7) {
      lane = 6 - lane;
    }
  }
  std::ranges::sort(facts);
  return facts;
}

void testPartialCourseRetrySameRestoresSavedOptions(std::string_view scenario = "all") {
  for (const bool flip : {false, true}) {
    for (const bool mirrorPlayer1 : {false, true}) {
      for (const int capturedStages : {1, 2}) {
        if ((scenario == "flip" && (!flip || !mirrorPlayer1 || capturedStages != 1)) ||
            (scenario == "p2" && (flip || mirrorPlayer1 || capturedStages != 1))) {
          continue;
        }
        AbortTemporaryDirectory temporary;
        auto browsing = std::make_shared<CoursePlaySession>();
        std::vector<std::vector<PreparedLaneFact>> expectedFacts;
        replay::CourseReplayCapture capture;
        result_persistence::ModernCourseResultCapture resultCapture{
            .attemptId = "123e4567-e89b-42d3-a456-426614174000",
            .courseName = "Restored DP course",
            .constraintJson = "[]",
            .requestedPlayOption = mirrorPlayer1 ? "MIRROR" : "NORMAL",
            .initialGaugeType = GaugeType::Hard,
            .longNoteMode = 2,
            .playedAtUnixMillis = 1'700'000'000'000};
        PreparationContext playContext;
        replay::ReplayPlaybackCarryState carry;
        for (int stageIndex = 0; stageIndex < 2; ++stageIndex) {
          const auto path = temporary.path / ("restored-" + std::to_string(stageIndex) + ".bms");
          {
            std::ofstream file(path);
            file << "#PLAYER 3\n#TITLE Restored " << stageIndex << "\n#BPM 120\n#TOTAL 160\n#LNOBJ ZZ\n"
                    "#00011:0100ZZ00\n#00021:00020000\n#00016:00000300\n#00026:00000004\n"
                    "#00018:05000000\n#00029:00060000\n#00031:07000000\n#00041:00080000\n"
                    "#000D2:09000000\n#000E3:000A0000\n#00126:0B00ZZ00\n";
          }
          auto chart = parsePreparationFixture(path);
          browsing->entries.push_back({.meta = chart->Meta});
          auto expected = expectedDpLanes(preparedLaneFacts(*chart), flip, mirrorPlayer1);
          if (!mirrorPlayer1) {
            for (auto &fact : expected) {
              int &lane = std::get<3>(fact);
              if (lane >= 8 && lane < 15) {
                lane = 22 - lane;
              }
            }
            std::ranges::sort(expected);
          }
          expectedFacts.push_back(expected);
          StartOptions options;
          options.gaugeType = GaugeType::Hard;
          options.longNoteMode = 2;
          options.doublePlayFlip = flip;
          const auto applied = play_options::applySelectedPlayOptions(
              *chart, resultCapture.requestedPlayOption, mirrorPlayer1 ? "NORMAL" : "MIRROR", flip);
          options.playOption = applied.option;
          options.playOption2 = applied.option2;
          PreparedGamePlayScene scene(playContext, std::move(chart), options);
          resultCapture.entryFacts.push_back({.totalNotes = scene.chart->Meta.TotalNotes,
                                             .playLengthMicros = scene.chart->Meta.PlayLength});
          if (stageIndex >= capturedStages) {
            continue;
          }
          browsing->modernCourseChartPaths.push_back(path);
          RhythmState state(scene.chart, false, scene.rulesetPolicyBuild.policy->gauge);
          state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
          const bool aborted = capturedStages == 1;
          for (int noteIndex = 0; noteIndex < scene.chart->Meta.TotalNotes; ++noteIndex) {
            state.commitJudge(JudgeResult(aborted ? Poor : PGreat, 0));
          }
          if (aborted) {
            state.failUnfinishedAttempt();
          }
          std::string diagnostic;
          auto provisional = result_persistence::captureModernChartResult(
              resultCapture.attemptId, scene.chart->Meta, state, scene.attemptProvenance,
              2, resultCapture.playedAtUnixMillis, diagnostic);
          require(provisional.has_value(), diagnostic);
          const auto setup = replay::captureLocalReplaySetup(
              {.chart = {.md5 = scene.chart->Meta.MD5, .sha256 = scene.chart->Meta.SHA256, .keyMode = 14},
               .longNoteMode = 2, .hasUndefinedLongNotes = true}, scene.attemptProvenance, diagnostic);
          require(setup.has_value(), diagnostic);
          replay::ReplayPlaybackData playback{.setup = *setup};
          if (!aborted) {
            for (const auto *measure : scene.chart->Measures) {
              for (const auto *timeline : measure->TimeLines) {
                for (auto *note : timeline->Notes) {
                  if (!note || note->IsLandmineNote()) {
                    continue;
                  }
                  const auto control = replay::logicalControlForChartLane(
                      14, note->Lane, note->Lane == 7 || note->Lane == 15);
                  require(control.has_value(), "T3-R1 parsed note maps to replay control");
                  const auto *longNote = note->IsLongNote()
                                             ? static_cast<const bms_parser::LongNote *>(note) : nullptr;
                  if (longNote && longNote->IsTail() && replay::isDirectionalScratchControl(control->kind)) {
                    auto backspin = *control;
                    backspin.kind = replay::LogicalControlKind::ScratchCounterClockwise;
                    playback.input.push_back({.songTimeMicros = timeline->Timing, .control = backspin,
                                               .pressed = true});
                    playback.input.push_back({.songTimeMicros = timeline->Timing + 10'000, .control = backspin,
                                               .pressed = false});
                  }
                  playback.input.push_back({.songTimeMicros = timeline->Timing, .control = *control,
                                             .pressed = !longNote || !longNote->IsTail()});
                  if (!longNote) {
                    playback.input.push_back({.songTimeMicros = timeline->Timing + 10'000,
                                               .control = *control, .pressed = false});
                  }
                }
              }
            }
            std::ranges::stable_sort(playback.input, {}, &replay::InputTransition::songTimeMicros);
          }
          const replay::ReplayTimeBounds bounds{
              .completionSongTimeMicros = aborted ? 0 : 6'000'000,
              .aborted = aborted ? std::optional<bool>(true) : std::nullopt};
          auto materialized = replay::ReplayPlaybackMaterializer::materializeForConsumers(
              {.playback = playback, .timeBounds = bounds}, *provisional, *scene.chart, carry);
          require(materialized.judgedResult.has_value(), materialized.diagnostic);
          const auto simulatedResult = *materialized.judgedResult;
          materialized = replay::ReplayPlaybackMaterializer::materializeForConsumers(
              {.playback = playback, .timeBounds = bounds}, simulatedResult, *scene.chart, carry);
          require(materialized.playable() && materialized.judgedResult && materialized.finalGaugeState,
                  materialized.diagnostic);
          require(materialized.matched(), "T3-R1 saved replay facts match actual bounded simulation");
          const auto &judged = *materialized.judgedResult;
          require(aborted ? judged.score.clearType == kClearTypeFailedRank
                          : judged.score.finalGauge > 0,
                  "T3-R1 saved partial prefix is failed and full-prefix stages are legitimately cleared");
          resultCapture.stages.push_back({.stageIndex = stageIndex, .score = judged.score,
                                         .keyMode = judged.keyMode, .adoptedGaugeType = judged.adoptedGaugeType,
                                         .adoptedGaugeHistory = judged.adoptedGaugeHistory,
                                         .judgementTiming = judged.judgementTiming});
          carry = {.gauge = materialized.finalGaugeState, .combo = materialized.endingCombo,
                   .maximumCombo = judged.score.maxCombo};
          resultCapture.clearType = judged.score.clearType;
          capture.stages.push_back({.playback = std::move(playback), .timeBounds = bounds});
        }
        resultCapture.courseKey = course_identity::makeCourseKey(*browsing);
        std::string diagnostic;
        const auto saved = result_persistence::captureModernCourseResult(resultCapture, diagnostic);
        require(saved.has_value(), diagnostic);
        require(saved->provenance.eligibility == ScoreEligibility::Verified,
                "T3-R1 saved DP options are score-eligible before retry");
        capture.result = *saved;
        capture.constraints.longNoteMode = 2;
        const auto attempt = replay::captureCourseReplayAttempt(capture, diagnostic);
        require(attempt && attempt->replay, diagnostic);
        const auto repositoryPath = temporary.path / "replay.db";
        {
          ReplayRepository repository(repositoryPath);
          require(repository.EnsureSchema(), "T3-R1 temporary replay schema");
          replay::CourseReplayPersistence persistence(repository);
          const auto persisted = persistence.persist(*attempt);
          require(persisted.replayAttached && persisted.saved(), persisted.diagnostic);
        }
        ReplayRepository reopened(repositoryPath);
        browsing->modernCourseResultBrowsing = true;
        browsing->modernCourseRetrySameAllowed = true;
        browsing->modernCourseAttemptId = saved->attemptId;
        RetrySameResultFixture result(reopened);
        result.local.courseOptions.session = browsing;
        result.startModernCourseRetrySame();
        require(result.restored != nullptr, "T3-R1 actual verified factory and ResultScene restoration succeed");
        auto session = result.restored;
        const auto *retainedSecond = capturedStages == 2 ? session->preparedCourseCharts[1].get() : nullptr;
        const auto *firstSetup = session->courseRetrySameStageSetup(0);
        require(firstSetup != nullptr, "T3-R1 saved prefix retains authenticated stage setup");
        auto first = session->takePreparedCourseChart(0);
        require(first && preparedLaneFacts(*first) == expectedFacts[0],
                "T3-R1 recorded prefix is transformed exactly once");
        PreparedGamePlayScene firstScene(playContext, std::move(first),
                                         makeCourseRetrySameStageStartOptions(session, *firstSetup));
        require(preparedLaneFacts(*firstScene.chart) == expectedFacts[0],
                "T3-R1 live prefix launch never reapplies saved lane preparation");
        RhythmState cleared(firstScene.chart, false, firstScene.rulesetPolicyBuild.policy->gauge);
        cleared.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
        for (int noteIndex = 0; noteIndex < firstScene.chart->Meta.TotalNotes; ++noteIndex) {
          cleared.commitJudge(JudgeResult(PGreat, 0));
        }
        require(cleared.currentGauge > 0 && cleared.getScore() == firstScene.chart->Meta.TotalNotes * 2,
                "T3-R1 the retried retained stage finishes successfully before actual continuation");
        session->recordResult(firstScene.chart->Meta, cleared);
        session->recordStageProvenance(0, firstScene.attemptProvenance);
        session->carriedGauge = cleared.gaugeSnapshot();
        session->carriedCombo = cleared.combo;
        session->maxCombo = cleared.maxCombo;
        result.local.courseOptions.session = session;
        result.local.resultState = cleared;
        result.local.courseTransitionStarted = false;
        result.continueCourse();
        require(result.launchedChart && session->currentIndex == 1 &&
                    preparedLaneFacts(*result.launchedChart) == expectedFacts[1],
                "T3-R1 actual transition beyond saved prefix preserves DP flip and asymmetric P2 geometry");
        require(retainedSecond == nullptr || result.launchedChart.get() == retainedSecond,
                "T3-R1 full-prefix control consumes its original prepared second chart without replacement");
        require(session->doublePlayFlip == flip &&
                    session->requestedPlayOption2 == (mirrorPlayer1 ? "NORMAL" : "MIRROR"),
                "T3-R1 factory restores course-wide options only from authenticated saved facts");
        PreparedGamePlayScene secondScene(playContext, std::move(result.launchedChart), result.launchedOptions);
        session->recordStageProvenance(1, secondScene.attemptProvenance);
        require(secondScene.attemptProvenance.doublePlayFlip == flip &&
                    session->aggregateProvenance().eligibility == ScoreEligibility::Verified,
                "T3-R1 restored course retains truthful provenance without a spurious Modified downgrade");
      }
    }
  }
  std::cout << "T3-R1 actual verified partial/full course Retry Same tests passed\n";
}

void testActualDoublePlayFlipPreparation(std::string_view pathToTest) {
  AbortTemporaryDirectory temporary;
  const auto path = temporary.path / "asymmetric-dp.bms";
  {
    std::ofstream file(path);
    file << "#PLAYER 3\n#TITLE DP preparation\n#BPM 120\n#TOTAL 160\n#LNOBJ ZZ\n"
            "#00011:0100ZZ00\n#00021:00020000\n#00016:00000300\n#00026:00000004\n"
            "#00018:05000000\n#00029:00060000\n#00031:07000000\n#00041:00080000\n"
            "#000D2:09000000\n#000E3:000A0000\n#00126:0B00ZZ00\n";
  }
  auto original = parsePreparationFixture(path);
  require(original->Meta.IsDP && original->Meta.KeyMode == 14,
          "GAME02 asymmetric fixture is real 14-key DP");
  const auto originalFacts = preparedLaneFacts(*original);
  require(std::ranges::count_if(originalFacts, [](const auto &fact) { return std::get<0>(fact) == 1; }) == 2 &&
              std::ranges::count_if(originalFacts, [](const auto &fact) { return std::get<0>(fact) == 2; }) == 2 &&
              std::ranges::count_if(originalFacts, [](const auto &fact) { return std::get<4>(fact); }) == 2,
          "GAME02 fixture includes both-side invisible notes, mines, LN ends and backspin");
  PreparationContext playContext;
  for (const bool flip : {false, true}) {
    for (const bool mirror : {false, true}) {
      PreparedSelectorFixture selector;
      selector.context.settings.skinDoublePlayOption = flip ? 1 : 0;
      selector.context.settings.selectedPlayOption = mirror ? "MIRROR" : "NORMAL";
      selector.context.settings.skinPlayer2RandomOption = 0;
      selector.context.settings.selectedLnMode = "CN";
      StartOptions options;
      options.doublePlayFlip = flip;
      options.longNoteMode = 2;
      options.playOption = mirror ? "MIRROR" : "NORMAL";
      options.playOption2 = "NORMAL";
      std::unique_ptr<bms_parser::Chart> prepared;
      if (pathToTest == "selected") {
        prepared = selector.prepareSelected({.meta = original->Meta});
      } else if (pathToTest == "preloaded") {
        selector.preloadedChart_ = parsePreparationFixture(path);
        selector.preloadedPath_ = path;
        bms_parser::Chart *raw = nullptr;
        require(selector.reusePreloadedChart({.meta = original->Meta}, raw,
                                             selector.lastPlayInfo, selector.lastLnMode),
                "GAME02 actual selector consumes a matching raw preloaded chart");
        prepared.reset(raw);
        require(!selector.reusePreloadedChart({.meta = original->Meta}, raw,
                                              selector.lastPlayInfo, selector.lastLnMode),
                "GAME02 consumed preloaded chart cannot be transformed or launched twice");
      } else if (pathToTest == "course" || pathToTest == "course-next" ||
                 pathToTest == "course-in-game" || pathToTest == "course-menu") {
        auto session = std::make_shared<CoursePlaySession>();
        session->entries.push_back({.meta = original->Meta});
        session->longNoteMode = 2;
        session->doublePlayFlip = flip;
        session->requestedPlayOption = *options.playOption;
        if (pathToTest == "course") {
          prepared = selector.prepareCourse(session);
        } else if (pathToTest == "course-menu") {
          prepared = parsePreparationFixture(path);
          prepareMainMenuDpCourse(*prepared, session);
        } else {
          prepared = prepareNextDpCourse(session, options, pathToTest == "course-next");
          require(preparedLaneFacts(*prepared) == expectedDpLanes(originalFacts, flip, mirror),
                  "GAME02 next course stage applies the selected geometry");
          session->preparedCourseCharts.push_back(std::move(prepared));
          session->courseRetrySameData = std::make_shared<CourseReplayData>();
          ReplayData retained;
          retained.playOption = options.playOption;
          retained.playOption2 = options.playOption2;
          retained.provenance.doublePlayFlip = flip;
          session->courseRetrySameData->stages.push_back({.replay = retained});
          prepared = prepareNextDpCourse(session, options, pathToTest == "course-next");
          require(!session->hasPreparedCourseChart(0),
                  "GAME02 already-prepared same-pattern course stage is consumed once");
        }
      } else if (pathToTest == "result-retry") {
        ReplayData retrySource;
        retrySource.playOption = options.playOption;
        retrySource.playOption2 = options.playOption2;
        ScoreProvenance provenance;
        provenance.doublePlayFlip = flip;
        for (const bool samePattern : {false, true}) {
          for (const bool reuse : {false, true}) {
            if (reuse && !samePattern) {
              continue;
            }
            auto retry = parsePreparationFixture(path);
            if (reuse) {
              play_options::applySelectedPlayOptions(*retry, *options.playOption, "NORMAL", flip);
            }
            StartOptions retryOptions;
            require(prepareResultDpRetry(*retry, retrySource, provenance, samePattern,
                                         reuse, false, retryOptions),
                    "GAME02 actual result retry option preparation succeeds");
            require(retryOptions.doublePlayFlip == flip &&
                        preparedLaneFacts(*retry) == expectedDpLanes(originalFacts, flip, mirror),
                    "GAME02 actual result new/same/reused retry preserves DP flag and geometry");
          }
        }
        auto pristine = parsePreparationFixture(path);
        StartOptions deferredOptions;
        require(prepareResultDpRetry(*pristine, retrySource, provenance, true, false,
                                     true, deferredOptions) &&
                    deferredOptions.doublePlayFlip == flip &&
                    preparedLaneFacts(*pristine) == originalFacts,
                "GAME02 session-backed result retry defers geometry to the practice menu");
        continue;
      } else if (pathToTest == "replay") {
        auto authored = parsePreparationFixture(path);
        applyEffectiveLongNoteModeToChart(*authored, 2);
        const auto policy = buildGameplayRulesetPolicyAtPlayStart(options, *authored,
            AppSettings::NotePriorityMode::Lowest);
        require(policy.built(), policy.diagnostic);
        const auto provenance = captureScoreProvenanceAtPlayStart(options, authored->Meta, *policy.policy);
        std::string diagnostic;
        const auto setup = replay::captureLocalReplaySetup(
            {.chart = {.md5 = authored->Meta.MD5, .sha256 = authored->Meta.SHA256, .keyMode = 14},
             .longNoteMode = 2, .hasUndefinedLongNotes = true}, provenance, diagnostic);
        require(setup.has_value(), diagnostic);
        replay::BeatorajaReplayCodec codec;
        const replay::ReplayChartDocument document{
            .playback = {.setup = *setup},
            .timeBounds = {.completionSongTimeMicros = 10'000'000}};
        const auto encoded = codec.encodeChart(document, 1'700'000'000'000, diagnostic);
        require(encoded.has_value(), diagnostic);
        const auto decoded = codec.decode(*encoded, {.stageKeyModes = {14}});
        require(decoded.chart.has_value(), decoded.diagnostic);
        const auto runtime = replay::makeReplayDataFromSetup(decoded.chart->playback.setup,
            provenance, original->Meta, diagnostic);
        require(runtime.has_value(), diagnostic);
        std::atomic_bool cancelled = false;
        prepared = play_options::prepareReplayChart(path, *runtime, cancelled);
      } else if (pathToTest == "retry") {
        StartOptions retryOptions;
        std::atomic_bool cancelled = false;
        require(prepareRetryChart(original->Meta, options, prepared, retryOptions, cancelled),
                "GAME02 actual in-game fresh retry prepares requested options");
        options = retryOptions;
      } else if (pathToTest == "practice" || pathToTest == "fallback") {
        PreparedGamePlayScene scene(playContext, parsePreparationFixture(path), options);
        if (pathToTest == "practice") {
          const practice::SkinMenuAttemptPlan attempt{
              .startMicros = 0, .endMicros = 10'000'000, .total = 160,
              .random1P = mirror ? 1 : 0, .doublePlayFlip = flip};
          require(scene.preparePracticeAttemptFromMenu(attempt, "DP fixture"),
                  "GAME02 actual skin practice preparation succeeds");
        } else {
          require(scene.prepareBuiltInFallback(), "GAME02 actual built-in practice fallback succeeds");
        }
        require(preparedLaneFacts(*scene.chart) == expectedDpLanes(originalFacts, flip, mirror),
                "GAME02 skin/fallback practice applies requested DP flip exactly once before lane modifiers");
        scene.resetNotesForSameAttempt();
        require(preparedLaneFacts(*scene.chart) == expectedDpLanes(originalFacts, flip, mirror),
                "GAME02 in-place practice reset retains the prepared lane geometry");
        continue;
      }
      require(prepared != nullptr, "GAME02 actual preparation returns a chart");
      require(preparedLaneFacts(*prepared) == expectedDpLanes(originalFacts, flip, mirror),
              "GAME02 requested DP flip swaps every note family exactly once before player modifiers");
      PreparedGamePlayScene scene(playContext, std::move(prepared), options);
      require(scene.attemptProvenance.doublePlayFlip == flip &&
                  preparedLaneFacts(*scene.chart) == expectedDpLanes(originalFacts, flip, mirror),
              "GAME02 immutable launch provenance truthfully describes the played DP geometry");
      StartOptions reusedOptions = options;
      reusedOptions.ownsChart = false;
      PreparedGamePlayScene reused(playContext, scene.chart, reusedOptions);
      reused.resetNotesForSameAttempt();
      require(preparedLaneFacts(*scene.chart) == expectedDpLanes(originalFacts, flip, mirror),
              "GAME02 reused-chart launch/reset does not reapply DP flip");
    }
  }
  std::cout << "GAME02 " << pathToTest << " actual DP flip tests passed\n";
}
