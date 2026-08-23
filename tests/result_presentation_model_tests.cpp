#include "scene/ResultPresentationModel.h"
#include "scene/ResultPhotoExportPresentation.h"
#include "scene/ResultSkinFailurePresentation.h"
#include "scene/ResultSkinApplicationOverlays.h"
#include "scene/ResultSkinLayering.h"
#include "scene/ResultTouchControls.h"

#include "rendering/UniformCache.h"
#include "scene/ResultGaugeHistory.h"
#include "scene/play/GameplayGaugeTypes.h"
#include "skin/DefaultSkin.h"
#include "view/ClearLampColors.h"
#include "view/TextView.h"
#include "view/UiTheme.h"

#include <bgfx/bgfx.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

void SceneManager::changeScene(const std::string &, bool) {}

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

const TextView *textView(View *root, std::string_view name) {
  return root == nullptr ? nullptr
                         : dynamic_cast<const TextView *>(
                               root->findViewByName(std::string(name)));
}

std::vector<std::string> descendantTexts(const View &root) {
  std::vector<std::string> result;
  const auto collect = [&result](const auto &self, const View &view) -> void {
    if (const auto *text = dynamic_cast<const TextView *>(&view)) {
      result.push_back(text->getText());
    }
    for (const View *child : const_cast<View &>(view).getChildren()) {
      self(self, *child);
    }
  };
  collect(collect, root);
  return result;
}

ResultLocalPresentationOptions localOptions();

std::unique_ptr<View>
buildPresentationLayout(const ResultPresentationModel &model, int width = 1920,
                        int height = 1080, bool showControls = false) {
  rendering::window_width = width;
  rendering::window_height = height;
  rendering::render_width = width;
  rendering::render_height = height;
  rendering::ui_view_width = width;
  rendering::ui_view_height = height;

  auto root = std::make_unique<View>(0, 0, width, height);
  ResultSkinData skinData{};
  skinData.showControls = showControls;
  skinData.showTimingAnalytics = true;
  skinData.showResultGraph = true;
  skinData.presentation = &model;
  DefaultSkin skin;
  skin.buildLayout("Result", root.get(), &skinData);
  root->applyYogaLayout();
  return root;
}

std::unique_ptr<View> buildLegacyLayout(const bms_parser::ChartMeta &meta,
                                        const RhythmState &state) {
  rendering::window_width = rendering::design_width;
  rendering::window_height = rendering::design_height;
  rendering::render_width = rendering::design_width;
  rendering::render_height = rendering::design_height;
  rendering::ui_view_width = rendering::design_width;
  rendering::ui_view_height = rendering::design_height;

  auto root = std::make_unique<View>(0, 0, rendering::design_width,
                                     rendering::design_height);
  ResultSkinData skinData{&state, &meta, nullptr};
  skinData.showControls = false;
  skinData.playModeLabel = "R-RANDOM";
  skinData.laneOrderLabel = "123S4567";
  skinData.difficultyLabel = "★12";
  skinData.previousBest = localOptions().previousBest;
  DefaultSkin skin;
  skin.buildLayout("Result", root.get(), &skinData);
  root->applyYogaLayout();
  return root;
}

ResultComparisonCard currentOnlyCard(std::string title, std::string value,
                                     Color accent = ui_theme::textPrimary()) {
  return {
      .title = std::move(title),
      .current = {.label = "CURRENT",
                  .value = std::move(value),
                  .detail = {},
                  .accent = accent},
  };
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
  options.previousLampBest = ResultPreviousBestData{
      .score = 1'200,
      .maxScore = 2'000,
      .maxCombo = 500,
      .comboBreak = 80,
      .finalGauge = 100.0F,
      .clearType = kClearTypeHardClearRank,
      .createdAt = "2026-07-17T12:34:56Z",
  };

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
             model.lampComparison->target->value == "HARD CLEAR" &&
             model.lampComparison->target->detail == "GAUGE 100.0%" &&
             model.lampComparison->current.value == "NORMAL CLEAR" &&
             model.lampComparison->current.detail == "GAUGE 82.5%" &&
             sameColor(model.lampComparison->current.accent,
                       clearLampColorForRank(kClearTypeNormalClearRank)),
         "local lamp comparison uses the best lamp rather than best score");
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

  std::size_t selected = 0;
  for (std::size_t expected = 1; expected < model.gaugeSeries.size();
       ++expected) {
    selected =
        result_gauge_history::nextSeriesIndex(model.gaugeSeries, selected);
    expect(selected == expected,
           "presentation GAS selection advances in model series order");
    const auto graph =
        result_gauge_history::graphFor(model.gaugeSeries, selected);
    expect(graph && graph->seriesIndex == selected && graph->label &&
               graph->label->text == expectedLabels[expected] &&
               sameColor(graph->label->background,
                         clearLampColorForRank(
                             *model.gaugeSeries[expected].clearRank)),
           "presentation GAS selection keeps the selected label and color");
  }
  expect(result_gauge_history::nextSeriesIndex(model.gaugeSeries, selected) ==
             0,
         "presentation GAS selection wraps to the adopted gauge");
}

