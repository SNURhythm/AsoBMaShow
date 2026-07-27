#include "../src/ResultRecallBuilder.h"
#include "../src/ResultPersistenceModel.h"
#include "../src/PlayOptionUtils.h"
#include "../src/replay/LegacyReplayIdentity.h"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

namespace {

constexpr const char *kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

ScoreProvenance verifiedProvenance(GaugeType gauge = GaugeType::Hard) {
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  provenance.ruleset = RulesetDescriptor::For(GameplayRuleset::LR2);
  provenance.gaugeType = gauge;
  provenance.eligibility = ScoreEligibility::Verified;
  return provenance;
}

result_persistence::PersistedChartResult validResult(
    int resultId = 41, std::string path = "charts/recall.bms") {
  result_persistence::PersistedChartResult result{
      .resultId = resultId,
      .attemptId = kAttemptId,
      .score =
          {
              .chartPath = std::move(path),
              .chartMd5 = "0123456789abcdef0123456789abcdef",
              .chartSha256 =
                  "0123456789abcdef0123456789abcdef"
                  "0123456789abcdef0123456789abcdef",
              .chartTitle = "Recall Title",
              .chartArtist = "Recall Artist",
              .longNoteMode = 2,
              .score = 3,
              .maxScore = 4,
              .maxCombo = 2,
              .comboBreak = 0,
              .pGreat = 1,
              .great = 1,
              .fast = 1,
              .finalGauge = 93.25F,
              .clearType = kClearTypeFullComboRank,
              .provenance = verifiedProvenance(),
      },
      .keyMode = 7,
      .adoptedGaugeType = GaugeType::Easy,
      .adoptedGaugeHistory = {20.0F, 61.5F, 93.25F},
      .playedAtUnixMillis = 1784420645000LL,
  };
  result.judgementTiming.emplace();
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 0};
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

result_recall::ResultChartLoader chartLoader(int *calls = nullptr) {
  return [calls](const result_persistence::PersistedChartResult &result,
                 std::atomic_bool &) {
    if (calls != nullptr) {
      ++*calls;
    }
    auto chart = std::make_unique<bms_parser::Chart>();
    chart->Meta.BmsPath = result.score.chartPath;
    chart->Meta.MD5 = result.score.chartMd5;
    chart->Meta.SHA256 = result.score.chartSha256;
    chart->Meta.Title = "Changed on disk";
    chart->Meta.Artist = "Changed on disk";
    chart->Meta.KeyMode = 14;
    chart->Meta.TotalNotes = 999;
    chart->Meta.Banner = "banner.png";
    chart->Meta.StageFile = "stage.png";
    return chart;
  };
}

void assertStateMatches(const RhythmState &state,
                        const result_persistence::ChartScoreWrite &score,
                        const std::vector<float> &gaugeHistory,
                        const result_persistence::ChartJudgementTiming &timing) {
  assert(state.getScore() == score.score);
  assert(state.maxCombo == score.maxCombo);
  assert(state.comboBreak == score.comboBreak);
  assert(state.judgeCount.at(PGreat) == score.pGreat);
  assert(state.judgeCount.at(Great) == score.great);
  assert(state.judgeCount.at(Good) == score.good);
  assert(state.judgeCount.at(Bad) == score.bad);
  assert(state.judgeCount.at(Poor) == score.poor);
  assert(state.judgeCount.at(Kpoor) == score.kPoor);
  assert(state.fastCount == score.fast);
  assert(state.slowCount == score.slow);
  assert(state.currentGauge == score.finalGauge);
  assert(state.gaugeHistory == gaugeHistory);
  assert(state.judgementFastSlowCount.at(PGreat) ==
         timing.byJudgement[PGreat]);
  assert(state.getClearTypeRank() == score.clearType);
}

void testChartRecallUsesPersistedFactsOnly() {
  auto persisted = validResult();
  const auto expected = persisted;
  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(persisted), cancelled, chartLoader(&calls));

