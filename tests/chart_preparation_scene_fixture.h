#pragma once

struct PreparationContext {
  int inputProfile = 0;
  struct {
    AppSettings::NotePriorityMode notePriorityMode = AppSettings::NotePriorityMode::Lowest;
  } settings;
};

class PreparationSceneBase {
public:
  explicit PreparationSceneBase(PreparationContext &context) : context(context) {}
  PreparationContext &context;
};

StartOptions resolvePreparationInputDevices(StartOptions options, int, int) {
  if (options.practiceSession) {
    applyPracticeConfigurationToStartOptions(options, options.practiceSession->configuration());
  }
  options.inputDeviceCategories = {InputDeviceCategory::Keyboard};
  return options;
}

Judge presentationJudgeForPolicy(const gameplay::GameplayPolicyBuildOutcome &, int);
std::optional<NoteTimeRange> practiceAllowedNoteRange(const StartOptions &);
bool prepareRetryChart(const bms_parser::ChartMeta &, const StartOptions &,
                       std::unique_ptr<bms_parser::Chart> &, StartOptions &,
                       std::atomic_bool &);

struct PreparationVisualModel {};
PreparationVisualModel buildPreparationVisualModel(const bms_parser::Chart &, int) { return {}; }
struct PreparationVisualStore {
  void resetModel(const PreparationVisualModel &) {}
};
struct PreparationPresentation {
  void updateJudgeTimingWindows(const std::map<Judgement, std::pair<long long, long long>> &) {}
};
struct PreparationLaneController {
  template <typename... Arguments>
  explicit PreparationLaneController(Arguments &&...) {}
};

class PreparedGamePlayScene : public PreparationSceneBase {
public:
  std::unique_ptr<bms_parser::Chart> ownedChart;
  bms_parser::Chart *chart;
  StartOptions options;
  gameplay::GameplayPolicyBuildOutcome rulesetPolicyBuild;
  Judge judge;
  long long latePoorTiming = 0;
  ScoreProvenance attemptProvenance = ScoreProvenance::Legacy();
  PreparationVisualModel playfieldChartVisualModel;
  PreparationVisualStore *playfieldVisualStateStore = nullptr;
  PreparationPresentation *builtInPresentation = nullptr;
  std::unique_ptr<PreparationLaneController> ownedLaneInputController;
  PreparationLaneController *laneInputController = nullptr;
  std::nullptr_t presentationEventFanout = nullptr;
  std::unordered_map<int, bool> lanePressed;
  std::unique_ptr<RhythmState> ownedState;
  RhythmState *state = nullptr;

  PreparedGamePlayScene(PreparationContext &, bms_parser::Chart *, StartOptions);
  PreparedGamePlayScene(PreparationContext &, std::unique_ptr<bms_parser::Chart>, StartOptions);
  bool preparePracticeAttemptFromMenu(const practice::SkinMenuAttemptPlan &, std::string_view);
  bool prepareBuiltInFallback();
  void resetNotesForSameAttempt();
  void initializePlayfieldVisualNoteSources() {}
  std::optional<NoteTimeRange> practiceNoteRange() const { return practiceAllowedNoteRange(options); }
  void showPlaybackInitializationFailure(std::string_view diagnostic) { require(false, diagnostic); }
};

struct PreparedViewerFixture {
  struct { bms_parser::ChartMeta meta; } record;
  struct Status { void setText(std::string_view) {} };
  Status *statusText = nullptr;
  std::optional<std::string> viewerPlayOption = "NORMAL";
  std::optional<long long> viewerPlayOptionSeed = 123;
  std::optional<std::string> viewerPlayOption2;
  std::optional<long long> viewerPlayOption2Seed;
  std::optional<std::string> viewerLaneOrderSummary;
  bool applyViewerPlayOptions(bms_parser::Chart &, const char *);
  std::unique_ptr<bms_parser::Chart> freshLaunchChart(bool autoPlay);
};

