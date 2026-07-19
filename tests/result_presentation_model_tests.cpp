#include "scene/ResultPresentationModel.h"

#include "scene/play/GameplayGaugeTypes.h"
#include "view/ClearLampColors.h"
#include "view/UiTheme.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool sameColor(const Color &left, const Color &right) {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
}

const ResultInfoTile *findInfo(const ResultPresentationModel &model,
                               std::string_view label) {
  for (const auto &tile : model.infoTiles) {
    if (tile.label == label) {
      return &tile;
    }
  }
  return nullptr;
}

const ResultJudgementRow *findJudgement(const ResultPresentationModel &model,
                                        std::string_view label) {
  for (const auto &row : model.judgements) {
    if (row.label == label) {
      return &row;
    }
  }
  return nullptr;
}

bms_parser::ChartMeta localMeta() {
  bms_parser::ChartMeta meta;
  meta.Title = "Local Result";
  meta.Artist = "Local Artist";
  meta.TotalNotes = 1'000;
  meta.TotalLongNotes = 120;
  meta.PlayLevel = 12.0;
  meta.Bpm = 150.0;
  meta.MinBpm = 120.0;
  meta.MaxBpm = 180.0;
  meta.Rank = 2;
  meta.KeyMode = 7;
  meta.PlayLength = 95'000'000;
  meta.TotalLength = 100'000'000;
  return meta;
}

RhythmState localState() {
  RhythmState state(nullptr, false);
  state.judgeCount[PGreat] = 800;
  state.judgeCount[Great] = 100;
  state.judgeCount[Good] = 50;
  state.judgeCount[Bad] = 20;
  state.judgeCount[Poor] = 10;
  state.judgeCount[Kpoor] = 5;
  state.judgementFastSlowCount[PGreat] = {.fast = 380, .slow = 420};
  state.judgementFastSlowCount[Great] = {.fast = 40, .slow = 60};
  state.judgementFastSlowCount[Good] = {.fast = 20, .slow = 30};
  state.judgementFastSlowCount[Bad] = {.fast = 8, .slow = 12};
  state.judgementFastSlowCount[Poor] = {.fast = 4, .slow = 6};
  state.judgementFastSlowCount[Kpoor] = {.fast = 0, .slow = 0};
  state.fastCount = 452;
  state.slowCount = 528;
  state.maxCombo = 700;
  state.comboBreak = 30;
  state.gaugeType = GaugeType::Normal;
  state.selectedGaugeType = GaugeType::Normal;
  state.currentGauge = 82.5F;
  state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 82.5F;
  state.gaugeHistory = {20.0F, 60.0F, 82.5F};
  state.gaugeHistoryFor(GaugeType::Normal) = state.gaugeHistory;
  return state;
}

ResultLocalPresentationOptions localOptions() {
  return {
      .playModeLabel = "R-RANDOM",
      .laneOrderLabel = "123S4567",
      .difficultyLabel = "★12",
      .previousBest =
          ResultPreviousBestData{
              .score = 1'600,
              .maxScore = 2'000,
              .maxCombo = 650,
              .comboBreak = 35,
              .finalGauge = 76.0F,
              .clearType = kClearTypeEasyClearRank,
              .createdAt = "2026-07-18T12:34:56Z",
          },
  };
}

ir::IrRemoteScore remoteScore() {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "remote-score",
      .remoteChartId = "remote-chart",
      .chartMd5 = std::string(32, 'a'),
      .chartSha256 = std::string(64, 'b'),
      .title = "Remote Result",
      .artist = "Remote Artist",
      .service = "Bokutachi",
      .difficulty = "ANOTHER",
      .level = "12",
      .levelNumber = 12.7,
      .noteCount = 1'000,
      .score = 1'800,
      .lampRank = kClearTypeNormalClearRank,
      .timeAchievedUnixMillis = 1'700'000'000'123LL,
      .timeAddedUnixMillis = 1'700'000'100'123LL,
      .judgements =
          {.pGreat = 850, .great = 100, .good = 25, .bad = 15, .poor = 10},
      .timing = {.earlyPGreat = 400,
                 .latePGreat = 450,
                 .earlyGreat = 40,
                 .lateGreat = 60,
                 .earlyGood = 10,
                 .lateGood = 15,
                 .earlyBad = 5,
                 .lateBad = 10,
                 .earlyPoor = 4,
                 .latePoor = 6},
      .fast = 459,
      .slow = 541,
      .maxCombo = 800,
      .badPoints = 25,
      .finalGauge = 82.5F,
      .gaugeHistory = {20.0F, std::nullopt, 0.0F, 100.0F, 82.5F},
      .random = "RANDOM",
      .gauge = "NORMAL",
      .inputDevice = "KEYBOARD",
      .client = "AsoBMaShow",
  };
}