  assert(outcome.value.has_value());
  assert(outcome.diagnostic.empty());
  assert(calls == 1);
  const auto &recalled = *outcome.value;
  assert(recalled.result == expected);
  assert(recalled.chart != nullptr);
  assert(recalled.chart->Meta.Banner == "banner.png");
  assert(recalled.chart->Meta.StageFile == "stage.png");
  assert(recalled.chart->Meta.BmsPath == expected.score.chartPath);
  assert(recalled.chart->Meta.Title == expected.score.chartTitle);
  assert(recalled.chart->Meta.Artist == expected.score.chartArtist);
  assert(recalled.chart->Meta.MD5 == expected.score.chartMd5);
  assert(recalled.chart->Meta.SHA256 == expected.score.chartSha256);
  assert(recalled.chart->Meta.KeyMode == expected.keyMode);
  assert(recalled.chart->Meta.TotalNotes == expected.score.maxScore / 2);
  assert(recalled.chart->Meta.LnMode == expected.score.longNoteMode);
  assertStateMatches(recalled.state, expected.score,
                     expected.adoptedGaugeHistory,
                     *expected.judgementTiming);
  assert(recalled.state.gaugeType == expected.adoptedGaugeType);
  assert(recalled.state.selectedGaugeType == expected.adoptedGaugeType);
  assert(recalled.state.gaugeHistoryFor(expected.adoptedGaugeType) ==
         expected.adoptedGaugeHistory);
  assert(recalled.result.playedAtUnixMillis == 1784420645000LL);
  assert(recalled.result.score.provenance == expected.score.provenance);
}

void testChartRecallPreservesResolvedRuntimePathForRetry() {
  auto persisted = validResult();
  persisted.score.chartPath = "Documents/BMS/portable-recall.bms";
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  const std::filesystem::path resolvedPath =
      std::filesystem::path("/current-profile/Documents/BMS") /
      "portable-recall.bms";

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      persisted, cancelled,
      [resolvedPath](const result_persistence::PersistedChartResult &result,
                     std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = resolvedPath;
        chart->Meta.MD5 = result.score.chartMd5;
        chart->Meta.SHA256 = result.score.chartSha256;
        return chart;
      });

  assert(outcome.value.has_value());
  assert(outcome.value->result.score.chartPath == persisted.score.chartPath);
  assert(outcome.value->chart->Meta.BmsPath == resolvedPath);
}

void testChartRecallRejectsInvalidResultBeforeLoadingAssets() {
  auto persisted = validResult();
  persisted.score.score = 1;
  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(persisted), cancelled, chartLoader(&calls));
  assert(!outcome.value.has_value());
  assert(outcome.diagnostic ==
         "saved chart result is invalid: score range is inconsistent with "
         "result counters");
  assert(calls == 0);
}

void testChartRecallDoesNotPublishMissingOrCancelledAssets() {
  std::atomic_bool cancelled = false;
  auto missing = result_recall::BuildChartResult(
      validResult(), cancelled,
      [](const result_persistence::PersistedChartResult &,
         std::atomic_bool &) { return std::unique_ptr<bms_parser::Chart>{}; });
  assert(!missing.value.has_value());
  assert(missing.diagnostic == "saved chart is unavailable");

  cancelled = true;
  auto stopped = result_recall::BuildChartResult(validResult(), cancelled,
                                                  chartLoader());
  assert(!stopped.value.has_value());
  assert(stopped.diagnostic == "saved chart is unavailable");
}

void testChartRecallRejectsChangedChartIdentity() {
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      validResult(), cancelled,
      [](const result_persistence::PersistedChartResult &result,
         std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = result.score.chartPath;
        chart->Meta.MD5 = std::string(32, 'd');
        chart->Meta.SHA256 = std::string(64, 'e');
        return chart;
      });

  assert(!outcome.value.has_value());
  assert(outcome.diagnostic ==
         "saved chart no longer matches its stored identity");
}

void testChartRecallAcceptsMatchingMd5OnlyMigrationIdentity() {
  auto persisted = validResult();
  const std::string realSha = std::string(64, 'e');
  persisted.score.chartSha256 =
      *replay::legacyReplaySha256ForMd5(persisted.score.chartMd5);
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      persisted, cancelled,
      [realSha](const result_persistence::PersistedChartResult &result,
                std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = result.score.chartPath;
        chart->Meta.MD5 = result.score.chartMd5;
        chart->Meta.SHA256 = realSha;
        return chart;
      });

  assert(outcome.value.has_value());
  assert(outcome.value->chart->Meta.SHA256 == persisted.score.chartSha256);
}

