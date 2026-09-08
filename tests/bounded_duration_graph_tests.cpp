#include "graph_allocation_guard.h"
#include "bms_parser.hpp"
#include "scene/play/PlayfieldChartVisualModel.h"
#include "scene/play/SkinGameplayGraphState.h"

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace {

struct Failure {
  const char *message;
};

void require(bool condition, const char *message) {
  if (!condition) throw Failure{message};
}

bms_parser::TimeLine *appendTimeline(bms_parser::Measure &measure,
                                   long long timing, double bpm) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timing;
  timeline->BeatPosition = measure.TimeLines.size() * 4.0;
  timeline->Bpm = bpm;
  measure.TimeLines.push_back(timeline);
  return timeline;
}

void testShortChart() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.Bpm = 120.0;
  auto *measure = new bms_parser::Measure();
  chart.Measures.push_back(measure);
  auto *first = appendTimeline(*measure, 0, 120.0);
  first->SetNote(0, new bms_parser::Note(1));
  first->SetNote(7, new bms_parser::Note(2));
  auto *headTimeline = appendTimeline(*measure, 1'000'000, 120.0);
  auto *tailTimeline = appendTimeline(*measure, 3'000'000, 180.0);
  auto *head = new bms_parser::LongNote(3, bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(3, bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(1, head);
  tailTimeline->SetNote(1, tail);
  tailTimeline->SetNote(7, new bms_parser::Note(4));
  tailTimeline->SetInvisibleNote(2, new bms_parser::Note(5));
  tailTimeline->SetLandmineNote(3, new bms_parser::LandmineNote(5.0F));

  const auto model = buildPlayfieldChartVisualModel(chart, 1);
  const std::vector<SkinNormalDistribution> expected{
      {0, 0, 1, 0, 0, 1, 0},
      {0, 0, 0, 1, 0, 0, 0},
      {0, 0, 0, 0, 1, 0, 0},
      {0, 0, 1, 0, 1, 0, 1},
      {0, 0, 0, 0, 0, 0, 0}};
  require(model.skinGameplayGraph.normalDistribution == expected,
          "short graph must retain lane buckets, LN body, final bin and padding");
  require(model.skinGameplayGraph.judgementDistributionSeconds == 4,
          "short judgement duration must exclude the normal padding bin");
  SkinGameplayGraphAccumulator accumulator(
      model.skinGameplayGraph.judgementNotes, 4, {}, 4);
  require(accumulator.state().judgementDistribution[0][0] == 2 &&
              accumulator.state().judgementDistribution[1][0] == 1 &&
              accumulator.state().judgementDistribution[3][0] == 1,
          "short graph must exclude invisible, mine and classic tail judgements");
}

void testAdmissionBoundary() {
  for (const auto finalSecond : {1636LL, 1637LL, 5000LL}) {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 7;
    auto *measure = new bms_parser::Measure();
    chart.Measures.push_back(measure);
    appendTimeline(*measure, finalSecond * 1'000'000, 120.0)
        ->SetNote(0, new bms_parser::Note(1));
    const auto model = buildPlayfieldChartVisualModel(chart, 1);
    if (finalSecond == 1636) {
      require(model.skinGameplayGraph.normalDistribution.size() == 1638 &&
                  model.skinGameplayGraph.normalDistribution[1636][5] == 1 &&
                  model.skinGameplayGraph.normalDistribution[1637] ==
                      SkinNormalDistribution{},
              "last admitted second must retain its note and padding");
    } else {
      require(model.skinGameplayGraph.normalDistribution.empty(),
              "unrenderable duration must use empty distribution admission");
    }
    require(model.notes.size() == 1 && model.timelines.back().timeMicros ==
                                           finalSecond * 1'000'000,
            "graph admission must not truncate the playable chart");
  }
}

void testDistantFullModel() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 1;
  chart.Meta.Total = 100.0;
  auto *measure = new bms_parser::Measure();
  chart.Measures.push_back(measure);
  appendTimeline(*measure, 0, 120.0);
  auto *distant = appendTimeline(*measure, 100'000'000'000'000LL, 180.0);
  distant->Scroll = 2.0;
  distant->SetNote(0, new bms_parser::Note(1));

  const auto model = buildPlayfieldChartVisualModel(chart, 1);
  require(model.skinGameplayGraph.normalDistribution.empty(),
          "distant full model must not allocate dense seconds");
  const auto &information = model.staticMetadata.songInformation;
  require(information.has_value() &&
              std::abs(information->density - 1.0 / 100'000'002.0) < 1e-9 &&
              std::abs(information->peakDensity - 1.0) < 1e-6 &&
              std::abs(information->endDensity - 0.2) < 1e-6,
          "distant full model must preserve existing sparse summary semantics");
  const auto before = chartVisualTimelineAuthorityAtTime(
      model, 99'999'999'999'999LL);
  const auto at = chartVisualTimelineAuthorityAtTime(
      model, 100'000'000'000'000LL);
  require(before.bpm == 120.0 && before.scrollRate == 1.0 &&
              at.bpm == 180.0 && at.scrollRate == 2.0 &&
              model.notes.size() == 1,
          "bounded graph must retain distant timeline lookup authority");
}