void testLocalNormalParity() {
  const bms_parser::ChartMeta meta = localMeta();
  const RhythmState state = localState();
  auto options = localOptions();

  bms_parser::Chart chart;
  chart.Meta = meta;
  const std::span<const ReplayData> noAttempts;
  options.timingAnalytics.emplace(chart, noAttempts, 2);

  const ResultPresentationModel model =
      makeLocalResultPresentation(meta, state, std::move(options));

  expect(model.title == "Local Result" && model.artist == "Local Artist",
         "local header title and artist are preserved");
  expect(model.difficulty == "★12 / LV 12" && model.playtype == "7K",
         "local difficulty and key mode are presentation-ready");
  expect(model.score == 1'700 && model.maxScore == 2'000,
         "local score and maximum are preserved");
  expect(model.lampRank == kClearTypeNormalClearRank &&
             model.finalGauge == 82.5F && model.maxCombo == 700 &&
             model.comboBreak == 30,
         "local lamp, gauge, combo, and BREAK are preserved");
  expect(!model.badPoints.has_value(),
         "local BREAK is not relabeled as provider BP");

  expect(hasGradeCard(model), "complete local score has a grade card");
  const auto grade = gradeCard(model);
  expect(grade && grade->grade == "AA" && grade->rate == "85.00%" &&
             sameColor(grade->accent, ui_theme::scoreRankColor("AA")),
         "local grade card preserves grade, rate, and semantic color");

  expect(model.scoreComparison &&
             model.scoreComparison->title == "SCORE COMPARISON" &&
             model.scoreComparison->target &&
             model.scoreComparison->target->label == "BEST" &&
             model.scoreComparison->target->value == "1600" &&
             model.scoreComparison->target->detail == "AA" &&
             model.scoreComparison->current.label == "CURRENT" &&
             model.scoreComparison->current.value == "1700" &&
             model.scoreComparison->current.detail == "MAX 2000" &&
             model.scoreComparison->delta == "DELTA +100",
         "local score comparison text matches DefaultSkin");
  expect(model.lampComparison && model.lampComparison->target &&
             model.lampComparison->target->value == "EASY CLEAR" &&
             model.lampComparison->target->detail == "GAUGE 76.0%" &&
             model.lampComparison->current.value == "NORMAL CLEAR" &&
             model.lampComparison->current.detail == "GAUGE 82.5%" &&
             sameColor(model.lampComparison->current.accent,
                       clearLampColorForRank(kClearTypeNormalClearRank)),
         "local lamp comparison labels, values, and colors are preserved");
  expect(model.comboComparison && model.comboComparison->target &&
             model.comboComparison->target->value == "650" &&
             model.comboComparison->target->detail == "BREAK 35" &&
             model.comboComparison->current.value == "700" &&
             model.comboComparison->current.detail == "BREAK 30" &&
             model.comboComparison->delta == "COMBO +50 / BREAK -5",
         "local combo comparison text matches DefaultSkin");
  expect(hasComboBreakCard(model),
         "complete local combo and BREAK have their comparison card");

  expect(model.infoTiles.size() == 6,
         "local information grid keeps all six existing tiles");
  const auto *nextGrade = findInfo(model, "NEXT GRADE");
  const auto *totalNotes = findInfo(model, "TOTAL NOTES");
  const auto *bpm = findInfo(model, "BPM");
  const auto *judgeRank = findInfo(model, "JUDGE RANK");
  const auto *duration = findInfo(model, "DURATION");
  const auto *playMode = findInfo(model, "PLAY MODE");
  expect(nextGrade && nextGrade->value == "AAA" && nextGrade->detail == "-78",
         "local next-grade value is preserved");
  expect(totalNotes && totalNotes->value == "1000" &&
             totalNotes->detail == "120 LN",
         "local note totals are preserved");
  expect(bpm && bpm->value == "120-180(150)",
         "local variable BPM values are preserved");
  expect(judgeRank && judgeRank->value == "NORMAL",
         "local judge rank is preserved");
  expect(duration && duration->value == "1:35" &&
             duration->detail == "BGA 1:40",
         "local duration and BGA duration are preserved");
  expect(playMode && playMode->value == "R-RANDOM" &&
             playMode->detail == "123S4567",
         "local play mode and lane order are preserved");

  expect(hasJudgementCard(model) && model.judgements.size() == 6,
         "local judgement card keeps all six rows");
  const auto *pGreat = findJudgement(model, "PGREAT");
  const auto *poor = findJudgement(model, "POOR");
  const auto *kPoor = findJudgement(model, "KPOOR");
  expect(pGreat && pGreat->total == 800 && pGreat->early == 380 &&
             pGreat->late == 420 && sameColor(pGreat->color, ui_theme::cyan()),
         "local PGREAT total and timing values are preserved");
  expect(poor && poor->total == 10 && poor->early == 4 && poor->late == 6,
         "local POOR total and timing values are preserved");
  expect(kPoor && kPoor->total == 5 && kPoor->early == 0 && kPoor->late == 0,
         "local KPOOR remains present with explicit timing zeros");
  expect(model.comboBreak ==
             state.judgeCount.at(Bad) + state.judgeCount.at(Poor),
         "local BREAK is BAD plus POOR and excludes KPOOR");
  expect(model.fast == 452 && model.slow == 528,
         "local FAST and SLOW totals are preserved");
  expect(timingRows(model).size() == 6,
         "local timing rows remain present for every judgement");

  expect(hasGaugeCard(model) && model.gaugeSeries.size() == 1 &&
             model.gaugeSeries.front().label == "NORMAL" &&
             model.gaugeSeries.front().clearRank == kClearTypeNormalClearRank &&
             model.gaugeSeries.front().points ==
                 std::vector<std::optional<float>>({20.0F, 60.0F, 82.5F}),
         "local normal gauge graph keeps its values and colored label");
  expect(model.timingAnalytics.has_value() &&
             model.timingAnalytics->abandonedAttempts() == 2,
         "local timing analytics model is preserved");
  expect(!model.readOnlyIrUploaded,
         "local presentation is not marked as read-only IR");
}