void testRawReplayPreparationValidatesParsedChartIdentity() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("asobmashow-replay-identity-" +
       std::to_string(std::mt19937_64(std::random_device{}())()));
  std::filesystem::create_directories(directory);
  const auto chartPath = directory / "identity.bms";
  {
    std::ofstream chart(chartPath, std::ios::binary);
    chart << "#TITLE REPLAY IDENTITY\n#ARTIST TEST\n#BPM 120\n"
             "#WAV01 test.wav\n#00111:01\n";
    assert(chart.good());
  }

  std::atomic_bool cancelled = false;
  auto parsed = play_options::parseChart(
      chartPath, std::nullopt, std::nullopt, std::nullopt, cancelled,
      "identity fixture");
  assert(parsed != nullptr);

  replay::ReplayPlaybackData playback;
  playback.setup.chartSha256 = parsed->Meta.SHA256;
  playback.setup.chartMd5 = parsed->Meta.MD5;
  playback.setup.keyMode = parsed->Meta.KeyMode;
  playback.setup.playOption = "NORMAL";
  playback.setup.playOption2 = "NORMAL";
  assert(play_options::prepareReplayChart(chartPath, playback, cancelled) !=
         nullptr);

  auto legacyMd5Only = playback;
  legacyMd5Only.setup.chartSha256 =
      *replay::legacyReplaySha256ForMd5(parsed->Meta.MD5);
  assert(play_options::prepareReplayChart(chartPath, legacyMd5Only,
                                          cancelled) != nullptr);

  auto mismatched = playback;
  mismatched.setup.chartSha256 = std::string(64, 'e');
  mismatched.setup.chartMd5 = std::string(32, 'd');
  assert(play_options::prepareReplayChart(chartPath, mismatched, cancelled) ==
         nullptr);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