void testParserFullModel() {
  std::atomic_bool cancelled{false};
  bms_parser::Parser parser;
  bms_parser::Chart *parsed = nullptr;
  parser.Parse(std::filesystem::path(GRAPH_FIXTURE_ROOT) /
                   "graph_distant_timeline.bms",
               &parsed, false, false, cancelled);
  std::unique_ptr<bms_parser::Chart> chart(parsed);
  require(chart != nullptr, "low positive BPM fixture must remain parseable");
  const auto model = buildPlayfieldChartVisualModel(*chart, 1);
  require(model.notes.size() == 1 && !model.timelines.empty() &&
              model.timelines.back().timeMicros >= 239'999'999'000'000LL,
          "parser/full-model path must preserve the distant playable note");
  require(model.skinGameplayGraph.normalDistribution.empty(),
          "selector's parser/full-model graph producer must remain bounded");
  auto chartGraph = std::make_shared<SkinGameplayChartGraphState>(
      model.skinGameplayGraph);
  const auto view = skinGameplayGraphStateView({.chart = chartGraph});
  require(view.normalDistribution.empty() && !view.bpmSeries.empty(),
          "consumer view must preserve BPM metadata when distribution is omitted");
}

void testDistantLongNote() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 1;
  chart.Meta.Total = 100.0;
  auto *measure = new bms_parser::Measure();
  chart.Measures.push_back(measure);
  auto *headTimeline = appendTimeline(*measure, 0, 120.0);
  auto *tailTimeline = appendTimeline(*measure, 100'000'000'000'000LL, 180.0);
  auto *head = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(7, head);
  tailTimeline->SetNote(7, tail);
  const auto model = buildPlayfieldChartVisualModel(chart, 1);
  require(model.skinGameplayGraph.normalDistribution.empty() &&
              model.notes.size() == 2 &&
              model.notes.front().pairId == model.notes.back().id &&
              model.notes.back().pairId == model.notes.front().id,
          "omitted graph must skip elapsed-second LN work without breaking pairs");
}

