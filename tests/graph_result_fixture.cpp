#include "graph_allocation_guard.h"
#include "CoursePlaySession.h"
#include "ReplayResultStateBuilder.h"

#include <iostream>
#include <limits>
#include <string_view>

namespace {
struct Failure { const char *message; };

void require(bool condition, const char *message) {
  if (!condition) throw Failure{message};
}
}

ASOBMS_GRAPH_RESULT_METHODS

namespace {

void testCoursePadding() {
  CoursePlayEntry entry;
  entry.meta.PlayLength = 2'000'000;
  const auto shortGraph = courseGraphPaddingForEntry(entry, GaugeType::Hard);
  require(shortGraph.chart->normalDistribution.size() == 4 &&
              shortGraph.dynamic->judgementDistribution.size() == 3 &&
              shortGraph.dynamic->earlyLateDistribution.size() == 3 &&
              shortGraph.dynamic->gaugeHistories.front().size() == 5,
          "actual short course padding must preserve all source shapes");
  entry.meta.PlayLength = std::numeric_limits<long long>::max();
  const auto distant = courseGraphPaddingForEntry(entry, GaugeType::Hard);
  require(distant.chart->normalDistribution.empty() &&
              distant.dynamic->judgementDistribution.empty() &&
              distant.dynamic->earlyLateDistribution.empty() &&
              distant.chart->judgementDistributionSeconds == 9'223'372'036'855ULL,
          "course padding must admit before conversion or dense allocation");
  for (const auto &history : distant.dynamic->gaugeHistories) {
    require(history.empty(), "overlong course padding must not create a gauge prefix");
  }
}

void testCourseFallback() {
  bms_parser::Chart chart;
  RhythmState state(&chart, false);
  state.gaugeHistory = {20.0F, 80.0F};
  CoursePlaySession session;
  session.entries.resize(1);
  session.completedResults.emplace_back(chart.Meta, state);
  auto graphChart = std::make_shared<SkinGameplayChartGraphState>();
  auto graphDynamic = std::make_shared<SkinGameplayDynamicGraphState>();
  session.completedResults.front().gameplayGraph =
      {.chart = graphChart, .dynamic = graphDynamic};
  const auto legacy = courseGameplayGraphForSession(session, state);
  require(legacy.dynamic->gaugeHistories[gaugeTypeIndex(state.gaugeType)] ==
              state.gaugeHistory,
          "admitted legacy result without sampled graph retains event fallback");

  session.completedResults.clear();
  session.entries.front().meta.PlayLength = 5'000'000'000LL;
  const auto omitted = courseGameplayGraphForSession(session, state);
  for (const auto &history : omitted.dynamic->gaugeHistories) {
    require(history.empty(), "course fallback must not resurrect deliberately omitted trace");
  }
  require(omitted.dynamic->gaugeHistorySections.empty() &&
              state.gaugeHistory == std::vector<float>({20.0F, 80.0F}),
          "omitted course trace has no false separators and retains durable input");
  session.completedResults.emplace_back(session.entries.front().meta, state);
  const auto completedWithoutGraph = courseGameplayGraphForSession(session, state);
  require(completedWithoutGraph.chart != nullptr &&
              completedWithoutGraph.dynamic != nullptr &&
              completedWithoutGraph.chart->judgementDistributionSeconds == 5001 &&
              completedWithoutGraph.dynamic->gaugeHistories[gaugeTypeIndex(state.gaugeType)].empty(),
          "completed legacy stage still supplies truthful duration and overlong omission");
}

void testSyntheticPaddingAdmission() {
  RhythmState state(nullptr, false);
  state.gaugeHistory = {20.0F, 80.0F};
  CoursePlaySession session;
  session.entries.resize(2);
  session.entries[0].meta.PlayLength = 1'000'000;
  session.entries[1].meta.PlayLength = 500'000'000'000'000LL;
  appendMissingCourseGaugeHistory(state, session, 0);
  require(state.gaugeHistory == std::vector<float>({20.0F, 80.0F}),
          "synthetic admission must preserve real events without appending a partial tail");
  session.entries.resize(1);
  appendMissingCourseGaugeHistory(state, session, 0);
  require(state.gaugeHistory == std::vector<float>({20.0F, 80.0F, 0.0F, 0.0F, 0.0F}),
          "short missing-stage synthetic samples retain their original convention");
}

void testReplayCopies() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  chart.Measures.push_back(measure);
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 1'000'000;
  timeline->Bpm = 120.0;
  timeline->SetNote(1, new bms_parser::Note(1));
  measure->TimeLines.push_back(timeline);
  RhythmState state(&chart, false);
  state.gaugeHistory = {20.0F, 80.0F};
  state.gaugeHistoryFor(state.gaugeType) = state.gaugeHistory;
  ReplayData replay;
  const auto legacy = replay_result::BuildSkinGameplayChartGraphState(chart, state);
  const auto replayGraph = replay_result::BuildSkinGameplayGraphState(chart, replay, state);
  require(legacy.dynamic->gaugeHistories[gaugeTypeIndex(state.gaugeType)] ==
              state.gaugeHistory &&
              replayGraph.dynamic->gaugeHistories[gaugeTypeIndex(state.gaugeType)] ==
                  state.gaugeHistory,
          "actual replay and replay-less admitted graphs preserve legacy history");
  state.gaugeHistoryFor(GaugeType::Hard).assign(4097, 30.0F);
  for (const auto &graph : {
           replay_result::BuildSkinGameplayChartGraphState(chart, state),
           replay_result::BuildSkinGameplayGraphState(chart, replay, state)}) {
    for (const auto &history : graph.dynamic->gaugeHistories) {
      require(history.empty(), "all graph copy channels must obey admission");
    }
  }
  require(state.gaugeHistoryFor(GaugeType::Hard).size() == 4097 &&
              state.gaugeHistory.size() == 2,
          "display copy admission must not truncate durable event histories");
  state.gaugeHistoryFor(GaugeType::Hard).clear();
  timeline->Timing = 100'000'000'000'000LL;
  for (const auto &graph : {
           replay_result::BuildSkinGameplayChartGraphState(chart, state),
           replay_result::BuildSkinGameplayGraphState(chart, replay, state)}) {
    require(graph.dynamic->gaugeHistories[gaugeTypeIndex(state.gaugeType)].empty(),
            "overlong replay graph must not fall back to a misleading event trace");
  }
}