std::unique_ptr<bms_parser::Chart> parsePreparationFixture(const std::filesystem::path &path) {
  std::atomic_bool cancelled = false;
  auto chart = play_options::parseChart(path, cancelled, "preparation fixture");
  require(chart && !cancelled, "COR05 bounded undefined-LN fixture parses");
  return chart;
}

void requirePreparedPolicy(const PreparedGamePlayScene &scene, int expectedNotes) {
  require(scene.rulesetPolicyBuild.built(), scene.rulesetPolicyBuild.diagnostic);
  require(scene.chart->Meta.TotalNotes == expectedNotes,
          "COR05 actual preparation normalizes effective note counts before play");
  require(scene.rulesetPolicyBuild.policy->gauge.totalNotes == expectedNotes,
          "COR05 actual constructor/practice policy uses the prepared effective note count");
  require(scene.attemptProvenance.stages.size() == 1 &&
              scene.attemptProvenance.stages[0].totalNotes == expectedNotes,
          "COR05 actual constructor/practice provenance agrees with fixed gauge rules");
  bms_parser::ChartMeta expectedMeta = scene.chart->Meta;
  expectedMeta.TotalNotes = expectedNotes;
  const auto expectedRules = compileGameplayGaugeRules(
      scene.options.ruleset, expectedMeta, scene.options.gaugeProfile);
  RhythmState actual(scene.chart, false, scene.rulesetPolicyBuild.policy->gauge);
  RhythmState expected(scene.chart, false, expectedRules);
  actual.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  expected.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None);
  for (const auto judgement : {PGreat, Bad, Great, Good}) {
    actual.commitJudge(JudgeResult(judgement, 0));
    expected.commitJudge(JudgeResult(judgement, 0));
    require(actual.currentGauge == expected.currentGauge &&
                actual.gaugeSnapshot().gaugeValues == expected.gaugeSnapshot().gaugeValues &&
                actual.getScore() == expected.getScore(),
            "COR05 identical judgments have identical gauge and score effects");
  }
}