void testFrozenCourseEntryReusesRandomBranchAndSelectedLongNoteMode() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("asobmashow-course-entry-facts-" +
       std::to_string(std::mt19937_64(std::random_device{}())()));
  std::filesystem::create_directories(directory);
  const auto chartPath = directory / "random-ln.bms";
  {
    std::ofstream chart(chartPath, std::ios::binary);
    chart << "#PLAYER 1\n#TITLE COURSE FACTS\n#ARTIST TEST\n#BPM 120\n"
             "#LNTYPE 1\n#WAV01 head.wav\n#WAV02 tail.wav\n"
             "#RANDOM 2\n#IF 1\n#00111:01\n#ELSE\n#00111:0101\n"
             "#ENDIF\n#ENDRANDOM\n#00251:0102\n";
    assert(chart.good());
  }

  std::atomic_bool cancelled = false;
  auto selectedBranch = play_options::parseChart(
      chartPath, 123'456U, std::string("std::mt19937_64"), std::vector<int>{2},
      cancelled, "course snapshot fixture");
  assert(selectedBranch != nullptr);
  const auto frozenMeta = selectedBranch->Meta;

  CourseConstraintRules constraints;
  auto prepared = play_options::prepareCourseChart(
      frozenMeta, constraints, long_note_mode::kCnValue, cancelled);
  assert(prepared != nullptr);
  assert(prepared->Meta.SHA256 == frozenMeta.SHA256);
  assert(prepared->Meta.MD5 == frozenMeta.MD5);
  assert(prepared->Meta.RandomSeed == 123'456U);
  assert(prepared->Meta.RandomPrng == "std::mt19937_64");
  assert(prepared->Meta.RandomValues == std::vector<int>({2}));
  assert(prepared->Meta.LnMode == long_note_mode::kCnValue);

  auto preparedAgain = play_options::prepareCourseChart(
      prepared->Meta, constraints, long_note_mode::kCnValue, cancelled);
  assert(preparedAgain != nullptr);
  assert(preparedAgain->Meta.TotalNotes == prepared->Meta.TotalNotes);
  assert(preparedAgain->Meta.PlayLength == prepared->Meta.PlayLength);
  assert(preparedAgain->Meta.RandomValues == prepared->Meta.RandomValues);

  {
    std::ofstream replacement(chartPath, std::ios::binary | std::ios::trunc);
    replacement << "#PLAYER 1\n#TITLE REPLACED COURSE STAGE\n#ARTIST TEST\n"
                   "#BPM 120\n#WAV01 note.wav\n#00111:0101\n";
    assert(replacement.good());
  }
  auto replaced = play_options::prepareCourseChart(
      frozenMeta, constraints, long_note_mode::kCnValue, cancelled);
  assert(replaced == nullptr);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

void testRawReplayPreparationAppliesDoublePlayFlip() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("asobmashow-replay-dp-flip-" +
       std::to_string(std::mt19937_64(std::random_device{}())()));
  std::filesystem::create_directories(directory);
  const auto chartPath = directory / "dp-flip.bms";
  {
    std::ofstream chart(chartPath, std::ios::binary);
    chart << "#PLAYER 2\n#TITLE DP FLIP\n#ARTIST TEST\n#BPM 120\n"
             "#LNTYPE 1\n"
             "#WAV01 player-one-key.wav\n"
             "#WAV02 player-two-key.wav\n"
             "#WAV03 player-one-scratch.wav\n"
             "#WAV04 player-two-scratch.wav\n"
             "#WAV05 player-one-seventh-key.wav\n"
             "#00111:01\n#00118:05\n#00121:02\n#00116:03\n#00126:04\n"
             "#00251:0505\n";
    assert(chart.good());
  }

  std::atomic_bool cancelled = false;
  auto parsed = play_options::parseChart(
      chartPath, std::nullopt, std::nullopt, std::nullopt, cancelled,
      "DP FLIP fixture");
  assert(parsed != nullptr && parsed->Meta.IsDP && parsed->Meta.KeyMode == 14);

  replay::ReplayPlaybackData playback;
  playback.setup.chartSha256 = parsed->Meta.SHA256;
  playback.setup.chartMd5 = parsed->Meta.MD5;
  playback.setup.keyMode = parsed->Meta.KeyMode;
  playback.setup.playOption = "NORMAL";
  playback.setup.playOption2 = "NORMAL";
  playback.setup.doublePlayOption = replay::DoublePlayOption::Flip;
  playback.setup.longNoteMode = long_note_mode::kCnValue;

  auto prepared =
      play_options::prepareReplayChart(chartPath, playback, cancelled);
  assert(prepared != nullptr);
  const auto wavAtLane = [&prepared](int lane) {
    for (const auto *measure : prepared->Measures) {
      for (const auto *timeline : measure->TimeLines) {
        if (lane >= 0 && lane < static_cast<int>(timeline->Notes.size()) &&
            timeline->Notes[static_cast<std::size_t>(lane)] != nullptr) {
          return timeline->Notes[static_cast<std::size_t>(lane)]->Wav;
        }
      }
    }
    return -1;
  };
  assert(wavAtLane(0) == 2);
  assert(wavAtLane(8) == 1);
  assert(wavAtLane(7) == 4);
  assert(wavAtLane(15) == 3);

  JudgedPlaybackData judged;
  judged.chartMeta = parsed->Meta;
  judged.setup = playback.setup;
  auto judgedPrepared =
      play_options::prepareReplayChart(chartPath, judged, cancelled);
  assert(judgedPrepared != nullptr);
  const auto judgedWavAtLane = [&judgedPrepared](int lane) {
    for (const auto *measure : judgedPrepared->Measures) {
      for (const auto *timeline : measure->TimeLines) {
        if (lane >= 0 && lane < static_cast<int>(timeline->Notes.size()) &&
            timeline->Notes[static_cast<std::size_t>(lane)] != nullptr) {
          return timeline->Notes[static_cast<std::size_t>(lane)]->Wav;
        }
      }
    }
    return -1;
  };
  assert(judgedWavAtLane(0) == 2);
  assert(judgedWavAtLane(8) == 1);
  assert(judgedWavAtLane(7) == 4);
  assert(judgedWavAtLane(15) == 3);
  assert(judgedPrepared->Meta.LnMode == long_note_mode::kCnValue);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

result_persistence::PersistedCourseResult validCourseResult() {
  auto first = validResult(101, "charts/stage-1.bms");
  auto second = validResult(102, "charts/stage-2.bms");
  second.score.chartTitle = "Stage Two";
  second.score.score = 2;
  second.score.pGreat = 1;
  second.score.great = 0;
  second.score.maxCombo = 1;
  second.score.comboBreak = 1;
  second.score.fast = 0;
  second.score.finalGauge = 62.5F;
  second.score.clearType = kClearTypeNormalClearRank;
  second.adoptedGaugeHistory = {93.25F, 62.5F};
  second.judgementTiming->byJudgement[PGreat] = {};

  result_persistence::PersistedCourseResult result{
      .resultId = 9,
      .attemptId = kAttemptId,
      .courseKey = "course:v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .legacyCourseId = 7,
      .courseName = "Recall Course",
      .courseGroupName = "Records",
      .constraintJson = "{}",
      .completedCharts = 2,
      .totalCharts = 2,
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Normal,
      .gaugeProfile = GaugeProfile::Course7Keys,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .longNoteMode = 2,
      .finalScore = first.score.score + second.score.score,
      .maxScore = first.score.maxScore + second.score.maxScore,
      .maxCombo = 2,
      .finalGauge = 47.25F,
      .clearType = kClearTypeFailedRank,
      .provenance = verifiedProvenance(GaugeType::Normal),
      .stages = {
          {.stageIndex = 0,
           .score = first.score,
           .keyMode = first.keyMode,
           .adoptedGaugeType = first.adoptedGaugeType,
           .adoptedGaugeHistory = first.adoptedGaugeHistory,
           .judgementTiming = first.judgementTiming},
          {.stageIndex = 1,
           .score = second.score,
           .keyMode = second.keyMode,
           .adoptedGaugeType = second.adoptedGaugeType,
           .adoptedGaugeHistory = second.adoptedGaugeHistory,
           .judgementTiming = second.judgementTiming},
      },
      .entryFacts = {
          {.totalNotes = first.score.maxScore / 2,
           .playLengthMicros = 1'000'000},
          {.totalNotes = second.score.maxScore / 2,
           .playLengthMicros = 2'000'000},
      },
      .playedAtUnixMillis = 1784420645000LL,
  };
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

void testCourseRecallUsesOrderedPersistedStageFacts() {
  auto persisted = validCourseResult();
  const auto expected = persisted;
  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(persisted), cancelled, chartLoader(&calls));

  assert(outcome.value.has_value());
  assert(calls == 2);
  assert(outcome.value->result == expected);
  const auto &session = outcome.value->session;
  assert(session != nullptr);
  assert(session->courseId == expected.legacyCourseId);
  assert(session->courseKey == expected.courseKey);
  assert(session->courseName == expected.courseName);
  assert(session->entries.size() == 2);
  assert(session->authoritativeEntryMetasComplete());
  assert(session->entryMeta(0)->TotalNotes ==
         expected.entryFacts[0].totalNotes);
  assert(session->entryMeta(1)->PlayLength ==
         expected.entryFacts[1].playLengthMicros);
  assert(session->completedResults.size() == 2);
  assert(session->ownedResultBrowseCharts.size() == 2);
  assert(session->courseReplayData == nullptr);
  assert(session->carriedGauge.has_value());
  assert(session->carriedGauge->currentGauge == expected.finalGauge);
  assert(session->carriedGauge->selectedGaugeType ==
         expected.initialGaugeType);
  assert(session->carriedGauge->gaugeAutoShift == expected.gaugeAutoShift);
  assert(session->carriedGauge->gaugeAutoShiftLowerBound ==
         expected.gaugeAutoShiftLowerBound);
  assert(session->carriedGauge->gaugeProfile == expected.gaugeProfile);
  assert(session->carriedGauge->gaugeValues[gaugeTypeIndex(
             session->carriedGauge->gaugeType)] == expected.finalGauge);
  assert(session->recalledCourseClearTypeRank == expected.clearType);
  assert(session->stageProvenance.at(0) ==
         std::optional(expected.stages[0].score.provenance));
  assertStateMatches(session->completedResults[0].state,
                     expected.stages[0].score,
                     expected.stages[0].adoptedGaugeHistory,
                     *expected.stages[0].judgementTiming);
  assertStateMatches(session->completedResults[1].state,
                     expected.stages[1].score,
                     expected.stages[1].adoptedGaugeHistory,
                     *expected.stages[1].judgementTiming);
}

void testIncompleteCourseRecallPreservesPersistedTotalAndOutcome() {
  auto persisted = validCourseResult();
  persisted.stages.resize(1);
  persisted.completedCharts = 1;
  persisted.totalCharts = 3;
  persisted.entryFacts = {
      {.totalNotes = persisted.stages[0].score.maxScore / 2,
       .playLengthMicros = 1'000'000},
      {.totalNotes = 123, .playLengthMicros = 2'000'000},
      {.totalNotes = 456, .playLengthMicros = 3'000'000},
  };
  persisted.finalScore = persisted.stages[0].score.score;
  persisted.maxScore = 1'162;
  persisted.maxCombo = persisted.stages[0].score.maxCombo;
  persisted.finalGauge = 12.5F;
  persisted.clearType = kClearTypeFailedRank;
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);

  int calls = 0;
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      persisted, cancelled, chartLoader(&calls));
  assert(outcome.value.has_value());
  const auto &session = outcome.value->session;
  assert(calls == 1);
  assert(session->entries.size() == 3);
  assert(session->authoritativeEntryMetasComplete());
  assert(session->completedResults.size() == 1);
  assert(session->ownedResultBrowseCharts.size() == 1);
  assert(session->entries[1].meta.BmsPath.empty());
  assert(session->entries[2].meta.BmsPath.empty());
  assert(session->entries[1].meta.TotalNotes == 123);
  assert(session->entries[1].meta.PlayLength == 2'000'000);
  assert(session->entries[2].meta.TotalNotes == 456);
  assert(session->entries[2].meta.PlayLength == 3'000'000);
  assert(session->entryMeta(1)->TotalNotes == 123);
  assert(session->entryMeta(2)->PlayLength == 3'000'000);
  assert(session->carriedGauge.has_value());
  assert(session->carriedGauge->currentGauge == persisted.finalGauge);
  assert(session->recalledCourseClearTypeRank == persisted.clearType);
}