void testSceneAndExporterShareInitialGaugeChoice() {
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
  const auto localSeries = result_gauge_history::seriesFor(state);
  const auto sceneChoice = result_gauge_history::graphFor(model.gaugeSeries, 0);
  const auto exporterChoice = result_gauge_history::graphFor(localSeries, 0);
  expect(model.gaugeSeries == localSeries && sceneChoice && exporterChoice &&
             sceneChoice->seriesIndex == exporterChoice->seriesIndex &&
             sceneChoice->label && exporterChoice->label &&
             sceneChoice->label->text == exporterChoice->label->text &&
             sameColor(sceneChoice->label->background,
                       exporterChoice->label->background) &&
             sceneChoice->geometry.segments.size() ==
                 exporterChoice->geometry.segments.size() &&
             sceneChoice->geometry.markers.size() ==
                 exporterChoice->geometry.markers.size(),
         "scene model and photo exporter adapter share one initial graph choice");
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
  const auto graph = result_gauge_history::graphFor(model.gaugeSeries, 0);
  expect(graph && graph->geometry.strips.size() == 2 &&
             graph->geometry.segments.size() == 2 &&
             graph->geometry.segments[0].from.index == 2 &&
             graph->geometry.segments[0].to.index == 3 &&
             graph->geometry.segments[1].from.index == 3 &&
             graph->geometry.segments[1].to.index == 4,
         "remote null history preserves separate strips with adjacent segments");
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
  expect(!noBp.badPoints && !findInfo(noBp, "BP") && !noBp.comboBreak &&
             !hasComboBreakCard(noBp) && hasGaugeCard(noBp),
         "missing BP removes its tile and unverifiable BREAK presentation");

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
  remote.gaugeHistory = {std::nullopt, std::nullopt, std::nullopt};
  const auto allNullHistory = makeRemoteResultPresentation(remote);
  expect(!hasGaugeCard(allNullHistory) &&
             allNullHistory.gaugeSeries.empty(),
         "all-null gauge history removes the graph card and geometry source");

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

void testRemoteBreakUsesAuthoritativeBadPoints() {
  auto remote = remoteScore();
  remote.badPoints = 31;
  const auto authoritative = makeRemoteResultPresentation(remote);
  expect(authoritative.comboBreak == 31 && authoritative.comboComparison &&
             authoritative.comboComparison->current.detail == "BREAK 31",
         "remote BREAK uses authoritative BP instead of incomplete judgements");

  remote.badPoints.reset();
  const auto missing = makeRemoteResultPresentation(remote);
  expect(!missing.comboBreak && !hasComboBreakCard(missing),
         "remote BREAK is hidden when authoritative BP is unavailable");
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
  expect(missing.comboBreak == 0 && hasComboBreakCard(missing) &&
             !hasJudgementCard(missing),
         "missing BAD does not discard separately supplied zero BP");
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

void testRemoteGaugeLabelAndLampFallbackSemantics() {
  auto remote = remoteScore();
  remote.lampRank = 999;
  remote.gauge = "NORMAL";
  const auto supplied = makeRemoteResultPresentation(remote);
  const auto suppliedGraph =
      result_gauge_history::graphFor(supplied.gaugeSeries, 0);
  expect(suppliedGraph && suppliedGraph->label &&
             suppliedGraph->label->text == "NORMAL" &&
             sameColor(suppliedGraph->label->background,
                       clearLampColorForRank(kClearTypeNormalClearRank)),
         "remote supplied gauge label is preserved with known gauge color");

  remote = remoteScore();
  remote.gauge.reset();
  const auto lampOnly = makeRemoteResultPresentation(remote);
  const auto lampGraph =
      result_gauge_history::graphFor(lampOnly.gaugeSeries, 0);
  expect(lampGraph && lampGraph->label &&
             lampGraph->label->text == "NORMAL CLEAR" &&
             sameColor(lampGraph->label->background,
                       clearLampColorForRank(kClearTypeNormalClearRank)),
         "known remote lamp supplies lamp semantics without inventing gauge");
}

void testDefaultSkinLocalPresentationContract() {
  auto options = localOptions();
  bms_parser::Chart chart;
  chart.Meta = localMeta();
  const std::span<const ReplayData> noAttempts;
  options.timingAnalytics.emplace(chart, noAttempts, 2);
  const auto model = makeLocalResultPresentation(localMeta(), localState(),
                                                 std::move(options));
  const auto root = buildPresentationLayout(model, 1920, 1080, true);

  expect(textView(root.get(), "title") &&
             textView(root.get(), "title")->getText() == "Local Result" &&
             textView(root.get(), "difficulty") &&
             textView(root.get(), "artist"),
         "authoritative local presentation keeps existing header descendants");
  expect(root->findViewByName("resultSummary") &&
             root->findViewByName("resultSummaryCard:grade") &&
             root->findViewByName("resultSummaryCard:score") &&
             root->findViewByName("resultSummaryCard:lamp") &&
             root->findViewByName("resultSummaryCard:combo") &&
             textView(root.get(), "grade") &&
             textView(root.get(), "grade")->getText() == "AA",
         "local presentation keeps the named grade and comparison cards");
  auto *gradeCardView = root->findViewByName("resultSummaryCard:grade");
  auto *scoreCardView = root->findViewByName("resultSummaryCard:score");
  auto *lampCardView = root->findViewByName("resultSummaryCard:lamp");
  auto *comboCardView = root->findViewByName("resultSummaryCard:combo");
  expect(
      gradeCardView && scoreCardView && lampCardView && comboCardView &&
          std::abs(gradeCardView->getWidth() - scoreCardView->getWidth()) <=
              1 &&
          std::abs(scoreCardView->getWidth() - lampCardView->getWidth()) <= 1 &&
          std::abs(lampCardView->getWidth() - comboCardView->getWidth()) <= 1,
      "complete authoritative local summary cards share width evenly");
  expect(root->findViewByName("resultInfoGrid") &&
             root->findViewByName("resultInfoTile:total-notes") &&
             textView(root.get(), "resultInfoLabel:total-notes") &&
             textView(root.get(), "TOTAL NOTES") &&
             root->findViewByName("resultInfoTile:play-mode") &&
             textView(root.get(), "PLAY MODE"),
         "local information values keep their names and gain semantic names");
  expect(
      root->findViewByName("detailsGrid") &&
          root->findViewByName("resultJudgementTile:pgreat") &&
          textView(root.get(), "pgreat") &&
          textView(root.get(), "pgreatFast") &&
          textView(root.get(), "pgreatSlow") &&
          root->findViewByName("resultJudgementTile:kpoor") &&
          textView(root.get(), "kpoor") && textView(root.get(), "break") &&
          textView(root.get(), "fast") && textView(root.get(), "slow"),
      "local judgement, KPOOR, BREAK, FAST, and SLOW names remain available");
  expect(root->findViewByName("graph") != nullptr,
         "local model gauge history keeps the graph in the skin view tree");
  expect(root->findViewByName("resultVisuals") &&
             root->findViewByName("timingAnalytics") &&
             root->findViewByName("backButton"),
         "local model keeps supplied replay analytics and current controls");

  auto noPreviousOptions = localOptions();
  noPreviousOptions.previousBest.reset();
  const auto noPreviousModel = makeLocalResultPresentation(
      localMeta(), localState(), std::move(noPreviousOptions));
  const auto noPreviousRoot = buildPresentationLayout(noPreviousModel);
  const auto localTexts = descendantTexts(*noPreviousRoot);
  expect(std::ranges::find(localTexts, "DELTA --") != localTexts.end() &&
             std::ranges::find(localTexts, "COMBO -- / BREAK --") !=
                 localTexts.end() &&
             std::ranges::find(localTexts, "NO PLAY") != localTexts.end(),
         "local no-previous comparison placeholders remain rendered");
}

void testDefaultSkinLegacyNullPresentationParity() {
  const auto meta = localMeta();
  const auto state = localState();
  const auto root = buildLegacyLayout(meta, state);

  expect(textView(root.get(), "title") && textView(root.get(), "artist") &&
             textView(root.get(), "difficulty") &&
             !root->findViewByName("playtype"),
         "legacy null presentation keeps the original result header");
  auto *summary = root->findViewByName("resultSummary");
  auto *gradePanel = root->findViewByName("resultSummaryCard:grade");
  auto *scorePanel = root->findViewByName("resultSummaryCard:score");
  auto *lampPanel = root->findViewByName("resultSummaryCard:lamp");
  auto *comboPanel = root->findViewByName("resultSummaryCard:combo");
  expect(summary && gradePanel && scorePanel &&
             summary->getChildren().size() == 4 &&
             gradePanel->getWidth() == 196 &&
             scorePanel->getWidth() > gradePanel->getWidth() && lampPanel &&
             comboPanel && lampPanel->getWidth() > gradePanel->getWidth() &&
             comboPanel->getWidth() > gradePanel->getWidth(),
         "legacy null presentation keeps fixed grade and comparison geometry");
  expect(
      root->findViewByName("resultInfoGrid") &&
          textView(root.get(), "NEXT GRADE") &&
          textView(root.get(), "TOTAL NOTES") && root->findViewByName("BPM") &&
          textView(root.get(), "JUDGE RANK") &&
          textView(root.get(), "DURATION") && textView(root.get(), "PLAY MODE"),
      "legacy null presentation retains every current information value");
  expect(textView(root.get(), "pgreat") && textView(root.get(), "kpoor") &&
             textView(root.get(), "break") && textView(root.get(), "fast") &&
             textView(root.get(), "slow") && root->findViewByName("graph"),
         "legacy null presentation retains judgements, metrics, and graph");

  auto emptyMeta = meta;
  emptyMeta.TotalNotes = 0;
  const auto emptyRoot = buildLegacyLayout(emptyMeta, state);
  expect(textView(emptyRoot.get(), "grade") &&
             textView(emptyRoot.get(), "grade")->getText().empty() &&
             textView(emptyRoot.get(), "resultGradeRate") &&
             textView(emptyRoot.get(), "resultGradeRate")->getText() == "0.00%",
         "legacy null presentation retains the zero-note grade panel");
}

void testDefaultSkinSparseRemoteOmitsUnsupportedViews() {
  auto remote = remoteScore();
  remote.game.clear();
  remote.artist.clear();
  remote.service.clear();
  remote.difficulty.reset();
  remote.level.reset();
  remote.noteCount = 0;
  remote.score = 0;
  remote.lampRank = 999;
  remote.judgements = {};
  remote.timing = {};
  remote.fast.reset();
  remote.slow.reset();
  remote.maxCombo.reset();
  remote.badPoints.reset();
  remote.finalGauge.reset();
  remote.gaugeHistory.clear();
  remote.random.reset();
  remote.gauge.reset();
  remote.inputDevice.reset();
  remote.client.reset();
  const auto model = makeRemoteResultPresentation(remote);
  const auto root = buildPresentationLayout(model);

  expect(textView(root.get(), "title") &&
             textView(root.get(), "title")->getText() == "Remote Result",
         "sparse remote result still renders its supplied title");
  expect(!root->findViewByName("resultSummary") &&
             !root->findViewByName("grade") &&
             !root->findViewByName("resultInfoGrid") &&
             !root->findViewByName("detailsGrid") &&
             !root->findViewByName("graph") &&
             !root->findViewByName("timingAnalytics"),
         "missing remote cards, grids, graph, and analytics consume no views");
  expect(!root->findViewByName("kpoor") &&
             !root->findViewByName("resultJudgementTile:kpoor"),
         "remote skin never fabricates KPOOR descendants");
  const auto texts = descendantTexts(*root);
  expect(std::ranges::none_of(texts,
                              [](const std::string &text) {
                                return text.find("--") != std::string::npos;
                              }),
         "sparse remote skin contains no placeholder dashes");
  expect(std::ranges::none_of(texts,
                              [](const std::string &text) {
                                return text == "0" || text == "0.0%" ||
                                       text == "0.00%";
                              }),
         "missing remote values do not become dummy zero text");
}

void testDefaultSkinSummaryCardsFlexWithoutAbsentSpace() {
  ResultPresentationModel one;
  one.title = "One Card";
  one.scoreComparison = currentOnlyCard("SCORE", "1234");
  const auto oneRoot = buildPresentationLayout(one, 1000, 1080);
  auto *oneRow = oneRoot->findViewByName("resultSummary");
  auto *oneCard = oneRoot->findViewByName("resultSummaryCard:score");
  expect(oneRow && oneCard && oneRow->getChildren().size() == 1 &&
             std::abs(oneCard->getWidth() - oneRow->getWidth()) <= 1,
         "one present summary card expands across the complete row");
  expect(!oneRoot->findViewByName("resultSummaryCard:grade") &&
             !oneRoot->findViewByName("resultSummaryCard:lamp") &&
             !oneRoot->findViewByName("resultSummaryCard:combo"),
         "absent summary cards add no descendant or width consumer");

  ResultPresentationModel three = one;
  three.lampComparison =
      currentOnlyCard("CLEAR LAMP", "NORMAL CLEAR", ui_theme::lime());
  three.comboComparison = currentOnlyCard("COMBO / BREAK", "700");
  const auto threeRoot = buildPresentationLayout(three, 1000, 1080);
  auto *threeRow = threeRoot->findViewByName("resultSummary");
  auto *score = threeRoot->findViewByName("resultSummaryCard:score");
  auto *lamp = threeRoot->findViewByName("resultSummaryCard:lamp");
  auto *combo = threeRoot->findViewByName("resultSummaryCard:combo");
  expect(threeRow && score && lamp && combo &&
             threeRow->getChildren().size() == 3 &&
             std::abs(score->getWidth() - lamp->getWidth()) <= 1 &&
             std::abs(lamp->getWidth() - combo->getWidth()) <= 1 &&
             score->getWidth() < oneCard->getWidth(),
         "multiple present summary cards divide available width evenly");
}

void testDefaultSkinExplicitZerosAndMobileMetadataWrap() {
  auto remote = remoteScore();
  remote.score = 0;
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
  remote.gaugeHistory = {0.0F};
  const auto model = makeRemoteResultPresentation(remote);
  const auto root = buildPresentationLayout(model, 640, 885, true);

  expect(textView(root.get(), "grade") &&
             root->findViewByName("resultSummaryCard:combo") &&
             textView(root.get(), "BP") &&
             textView(root.get(), "BP")->getText() == "0" &&
             textView(root.get(), "pgreat") &&
             textView(root.get(), "pgreat")->getText() == "0" &&
             textView(root.get(), "pgreatFast") &&
             textView(root.get(), "pgreatFast")->getText() == "0" &&
             textView(root.get(), "break") &&
             textView(root.get(), "break")->getText() == "0" &&
             textView(root.get(), "fast") &&
             textView(root.get(), "fast")->getText() == "0" &&
             textView(root.get(), "slow") &&
             textView(root.get(), "slow")->getText() == "0",
         "explicit remote zeros remain rendered in every supplied card or row");
  expect(!root->findViewByName("kpoor"),
         "explicit remote zero totals still do not invent KPOOR");
  expect(!root->findViewByName("timingAnalytics"),
         "remote graph never creates replay analytics from supplied totals");

  auto *grid = root->findViewByName("resultInfoGrid");
  auto *first = root->findViewByName("resultInfoTile:total-notes");
  auto *last = root->findViewByName("resultInfoTile:level");
  expect(grid && first && last && last->getY() > first->getY(),
         "remote metadata tiles wrap to another row at mobile width");
  bool contained = grid != nullptr;
  if (grid != nullptr) {
    for (View *tile : grid->getChildren()) {
      contained = contained && tile->getX() >= grid->getContentX() &&
                  tile->getY() >= grid->getContentY() &&
                  tile->getX() + tile->getWidth() <=
                      grid->getContentX() + grid->getContentWidth() + 1 &&
                  tile->getY() + tile->getHeight() <=
                      grid->getContentY() + grid->getContentHeight() + 1;
    }
  }
  expect(contained,
         "wrapped metadata tiles stay within the responsive information grid");
  expect(textView(root.get(), "resultInfoLabel:service") &&
             textView(root.get(), "resultInfoLabel:input-device") &&
             textView(root.get(), "resultInfoLabel:gauge-type"),
         "remote metadata labels have deterministic semantic names");
}

void testResultTouchControlsHideAndRestorePresentation() {
  const ResultTouchControlAvailability availability{
      .back = true, .retry = true, .retrySame = true, .rankings = true,
      .exportPhoto = true, .selectSection = true, .next = false};
  const auto visible = makeResultTouchControlPresentation(
      {.skinSelected = true, .hidden = false},
      availability);
  expect(visible.showsControls && !visible.capturesRestoreTouch &&
             visible.actions == std::vector<ResultTouchControlAction>{
                                    ResultTouchControlAction::Back,
                                    ResultTouchControlAction::Retry,
                                    ResultTouchControlAction::RetrySame,
                                    ResultTouchControlAction::Rankings,
                                    ResultTouchControlAction::ExportPhoto,
                                    ResultTouchControlAction::SelectSection,
                                    ResultTouchControlAction::Hide},
         "selected touch result skins expose the built-in actions and Hide");

  const auto hidden = makeResultTouchControlPresentation(
      {.skinSelected = true, .hidden = true},
      availability);
  expect(!hidden.showsControls && hidden.capturesRestoreTouch &&
             hidden.actions.empty(),
         "hidden result touch controls consume one anywhere-touch to restore");

  const auto withoutVirtualController = makeResultTouchControlPresentation(
      {.skinSelected = true, .hidden = false}, availability);
  expect(withoutVirtualController.showsControls &&
             !withoutVirtualController.actions.empty(),
         "selected result skins expose touch controls without a virtual controller");
}

void testSelectedResultSkinDefersRootOverlaysUntilAfterSkin() {
  expect(!shouldRenderResultRootAfterSkin(false),
         "built-in results render their root layout before scene rendering");
  expect(shouldRenderResultRootAfterSkin(true),
         "selected result skins render root overlays after the skin");
}

void testSelectedResultSkinKeepsRequiredApplicationOverlays() {
  const auto selected = makeResultSkinApplicationOverlays(
      {.selectedSkin = true,
       .hasPersistenceResult = true,
       .courseStage = true,
       .savedResultBrowsing = false});
  expect(selected.showsPersistenceRecovery &&
             selected.buildsCourseExitConfirmation,
         "selected result skins retain save recovery and unsaved-course exit");

  const auto savedCourse = makeResultSkinApplicationOverlays(
      {.selectedSkin = true,
       .hasPersistenceResult = false,
       .courseStage = true,
       .savedResultBrowsing = true});
  expect(!savedCourse.showsPersistenceRecovery &&
             !savedCourse.buildsCourseExitConfirmation,
         "saved course browsing does not add application recovery overlays");
}

void testResultPhotoExportLabelsDoNotRequireNativeButton() {
  expect(resultPhotoExportLabel(ResultPhotoExportPresentation::Ready) ==
             "Export Photo" &&
             resultPhotoExportLabel(ResultPhotoExportPresentation::Saving) ==
                 "Saving..." &&
             resultPhotoExportLabel(ResultPhotoExportPresentation::Saved) ==
                 "Saved" &&
             resultPhotoExportLabel(ResultPhotoExportPresentation::Failed) ==
                 "Export Failed",
         "touch and native export controls share export status labels");
}

void testResultSkinFailureRestoresAUsableApplicationState() {
  const auto failure = makeResultSkinFailurePresentation(true);
  expect(failure.showNotice && failure.restoreTouchControls,
         "a result skin render failure visibly restores application controls");
  const auto noFailure = makeResultSkinFailurePresentation(false);
  expect(!noFailure.showNotice && !noFailure.restoreTouchControls,
         "a healthy result skin has no failure presentation");
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless bgfx initialization failed\n";
    return 1;
  }
  ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
  testLocalNormalParity();
  testLocalPacemakerAndRecallParity();
  testLocalGasGaugeOrder();
  testSceneAndExporterShareInitialGaugeChoice();
  testLocalFailedFullComboAndCourseOverrides();
  testFullyPopulatedRemotePresentation();
  testRemoteGradeAndPlaytypeDependencies();
  testRemoteJudgementAndTimingOmissions();
  testRemoteIndependentOptionalCardsAndMetadata();
  testRemoteBreakUsesAuthoritativeBadPoints();
  testRemoteMissingVersusExplicitZero();
  testRemoteUnknownLampDoesNotInventPresentation();
  testRemoteGaugeLabelAndLampFallbackSemantics();
  testDefaultSkinLocalPresentationContract();
  testDefaultSkinLegacyNullPresentationParity();
  testDefaultSkinSparseRemoteOmitsUnsupportedViews();
  testDefaultSkinSummaryCardsFlexWithoutAbsentSpace();
  testDefaultSkinExplicitZerosAndMobileMetadataWrap();
  testResultTouchControlsHideAndRestorePresentation();
  testSelectedResultSkinDefersRootOverlaysUntilAfterSkin();
  testSelectedResultSkinKeepsRequiredApplicationOverlays();
  testResultPhotoExportLabelsDoNotRequireNativeButton();
  testResultSkinFailureRestoresAUsableApplicationState();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (failures != 0) {
    std::cerr << failures << " result presentation model test(s) failed\n";
    return 1;
  }
  std::cout << "Result presentation model tests passed\n";
  return 0;
}