void testLocalPacemakerAndRecallParity() {
  const auto meta = localMeta();
  const auto state = localState();
  auto options = localOptions();
  options.pacemaker = ResultPacemakerData{
      .label = "AAA",
      .targetScore = 1'725,
      .delta = -25,
      .usesReplayProgression = true,
  };

  const auto model = makeLocalResultPresentation(meta, state, options);
  expect(model.scoreComparison && model.scoreComparison->title == "PACEMAKER" &&
             model.scoreComparison->target &&
             model.scoreComparison->target->label == "AAA" &&
             model.scoreComparison->target->value == "1725" &&
             model.scoreComparison->target->detail == "PACEMAKER GHOST" &&
             model.scoreComparison->current.label == "CURRENT" &&
             model.scoreComparison->delta == "PACEMAKER -25",
         "recalled local results retain pacemaker and current labels");

  options.previousBest.reset();
  options.pacemaker.reset();
  const auto noPrevious = makeLocalResultPresentation(meta, state, options);
  expect(noPrevious.scoreComparison && noPrevious.scoreComparison->target &&
             noPrevious.scoreComparison->target->value == "NO PLAY" &&
             noPrevious.scoreComparison->delta == "DELTA --" &&
             noPrevious.comboComparison && noPrevious.comboComparison->target &&
             noPrevious.comboComparison->target->value == "NO PLAY" &&
             noPrevious.comboComparison->delta == "COMBO -- / BREAK --",
         "local no-previous placeholders remain unchanged");
}

void testLocalGasGaugeOrder() {
  const auto meta = localMeta();
  auto state = localState();
  state.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  state.gaugeAutoShiftLowerBound = GaugeType::Easy;
  state.gaugeType = GaugeType::Hard;
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    state.gaugeHistoryFor(gaugeTypeAtIndex(index)) = {
        static_cast<float>(index), static_cast<float>(index + 10)};
  }

  const auto model = makeLocalResultPresentation(meta, state, localOptions());
  const std::vector<std::string> expectedLabels{"HARD", "HAZARD", "EX-HARD",
                                                "NORMAL", "EASY"};
  expect(model.gaugeSeries.size() == expectedLabels.size(),
         "GAS exposes the existing candidate count");
  for (std::size_t index = 0;
       index < model.gaugeSeries.size() && index < expectedLabels.size();
       ++index) {
    expect(model.gaugeSeries[index].label == expectedLabels[index],
           "GAS keeps the existing cycle order and label");
  }
  expect(!model.gaugeSeries.empty() &&
             model.gaugeSeries.front().label == "HARD" &&
             model.gaugeSeries.front().clearRank == kClearTypeHardClearRank &&
             model.gaugeSeries.front().points.front() == 3.0F,
         "GAS first series is the final adopted gauge with its colored rank");
}