void testDynamicAdmissionAndLateJudge() {
  SkinGameplayGraphAccumulator accumulator(
      {{.sourceId = 7, .second = 100'000'000, .countsTowardJudgement = true}},
      100'000'001, {}, 4);
  require(accumulator.state().judgementDistribution.empty() &&
              accumulator.state().earlyLateDistribution.empty(),
          "both dynamic channels must obey duration admission");
  accumulator.applyJudge(7, JudgeResult(Great, 12'000));
  require(accumulator.state().judgementDistribution.empty() &&
              accumulator.state().earlyLateDistribution.empty(),
          "late judgement must not expand omitted distributions");
  require(accumulator.state().recentJudgeTimingIndex == 1 &&
              accumulator.state().recentJudgeTimingsMillis[1] == -12 &&
              accumulator.state().judgementRevision != 0,
          "late judge must still update timing consumers without distribution bins");
  accumulator.reset(
      {{.sourceId = 8, .second = 1, .countsTowardJudgement = true}},
      2, {}, 4);
  accumulator.applyJudge(8, JudgeResult(Great, 12'000));
  require(accumulator.state().judgementDistribution[1][2] == 1 &&
              accumulator.state().judgementDistribution[1][0] == 0 &&
              accumulator.state().earlyLateDistribution[1][6] == 1,
          "short reset after oversized graph must restore exact judge bins");
}

void testMaximumCountAdmission() {
  SkinGameplayGraphAccumulator accumulator(
      {}, std::numeric_limits<std::size_t>::max(), {}, 0);
  require(accumulator.state().judgementDistribution.empty() &&
              accumulator.state().earlyLateDistribution.empty(),
          "maximum count must be rejected before vector sizing");
}

void testGaugeCapacityAndLateUpdate() {
  SkinGameplayGraphAccumulator accumulator(
      {}, 100'000'001, {}, std::numeric_limits<std::size_t>::max());
  std::array<float, kGaugeTypeCount> values;
  values.fill(42.0F);
  GameplayGaugeRules rules;
  rules.compiled = true;
  rules.gauges[gaugeTypeIndex(GaugeType::Normal)].maximum = 100.0F;
  rules.gauges[gaugeTypeIndex(GaugeType::Hard)].maximum = 100.0F;
  (void)accumulator.updateGaugeState(values, GaugeType::Normal, rules);
  (void)accumulator.advanceGaugeHistoryTo(
      std::numeric_limits<std::int64_t>::max());
  for (const auto &history : accumulator.state().gaugeHistories) {
    require(history.empty() && history.capacity() <= 4096,
            "overlong gauge channels must omit the trace, not show a prefix");
  }
  values.fill(55.0F);
  (void)accumulator.updateGaugeState(values, GaugeType::Hard, rules);
  (void)accumulator.advanceGaugeHistoryTo(
      std::numeric_limits<std::int64_t>::max());
  require(accumulator.state().gaugeHistories.front().empty() &&
              accumulator.state().gaugeType == GaugeType::Hard &&
              accumulator.state().gaugeSupported &&
              accumulator.state().gaugeMaximum == 100.0F,
          "omitted history stays empty while live gauge properties update");

  accumulator.reset({}, 2, {}, 4);
  (void)accumulator.updateGaugeState(values, GaugeType::Normal, rules);
  (void)accumulator.advanceGaugeHistoryTo(1'000'000);
  require(accumulator.state().gaugeHistories.front() ==
              std::vector<float>({55.0F, 55.0F, 55.0F}),
          "short admitted gauge preserves exact half-second samples");
  (void)accumulator.advanceGaugeHistoryTo(3'000'000);
  for (const auto &history : accumulator.state().gaugeHistories) {
    require(history.empty(),
            "unexpected late catch-up must invalidate, not freeze a partial trace");
  }
}

void testCombinedAdmission() {
  auto chart = std::make_shared<SkinGameplayChartGraphState>();
  chart->normalDistribution.assign(1001, {});
  chart->normalDistribution[999][5] = 1;
  chart->judgementDistributionSeconds = 1000;
  auto dynamic = std::make_shared<SkinGameplayDynamicGraphState>();
  dynamic->judgementDistribution.assign(1000, {});
  dynamic->earlyLateDistribution.assign(1000, {});
  const std::array<SkinGameplayGraphState, 2> stages{{
      {.chart = chart, .dynamic = dynamic},
      {.chart = chart, .dynamic = dynamic}}};
  const auto combined = combineSkinGameplayGraphStates(stages);
  require(combined.chart != nullptr && combined.dynamic != nullptr &&
              combined.chart->normalDistribution.empty() &&
              combined.dynamic->judgementDistribution.empty() &&
              combined.dynamic->earlyLateDistribution.empty(),
          "concatenation must apply aggregate admission before growing channels");
  require(combined.chart->judgementDistributionSeconds == 2000,
          "omitted course distribution must not shorten the time axis");
}

}

int main(int argc, char **argv) {
  struct TestCase {
    std::string_view name;
    void (*run)();
  };
  const TestCase cases[] = {
      {"short", testShortChart},
      {"boundary", testAdmissionBoundary},
      {"distant", testDistantFullModel},
      {"parser", testParserFullModel},
      {"long-note", testDistantLongNote},
      {"dynamic", testDynamicAdmissionAndLateJudge},
      {"maximum", testMaximumCountAdmission},
      {"gauge", testGaugeCapacityAndLateUpdate},
      {"combined", testCombinedAdmission}};
  bool selected = false;
  bool passed = true;
  for (const auto &test : cases) {
    if (argc > 1 && test.name != argv[1]) continue;
    selected = true;
    try {
      graph_test::AllocationGuard guard;
      test.run();
      std::cout << "PASS " << test.name << '\n';
    } catch (const Failure &failure) {
      std::cerr << "FAIL " << test.name << ": " << failure.message << '\n';
      passed = false;
    } catch (const std::bad_alloc &) {
      std::cerr << "FAIL " << test.name << ": guarded allocation rejected "
                << graph_test::rejectedAllocationBytes << " bytes\n";
      passed = false;
    } catch (const std::exception &error) {
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
      passed = false;
    }
  }
  return selected && passed ? 0 : 1;
}