void testCombinedOffsets() {
  auto chart = std::make_shared<SkinGameplayChartGraphState>();
  auto dynamic = std::make_shared<SkinGameplayDynamicGraphState>();
  chart->judgementDistributionSeconds = 2;
  chart->bpmSeries = {{.chartTimeMicros = 1'000'000, .bpm = 120.0}};
  const std::array<SkinGameplayGraphState, 2> stages{{
      {.chart = chart, .dynamic = dynamic}, {.chart = chart, .dynamic = dynamic}}};
  const auto ordinary = combineSkinGameplayGraphStates(stages);
  require(ordinary.chart->bpmSeries.size() == 2 &&
              ordinary.chart->bpmSeries.back().chartTimeMicros == 3'000'000 &&
              ordinary.chart->judgementDistributionSeconds == 4,
          "ordinary combined time offsets must stay exact");
  chart->judgementDistributionSeconds = 9'223'372'036'855ULL;
  const auto distant = combineSkinGameplayGraphStates(stages);
  require(distant.chart->bpmSeries.empty() &&
              distant.chart->judgementDistributionSeconds == 18'446'744'073'710ULL,
          "unrepresentable combined microsecond axis must be omitted, not wrapped");
  chart->judgementDistributionSeconds = std::numeric_limits<std::uint64_t>::max();
  const auto overflow = combineSkinGameplayGraphStates(stages);
  require(overflow.chart->durationUnavailable &&
              overflow.chart->judgementDistributionSeconds == 0 &&
              overflow.chart->bpmSeries.empty() &&
              !skinGameplayGraphDurationMicros(*overflow.chart),
          "unrepresentable logical sum must expose unavailable duration, not wrap");
  chart->judgementDistributionSeconds = 2;
  chart->bpmSeries.front().chartTimeMicros =
      std::numeric_limits<std::int64_t>::max() - 500'000;
  const auto pointOverflow = combineSkinGameplayGraphStates(stages);
  require(pointOverflow.chart->bpmSeries.empty() &&
              pointOverflow.chart->bpmSeriesOmitted &&
              pointOverflow.chart->judgementDistributionSeconds == 4,
          "point offset addition must not overflow even with representable duration");
}

void testPartialGaugeChannels() {
  auto chart = std::make_shared<SkinGameplayChartGraphState>();
  chart->judgementDistributionSeconds = 2;
  auto first = std::make_shared<SkinGameplayDynamicGraphState>();
  auto second = std::make_shared<SkinGameplayDynamicGraphState>();
  first->gaugeHistories[gaugeTypeIndex(GaugeType::Normal)] = {20.0F, 50.0F};
  second->gaugeHistories[gaugeTypeIndex(GaugeType::Normal)] = {50.0F, 80.0F};
  second->gaugeHistories[gaugeTypeIndex(GaugeType::Hard)] = {100.0F, 90.0F};
  const std::array<SkinGameplayGraphState, 2> stages{{
      {.chart = chart, .dynamic = first}, {.chart = chart, .dynamic = second}}};
  const auto combined = combineSkinGameplayGraphStates(stages);
  for (const auto &history : combined.dynamic->gaugeHistories) {
    require(history.empty(), "one incomplete gauge channel must not acquire a compressed axis");
  }
  require(combined.dynamic->gaugeHistoryOmitted,
          "partial channel omission must survive result fallback");
}

}

int main() {
  struct TestCase { std::string_view name; void (*run)(); };
  const TestCase cases[] = {{"padding", testCoursePadding},
                            {"fallback", testCourseFallback},
                            {"synthetic-padding", testSyntheticPaddingAdmission},
                            {"replay-copies", testReplayCopies},
                            {"offsets", testCombinedOffsets},
                            {"partial-channels", testPartialGaugeChannels}};
  bool passed = true;
  for (const auto &test : cases) {
    try {
      graph_test::AllocationGuard guard;
      test.run();
    } catch (const Failure &failure) {
      std::cerr << "FAIL " << test.name << ": " << failure.message << '\n';
      passed = false;
    } catch (const std::exception &error) {
      std::cerr << "FAIL " << test.name << ": " << error.what()
                << "; rejected bytes=" << graph_test::rejectedAllocationBytes << '\n';
      passed = false;
    }
  }
  return passed ? 0 : 1;
}