void testLocalFailedFullComboAndCourseOverrides() {
  const auto meta = localMeta();
  auto failed = localState();
  failed.currentGauge = 0.0F;
  failed.gaugeValues[gaugeTypeIndex(failed.gaugeType)] = 0.0F;
  auto options = localOptions();
  const auto failedModel = makeLocalResultPresentation(meta, failed, options);
  expect(failedModel.lampRank == kClearTypeFailedRank &&
             failedModel.lampComparison &&
             failedModel.lampComparison->current.value == "FAILED" &&
             failedModel.finalGauge == 0.0F,
         "failed local result keeps failed lamp and explicit zero gauge");

  options.currentClearLabelOverride = "FULL COMBO";
  options.currentClearRankOverride = kClearTypeFullComboRank;
  const auto fullCombo =
      makeLocalResultPresentation(meta, localState(), options);
  expect(fullCombo.lampRank == kClearTypeFullComboRank &&
             fullCombo.lampComparison &&
             fullCombo.lampComparison->current.value == "FULL COMBO" &&
             sameColor(fullCombo.lampComparison->current.accent,
                       clearLampColorForRank(kClearTypeFullComboRank)),
         "full-combo recall keeps its overridden label, rank, and color");

  options.currentClearLabelOverride = "NO PLAY";
  options.currentClearRankOverride = kNoClearTypeRank;
  const auto stage = makeLocalResultPresentation(meta, localState(), options);
  expect(stage.lampRank == kNoClearTypeRank && stage.lampComparison &&
             stage.lampComparison->current.value == "NO PLAY",
         "saved course stage keeps NO PLAY presentation");

  options.headerDifficultyLabelOverride = "COURSE";
  options.currentClearLabelOverride = "FULL COMBO";
  options.currentClearRankOverride = kClearTypeFullComboRank;
  const auto courseFinal =
      makeLocalResultPresentation(meta, localState(), options);
  expect(courseFinal.difficulty == "COURSE" && courseFinal.lampComparison &&
             courseFinal.lampComparison->current.value == "FULL COMBO",
         "saved course final keeps course header and final lamp");
}

void testFullyPopulatedRemotePresentation() {
  const auto model = makeRemoteResultPresentation(remoteScore());

  expect(model.title == "Remote Result" && model.artist == "Remote Artist" &&
             model.difficulty == "ANOTHER" && model.playtype == "7K",
         "remote header uses canonical title, artist, difficulty, and game");
  expect(model.achievedAtUnixMillis == 1'700'000'000'123LL &&
             model.service == "Bokutachi" && model.client == "AsoBMaShow" &&
             model.inputDevice == "KEYBOARD" && model.random == "RANDOM" &&
             model.gaugeType == "NORMAL",
         "remote metadata is copied without provider JSON parsing");
  expect(model.score == 1'800 && model.maxScore == 2'000 && hasGradeCard(model),
         "positive note count enables remote max score and grade");
  const auto grade = gradeCard(model);
  expect(grade && grade->grade == "AAA" && grade->rate == "90.00%" &&
             sameColor(grade->accent, ui_theme::scoreRankColor("AAA")),
         "remote EX rate, grade, and color are deterministic");
  expect(model.scoreComparison && !model.scoreComparison->target &&
             model.scoreComparison->current.value == "1800" &&
             model.scoreComparison->current.detail == "MAX 2000" &&
             !model.scoreComparison->delta,
         "remote score card has no inferred comparison target");
  expect(model.lampRank == kClearTypeNormalClearRank && model.lampComparison &&
             !model.lampComparison->target &&
             model.lampComparison->current.value == "NORMAL CLEAR" &&
             model.lampComparison->current.detail == "GAUGE 82.5%" &&
             sameColor(model.lampComparison->current.accent,
                       clearLampColorForRank(kClearTypeNormalClearRank)),
         "remote lamp uses known local label and color mapping");
  expect(model.maxCombo == 800 && model.comboBreak == 25 &&
             model.badPoints == 25 && hasComboBreakCard(model) &&
             model.comboComparison && !model.comboComparison->target &&
             model.comboComparison->current.value == "800" &&
             model.comboComparison->current.detail == "BREAK 25",
         "remote combo, derived BREAK, and separately labeled BP are present");
  const auto *bp = findInfo(model, "BP");
  expect(bp && bp->value == "25", "remote bad points remain labeled BP");

  expect(hasJudgementCard(model) && model.judgements.size() == 5,
         "remote judgement card requires and contains all five totals");
  expect(!findJudgement(model, "KPOOR"),
         "remote presentation never creates KPOOR");
  const auto *pGreat = findJudgement(model, "P-GREAT");
  expect(pGreat && pGreat->total == 850 && pGreat->early == 400 &&
             pGreat->late == 450 && sameColor(pGreat->color, ui_theme::cyan()),
         "remote P-GREAT total, timing pair, and color are mapped once");
  expect(timingRows(model).size() == 5,
         "fully populated remote score has five timing rows");
  expect(model.fast == 459 && model.slow == 541,
         "remote FAST and SLOW totals remain supplied values");

  expect(model.finalGauge == 82.5F && hasGaugeCard(model) &&
             model.gaugeSeries.size() == 1 &&
             model.gaugeSeries.front().label == "NORMAL" &&
             model.gaugeSeries.front().clearRank == kClearTypeNormalClearRank &&
             model.gaugeSeries.front().points == remoteScore().gaugeHistory &&
             !model.gaugeSeries.front().points[1].has_value(),
         "remote gauge card preserves null segments and supplied label");
  expect(!model.timingAnalytics.has_value(),
         "remote presentation never creates replay timing analytics");
  expect(model.readOnlyIrUploaded,
         "remote presentation is explicitly read-only and uploaded");
}