void testActualChartPreparationOrdering(std::string_view pathToTest) {
  AbortTemporaryDirectory temporary;
  const auto path = temporary.path / "undefined-ln.bms";
  {
    std::ofstream file(path);
    file << "#PLAYER 1\n#TITLE Preparation\n#BPM 120\n#TOTAL 120\n"
            "#LNOBJ ZZ\n#00011:0100ZZ00\n#00012:00000001\n"
            "#RANDOM 2\n#IF 1\n#00013:00000000\n#ENDIF\n"
            "#IF 2\n#00014:00000000\n#ENDIF\n#ENDRANDOM\n";
  }
  PreparationContext context;
  for (const int selectedMode : {2, 3}) {
    StartOptions options;
    options.longNoteMode = selectedMode;
    if (pathToTest == "constructors") {
      auto borrowed = parsePreparationFixture(path);
      PreparedGamePlayScene raw(context, borrowed.get(), options);
      requirePreparedPolicy(raw, 3);
      PreparedGamePlayScene owned(context, parsePreparationFixture(path), options);
      requirePreparedPolicy(owned, 3);
      PreparedGamePlayScene reused(context, borrowed.get(), options);
      reused.resetNotesForSameAttempt();
      requirePreparedPolicy(reused, 3);
    }
    for (const std::string option : {"NORMAL", "MIRROR", "RANDOM"}) {
      options.playOption = option;
      options.playOptionSeed = 123;
      if (pathToTest == "in-game-retry") {
        auto initial = parsePreparationFixture(path);
        applyEffectiveLongNoteModeToChart(*initial, selectedMode);
        std::unique_ptr<bms_parser::Chart> retry;
        StartOptions retryOptions;
        std::atomic_bool cancelled = false;
        require(prepareRetryChart(initial->Meta, options, retry, retryOptions, cancelled),
                "COR05 actual in-game retry helper prepares a new pattern");
        PreparedGamePlayScene scene(context, std::move(retry), retryOptions);
        requirePreparedPolicy(scene, 3);
        scene.resetNotesForSameAttempt();
        requirePreparedPolicy(scene, 3);
      }
      if (pathToTest == "retry") {
        auto initial = parsePreparationFixture(path);
        applyEffectiveLongNoteModeToChart(*initial, selectedMode);
        ReplayData retry;
        retry.chartMeta = initial->Meta;
        retry.randomSeed = initial->Meta.RandomSeed;
        retry.randomPrng = initial->Meta.RandomPrng;
        retry.randomValues = initial->Meta.RandomValues;
        retry.playOption = option;
        retry.playOptionSeed = 123;
        for (const bool samePattern : {false, true}) {
          std::atomic_bool cancelled = false;
          auto chart = play_options::parseChartForRetry(retry, retry.chartMeta, cancelled, samePattern);
          require(chart && chart->Meta.TotalNotes == 3,
                  "COR05 fresh retry helper returns effective CN/HCN counts, not only LnMode");
          if (samePattern) {
            require(chart->Meta.RandomValues == retry.randomValues && chart->Meta.RandomSeed == retry.randomSeed,
                    "COR05 same-pattern reparse preserves the chart random branch and seed");
          }
          std::optional<std::string> applied;
          std::optional<long long> seed;
          require(play_options::applyPlayOptionModifier(*chart, option, 123, 0, applied, seed),
                  "COR05 actual retry lane modifier succeeds");
          require(chart->Meta.TotalNotes == 3,
                  "COR05 lane modifiers cannot revert an effective CN/HCN count");
          PreparedGamePlayScene scene(context, std::move(chart), options);
          requirePreparedPolicy(scene, 3);
        }
      }
      if (pathToTest == "practice" || pathToTest == "skin-practice") {
        auto source = parsePreparationFixture(path);
        applyEffectiveLongNoteModeToChart(*source, selectedMode);
        PreparedGamePlayScene scene(context, std::move(source), options);
        const auto &timelines = scene.chart->Measures.back()->TimeLines;
        long long tailTime = 0;
        for (const auto *measure : scene.chart->Measures) {
          for (const auto *timeline : measure->TimeLines) {
            for (auto *note : timeline->Notes) {
              if (note && note->IsLongNote() && static_cast<bms_parser::LongNote *>(note)->IsTail()) {
                tailTime = timeline->Timing;
              }
            }
          }
        }
        require(tailTime > 0 && !timelines.empty(), "COR05 fixture identifies bounded LN tail timing");
        const practice::SkinMenuAttemptPlan attempt{
            .startMicros = 0, .endMicros = tailTime + 1,
            .gaugeType = GaugeType::Normal, .judgeRank = 100, .total = 120,
            .random1P = option == "NORMAL" ? 0 : option == "MIRROR" ? 1 : 2};
        require(scene.preparePracticeAttemptFromMenu(attempt, "fixture practice"),
                "COR05 actual skin practice range and option preparation succeeds");
        requirePreparedPolicy(scene, 2);
        if (pathToTest == "skin-practice") {
          continue;
        }
        PreparedGamePlayScene fallback(context, parsePreparationFixture(path), options);
        require(fallback.prepareBuiltInFallback(), "COR05 actual built-in fallback preparation succeeds");
        requirePreparedPolicy(fallback, 3);
      }
      if (pathToTest == "viewer") {
        for (const bool autoPlay : {false, true}) {
          PreparedViewerFixture viewer;
          viewer.record.meta = parsePreparationFixture(path)->Meta;
          viewer.viewerPlayOption = option;
          auto chart = viewer.freshLaunchChart(autoPlay);
          require(chart != nullptr, "COR05 actual viewer fresh parse/option path succeeds");
          options.autoPlay = autoPlay;
          PreparedGamePlayScene scene(context, std::move(chart), options);
          requirePreparedPolicy(scene, 3);
          if (!autoPlay) {
            require(scene.prepareBuiltInFallback(), "COR05 viewer manual practice uses actual fallback");
            requirePreparedPolicy(scene, 3);
          }
        }
      }
    }
  }
  std::cout << "COR05 " << pathToTest << " actual preparation tests passed\n";
}