void testCourseRecallDoesNotPublishPartialSession() {
  auto persisted = validCourseResult();
  int calls = 0;
  result_recall::ResultChartLoader failingLoader =
      [&calls](const result_persistence::PersistedChartResult &stage,
               std::atomic_bool &) {
        ++calls;
        if (calls == 2) {
          return std::unique_ptr<bms_parser::Chart>{};
        }
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = stage.score.chartPath;
        chart->Meta.MD5 = stage.score.chartMd5;
        chart->Meta.SHA256 = stage.score.chartSha256;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(persisted), cancelled, std::move(failingLoader));
  assert(!outcome.value.has_value());
  assert(outcome.diagnostic == "saved course stage is unavailable");
  assert(calls == 2);
}

void testCourseRecallRejectsChangedStageIdentity() {
  auto persisted = validCourseResult();
  int calls = 0;
  result_recall::ResultChartLoader changedStageLoader =
      [&calls](const result_persistence::PersistedChartResult &stage,
               std::atomic_bool &) {
        ++calls;
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta.BmsPath = stage.score.chartPath;
        chart->Meta.MD5 = stage.score.chartMd5;
        chart->Meta.SHA256 =
            calls == 2 ? std::string(64, 'e') : stage.score.chartSha256;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      std::move(persisted), cancelled, std::move(changedStageLoader));

  assert(!outcome.value.has_value());
  assert(outcome.diagnostic ==
         "saved course stage no longer matches its stored identity");
  assert(calls == 2);
}

} // namespace

int main() {
  testChartRecallUsesPersistedFactsOnly();
  testChartRecallPreservesResolvedRuntimePathForRetry();
  testChartRecallRejectsInvalidResultBeforeLoadingAssets();
  testChartRecallDoesNotPublishMissingOrCancelledAssets();
  testChartRecallRejectsChangedChartIdentity();
  testChartRecallAcceptsMatchingMd5OnlyMigrationIdentity();
  testRawReplayPreparationValidatesParsedChartIdentity();
  testFrozenCourseEntryReusesRandomBranchAndSelectedLongNoteMode();
  testRawReplayPreparationAppliesDoublePlayFlip();
  testCourseRecallUsesOrderedPersistedStageFacts();
  testIncompleteCourseRecallPreservesPersistedTotalAndOutcome();
  testCourseRecallDoesNotPublishPartialSession();
  testCourseRecallRejectsChangedStageIdentity();
  return 0;
}