void testRemoteGradeAndPlaytypeDependencies() {
  auto remote = remoteScore();
  remote.noteCount = 0;
  remote.score = 0;
  const auto noNotes = makeRemoteResultPresentation(remote);
  expect(
      noNotes.score == 0 && !noNotes.maxScore && !hasGradeCard(noNotes) &&
          !gradeCard(noNotes) && !noNotes.scoreComparison,
      "missing positive note count removes max-dependent score presentation");

  remote = remoteScore();
  remote.game = "BMS-7K";
  expect(!makeRemoteResultPresentation(remote).playtype,
         "remote playtype is not inferred from a near-match game");
  remote.game = "bms-14k";
  expect(makeRemoteResultPresentation(remote).playtype == "14K",
         "exact bms-14k maps to the remote playtype");
}

void testRemoteJudgementAndTimingOmissions() {
  using Reset = void (*)(ir::IrRemoteJudgements &);
  const std::vector<Reset> resets{
      [](auto &v) { v.pGreat.reset(); }, [](auto &v) { v.great.reset(); },
      [](auto &v) { v.good.reset(); },   [](auto &v) { v.bad.reset(); },
      [](auto &v) { v.poor.reset(); },
  };
  for (const Reset reset : resets) {
    auto remote = remoteScore();
    reset(remote.judgements);
    const auto model = makeRemoteResultPresentation(remote);
    expect(!hasJudgementCard(model) && model.judgements.empty(),
           "one missing remote total removes the whole judgement card");
    expect(!findJudgement(model, "KPOOR"),
           "remote omission path never creates KPOOR");
  }

  auto remote = remoteScore();
  remote.timing.earlyPGreat.reset();
  const auto missingEarly = makeRemoteResultPresentation(remote);
  const auto *pGreat = findJudgement(missingEarly, "P-GREAT");
  expect(hasJudgementCard(missingEarly) && pGreat && !pGreat->early &&
             !pGreat->late && timingRows(missingEarly).size() == 4,
         "one missing timing side removes only that judgement timing row");
  expect(findJudgement(missingEarly, "GREAT") &&
             findJudgement(missingEarly, "GREAT")->early == 40 &&
             findJudgement(missingEarly, "GREAT")->late == 60,
         "other remote totals and timing rows remain intact");
}

void testRemoteIndependentOptionalCardsAndMetadata() {
  const auto full = makeRemoteResultPresentation(remoteScore());

  auto remote = remoteScore();
  remote.maxCombo.reset();
  const auto noCombo = makeRemoteResultPresentation(remote);
  expect(!noCombo.maxCombo && noCombo.comboBreak == 25 &&
             !hasComboBreakCard(noCombo) && noCombo.badPoints == 25 &&
             hasGaugeCard(noCombo),
         "missing combo removes only the combo/BREAK card");

  remote = remoteScore();
  remote.badPoints.reset();
  const auto noBp = makeRemoteResultPresentation(remote);
  expect(!noBp.badPoints && !findInfo(noBp, "BP") && hasComboBreakCard(noBp) &&
             hasGaugeCard(noBp),
         "missing BP removes only its independently labeled tile");

  remote = remoteScore();
  remote.finalGauge.reset();
  const auto noFinalGauge = makeRemoteResultPresentation(remote);
  expect(!noFinalGauge.finalGauge && noFinalGauge.lampComparison &&
             noFinalGauge.lampComparison->current.detail.empty() &&
             hasGaugeCard(noFinalGauge) &&
             noFinalGauge.gaugeSeries == full.gaugeSeries,
         "missing final gauge leaves lamp and gauge history intact");

  remote = remoteScore();
  remote.gaugeHistory.clear();
  const auto noHistory = makeRemoteResultPresentation(remote);
  expect(noHistory.finalGauge == 82.5F && !hasGaugeCard(noHistory) &&
             noHistory.gaugeSeries.empty() && noHistory.gaugeType == "NORMAL",
         "missing gauge history removes only the graph card");

  remote = remoteScore();
  remote.random.reset();
  const auto noRandom = makeRemoteResultPresentation(remote);
  expect(!noRandom.random && !findInfo(noRandom, "RANDOM") &&
             noRandom.service == full.service &&
             noRandom.client == full.client &&
             noRandom.inputDevice == full.inputDevice &&
             noRandom.gaugeType == full.gaugeType,
         "one missing metadata value removes only its tile");

  remote = remoteScore();
  remote.service.clear();
  const auto noService = makeRemoteResultPresentation(remote);
  expect(!noService.service && !findInfo(noService, "SERVICE") &&
             noService.client == "AsoBMaShow",
         "empty canonical service remains absent rather than a placeholder");
}

void testRemoteMissingVersusExplicitZero() {
  auto remote = remoteScore();
  remote.judgements = {.pGreat = 0, .great = 0, .good = 0, .bad = 0, .poor = 0};
  remote.timing = {.earlyPGreat = 0,
                   .latePGreat = 0,
                   .earlyGreat = 0,
                   .lateGreat = 0,
                   .earlyGood = 0,
                   .lateGood = 0,
                   .earlyBad = 0,
                   .lateBad = 0,
                   .earlyPoor = 0,
                   .latePoor = 0};
  remote.fast = 0;
  remote.slow = 0;
  remote.maxCombo = 0;
  remote.badPoints = 0;
  remote.finalGauge = 0.0F;
  remote.gaugeHistory = {0.0F, std::nullopt};
  const auto zero = makeRemoteResultPresentation(remote);
  expect(zero.comboBreak == 0 && zero.badPoints == 0 && zero.maxCombo == 0 &&
             zero.finalGauge == 0.0F && zero.fast == 0 && zero.slow == 0,
         "explicit zero remote metrics remain present");
  expect(hasJudgementCard(zero) && hasComboBreakCard(zero) &&
             hasGaugeCard(zero) && timingRows(zero).size() == 5,
         "explicit zero dependencies still create their cards and rows");
  const auto *bad = findJudgement(zero, "BAD");
  expect(bad && bad->total == 0 && bad->early == 0 && bad->late == 0,
         "explicit zero judgement and timing values remain present");

  remote.judgements.bad.reset();
  const auto missing = makeRemoteResultPresentation(remote);
  expect(!missing.comboBreak && !hasJudgementCard(missing),
         "missing BAD is absent and distinct from explicit zero");
}

void testRemoteUnknownLampDoesNotInventPresentation() {
  auto remote = remoteScore();
  remote.lampRank = 999;
  const auto model = makeRemoteResultPresentation(remote);
  expect(!model.lampRank && !model.lampComparison &&
             model.gaugeSeries.size() == 1 &&
             !model.gaugeSeries.front().clearRank,
         "unknown remote lamp has no invented label, color, or graph rank");
}
} // namespace

int main() {
  ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
  testLocalNormalParity();
  testLocalPacemakerAndRecallParity();
  testLocalGasGaugeOrder();
  testLocalFailedFullComboAndCourseOverrides();
  testFullyPopulatedRemotePresentation();
  testRemoteGradeAndPlaytypeDependencies();
  testRemoteJudgementAndTimingOmissions();
  testRemoteIndependentOptionalCardsAndMetadata();
  testRemoteMissingVersusExplicitZero();
  testRemoteUnknownLampDoesNotInventPresentation();
  if (failures != 0) {
    std::cerr << failures << " result presentation model test(s) failed\n";
    return 1;
  }
  std::cout << "Result presentation model tests passed\n";
  return 0;
}
