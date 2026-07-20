#include "practice/PracticeAnalytics.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void requireNear(double expected, double actual, double tolerance,
                 const char *message) {
  if (std::fabs(expected - actual) > tolerance) {
    std::cerr << message << ": expected " << expected << ", actual " << actual
              << '\n';
    std::exit(1);
  }
}

bms_parser::TimeLine *addTimeline(bms_parser::Measure &measure,
                                  long long timingMicros) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  measure.TimeLines.push_back(timeline);
  return timeline;
}

void addNote(bms_parser::TimeLine &timeline, int lane) {
  auto *note = new bms_parser::Note(1);
  note->Lane = lane;
  note->Timeline = &timeline;
  timeline.Notes.push_back(note);
}

void addLongNoteTail(bms_parser::TimeLine &timeline, int lane) {
  auto *note =
      new bms_parser::LongNote(1, bms_parser::LongNoteType::ChargeNote);
  note->Lane = lane;
  note->Timeline = &timeline;
  timeline.Notes.push_back(note);
}

bms_parser::Chart makeChart() {
  bms_parser::Chart chart;
  chart.Meta.TotalLength = 3'000'000;
  chart.Meta.PlayLength = 2'000'000;
  chart.Meta.SHA256 = std::string(64, 'a');
  chart.Meta.MD5 = std::string(32, 'b');

  auto *measure0 = new bms_parser::Measure();
  measure0->Timing = 0;
  addNote(*addTimeline(*measure0, 0), 3);
  addNote(*addTimeline(*measure0, 200'000), 1);
  chart.Measures.push_back(measure0);

  auto *measure1 = new bms_parser::Measure();
  measure1->Timing = 1'000'000;
  addLongNoteTail(*addTimeline(*measure1, 1'000'000), 3);
  addNote(*addTimeline(*measure1, 1'500'000), 2);
  chart.Measures.push_back(measure1);

  auto *measure2 = new bms_parser::Measure();
  measure2->Timing = 2'000'000;
  addNote(*addTimeline(*measure2, 2'000'000), 1);
  chart.Measures.push_back(measure2);
  return chart;
}

std::vector<JudgeWindowProvenance> windows(long long pGreatEarly = -10'000) {
  return {
      {.judgement = PGreat,
       .earlyMicros = pGreatEarly,
       .lateMicros = 10'000},
      {.judgement = Great, .earlyMicros = -30'000, .lateMicros = 30'000},
      {.judgement = Good, .earlyMicros = -75'000, .lateMicros = 75'000},
      {.judgement = Bad, .earlyMicros = -330'000, .lateMicros = 420'000},
      {.judgement = Kpoor,
       .earlyMicros = -500'000,
       .lateMicros = 150'000},
  };
}

ScoreStageProvenance
provenanceStage(std::string sha256, std::string md5,
                std::vector<JudgeWindowProvenance> effectiveWindows) {
  return {
      .chartMd5 = std::move(md5),
      .chartSha256 = std::move(sha256),
      .effectiveJudgeWindows = std::move(effectiveWindows),
  };
}

ReplayData makeReplay() {
  ReplayData replay;
  replay.provenance.playback = {.percent = 75,
                                .mode = audio::PlaybackMode::PitchShift};
  replay.provenance.judgeWindowScalePercent = 80;
  replay.provenance.stages = {{
      .chartMd5 = std::string(32, 'b'),
      .chartSha256 = std::string(64, 'a'),
      .effectiveJudgeWindows = windows(),
  }};
  return replay;
}

ReplayEvent event(ReplayEventAction action, int lane, long long noteMicros,
                  Judgement judgement, long long diffMicros) {
  return {.action = action,
          .lane = lane,
          .noteTimeMicros = noteMicros,
          .judgement = judgement,
          .diffMicros = diffMicros};
}

void testTimingAndBreakdowns() {
  auto chart = makeChart();
  auto replay = makeReplay();
  replay.events = {
      event(ReplayEventAction::Press, 3, 0, Great, -15'000),
      event(ReplayEventAction::Press, 1, 200'000, PGreat, 3'750),
      event(ReplayEventAction::Release, 3, 1'000'000, Good, 7'500),
      event(ReplayEventAction::Press, 2, 1'500'000, Bad, -3'750),
      event(ReplayEventAction::Miss, 1, 2'000'000, Poor, 400'000),
      event(ReplayEventAction::Mine, 3, 0, Poor, 50'000),
      event(ReplayEventAction::Press, 4, -1, Great, 15'000),
      event(ReplayEventAction::Press, 3, 0, None, 25'000),
      event(ReplayEventAction::Press, 3, 0, Kpoor, 35'000),
      event(ReplayEventAction::Press, 7, 900'000, Great, 45'000),
      event(ReplayEventAction::Miss, 3, 0, None, 400'000),
      event(ReplayEventAction::Miss, 2, 1'500'000, Kpoor, 400'000),
      event(ReplayEventAction::Miss, 3, 1'000'000, Great, 400'000),
  };

  const practice::Analysis analysis = practice::analyze(chart, replay);
  require(analysis.overall.samples == 4, "only judged note events are samples");
  require(analysis.overall.early == 2 && analysis.overall.late == 2,
          "signed samples count early and late");
  require(analysis.overall.misses == 1, "miss actions are counted");
  require(analysis.overall.meanMillis.has_value(), "mean is present");
  requireNear(-2.5, *analysis.overall.meanMillis, 0.000001,
              "signed mean uses real milliseconds");
  requireNear(std::sqrt(131.25), *analysis.overall.standardDeviationMillis,
              0.000001, "population standard deviation");
  requireNear(0.0, *analysis.overall.medianMillis, 0.000001, "median");

  const std::vector<std::pair<int, std::size_t>> expectedBins = {
      {-20, 1},
      {-5, 1},
      {5, 1},
      {10, 1},
  };
  require(analysis.histogram.size() == expectedBins.size(),
          "histogram reports occupied five millisecond bins");
  for (std::size_t index = 0; index < expectedBins.size(); ++index) {
    require(analysis.histogram[index].lowerMillis == expectedBins[index].first,
            "histogram lower boundary");
    require(analysis.histogram[index].upperMillis ==
                expectedBins[index].first + 5,
            "histogram upper boundary");
    require(analysis.histogram[index].count == expectedBins[index].second,
            "histogram count");
  }

  require(analysis.lanes.size() == 3, "only analyzed lanes are emitted");
  require(analysis.lanes[0].lane == 1 && analysis.lanes[1].lane == 2 &&
              analysis.lanes[2].lane == 3,
          "lanes use deterministic ascending order");
  requireNear(5.0, *analysis.lanes[0].timing.meanMillis, 0.000001,
              "lane one timing bias");
  require(analysis.lanes[0].timing.misses == 1, "lane one miss count");
  requireNear(-5.0, *analysis.lanes[1].timing.meanMillis, 0.000001,
              "lane two timing bias");
  requireNear(-5.0, *analysis.lanes[2].timing.meanMillis, 0.000001,
              "lane three timing bias");
  require(analysis.lanes[1].timing.misses == 0 &&
              analysis.lanes[2].timing.misses == 0,
          "invalid miss judgements do not affect lane miss counts");

  require(analysis.sections.size() == 3,
          "one deterministic section is emitted per measure");
  require(analysis.sections[0].firstMeasure == 0 &&
              analysis.sections[0].lastMeasure == 0 &&
              analysis.sections[0].startMicros == 0 &&
              analysis.sections[0].endMicros == 1'000'000,
          "first measure boundaries");
  require(analysis.sections[1].startMicros == 1'000'000 &&
              analysis.sections[1].endMicros == 2'000'000 &&
              analysis.sections[1].timing.samples == 2,
          "an event on a measure boundary maps to the next measure");
  requireNear(0.5, analysis.sections[1].badMissRate, 0.000001,
              "bad and miss rate includes bad judgements");
  requireNear(0.75, *analysis.sections[0].accuracy, 0.000001,
              "section accuracy uses EX-score weighting");
  requireNear(0.0, *analysis.sections[1].accuracy, 0.000001,
              "good and bad judgements earn no EX-score points");
  require(analysis.sections[0].timing.misses == 0 &&
              analysis.sections[1].timing.misses == 0,
          "invalid miss judgements do not affect section miss counts");
  require(analysis.sections[2].startMicros == 2'000'000 &&
              analysis.sections[2].endMicros == 3'000'000 &&
              analysis.sections[2].timing.samples == 0 &&
              analysis.sections[2].timing.misses == 1,
          "last measure uses chart end and receives boundary miss");
  requireNear(1.0, analysis.sections[2].badMissRate, 0.000001,
              "miss-only section has a finite rate");
  requireNear(0.0, *analysis.sections[2].accuracy, 0.000001,
              "miss-only section has zero accuracy");
}

void testEmptyAnalysisHasNullStatistics() {
  auto chart = makeChart();
  const auto analysis = practice::analyze(chart, makeReplay());
  require(analysis.overall.samples == 0 && analysis.overall.misses == 0,
          "empty replay has zero counts");
  require(!analysis.overall.meanMillis.has_value() &&
              !analysis.overall.standardDeviationMillis.has_value() &&
              !analysis.overall.medianMillis.has_value(),
          "empty replay statistics are nullopt");
  require(analysis.histogram.empty() && analysis.lanes.empty(),
          "empty replay has no bins or lanes");
  for (const auto &section : analysis.sections) {
    require(!section.timing.meanMillis.has_value() &&
                !section.accuracy.has_value() &&
                std::isfinite(section.badMissRate) &&
                section.badMissRate == 0.0,
            "empty sections never expose NaN");
  }
}

void testHistogramUsesSparseStableBins() {
  auto chart = makeChart();
  auto replay = makeReplay();
  replay.provenance.playback.percent = 100;
  replay.events = {
      event(ReplayEventAction::Press, 3, 0, Great, -1'000'000),
      event(ReplayEventAction::Press, 3, 0, Great, -1),
      event(ReplayEventAction::Press, 3, 0, Great, 1),
      event(ReplayEventAction::Press, 3, 0, Great, 1'000'000),
  };

  const auto analysis = practice::analyze(chart, replay);
  const std::vector<std::pair<int, std::size_t>> expectedBins = {
      {-1000, 1}, {-5, 1}, {0, 1}, {1000, 1}};
  require(analysis.histogram.size() == expectedBins.size(),
          "histogram storage scales with occupied bins, not timing span");
  for (std::size_t index = 0; index < expectedBins.size(); ++index) {
    require(analysis.histogram[index].lowerMillis ==
                    expectedBins[index].first &&
                analysis.histogram[index].upperMillis ==
                    expectedBins[index].first + 5 &&
                analysis.histogram[index].count == expectedBins[index].second,
            "wide and sub-millisecond samples use stable floor-aligned bins");
  }
}

void testHistogramSeparatesOverflowFromFiniteEndpointBins() {
  constexpr int lowestFiniteLower =
      std::numeric_limits<int>::min() - std::numeric_limits<int>::min() % 5;
  constexpr int highestFiniteLower =
      (std::numeric_limits<int>::max() - 5) / 5 * 5;

  auto chart = makeChart();
  auto replay = makeReplay();
  replay.provenance.playback.percent = 100;
  replay.events = {
      event(ReplayEventAction::Press, 3, 0, Great,
            std::numeric_limits<long long>::min()),
      event(ReplayEventAction::Press, 3, 0, Great,
            static_cast<long long>(lowestFiniteLower) * 1'000),
      event(ReplayEventAction::Press, 3, 0, Great,
            static_cast<long long>(highestFiniteLower) * 1'000),
      event(ReplayEventAction::Press, 3, 0, Great,
            std::numeric_limits<long long>::max()),
  };

  const auto analysis = practice::analyze(chart, replay);
  require(analysis.histogramLowerOverflow == 1 &&
              analysis.histogramUpperOverflow == 1,
          "unrepresentable timing bins use explicit overflow counts");
  require(analysis.histogram.size() == 2,
          "overflow samples cannot collide with finite endpoint bins");
  require(analysis.histogram[0].lowerMillis == lowestFiniteLower &&
              analysis.histogram[0].upperMillis == lowestFiniteLower + 5 &&
              analysis.histogram[0].count == 1,
          "lowest representable finite bin remains distinct");
  require(analysis.histogram[1].lowerMillis == highestFiniteLower &&
              analysis.histogram[1].upperMillis == highestFiniteLower + 5 &&
              analysis.histogram[1].count == 1,
          "highest representable finite bin remains distinct");
}

void testCompatibleAttemptGroups() {
  auto chart = makeChart();
  auto first = makeReplay();
  first.events.push_back(event(ReplayEventAction::Press, 3, 0, Great, -15'000));
  auto compatible = first;
  compatible.events.front().diffMicros = 7'500;
  auto differentRate = first;
  differentRate.provenance.playback.percent = 100;
  auto differentScale = first;
  differentScale.provenance.judgeWindowScalePercent = 100;
  auto differentWindows = first;
  differentWindows.provenance.stages.front().effectiveJudgeWindows =
      windows(-10'001);
  auto differentMode = first;
  differentMode.provenance.playback.mode = audio::PlaybackMode::TimeStretch;

  const std::vector<ReplayData> attempts = {
      first,          compatible,       differentRate,
      differentScale, differentWindows, differentMode};
  const auto groups = practice::analyzeCompatibleAttempts(chart, attempts);
  require(groups.size() == 5,
          "rate, mode, scale, and exact windows split groups");
  require(groups[0].attemptIndices == std::vector<std::size_t>({0, 1}) &&
              groups[1].attemptIndices == std::vector<std::size_t>({2}) &&
              groups[2].attemptIndices == std::vector<std::size_t>({3}) &&
              groups[3].attemptIndices == std::vector<std::size_t>({4}) &&
              groups[4].attemptIndices == std::vector<std::size_t>({5}),
          "groups and attempt indices retain stable input order");
  require(groups[0].aggregate.overall.samples == 2,
          "compatible attempts aggregate their samples");
  requireNear(-5.0, *groups[0].aggregate.overall.meanMillis, 0.000001,
              "aggregate converts each chart delta through group playback");
  require(groups[0].conditions.playback.percent == 75 &&
              groups[0].conditions.judgeWindowScalePercent == 80 &&
              groups[0].conditions.effectiveJudgeWindows == windows(),
          "group reports exact recorded timing conditions");
}

void testTimingConditionsUseStrictDurableStageIdentity() {
  auto chart = makeChart();
  auto caseVariant = makeReplay();
  caseVariant.provenance.stages = {
      provenanceStage(std::string(64, 'A'), std::string(32, 'B'),
                      windows(-11'000)),
      provenanceStage(std::string(64, 'c'), std::string(32, 'd'),
                      windows(-12'000)),
  };
  const std::vector<ReplayData> caseAttempts = {caseVariant};
  const auto caseGroups =
      practice::analyzeCompatibleAttempts(chart, caseAttempts);
  require(caseGroups.size() == 1 &&
              caseGroups.front().conditions.windowResolution ==
                  practice::TimingWindowResolution::Resolved &&
              caseGroups.front().conditions.effectiveJudgeWindows ==
                  windows(-11'000),
          "durable stage hashes match case-insensitively");

  auto conflictingSha = makeReplay();
  conflictingSha.provenance.stages = {
      provenanceStage(std::string(64, 'c'), std::string(32, 'b'),
                      windows(-13'000)),
      provenanceStage(std::string(64, 'd'), std::string(32, 'e'),
                      windows(-14'000)),
  };
  const std::vector<ReplayData> conflictAttempts = {conflictingSha};
  const auto conflictGroups =
      practice::analyzeCompatibleAttempts(chart, conflictAttempts);
  require(conflictGroups.front().conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              conflictGroups.front().conditions.effectiveJudgeWindows.empty(),
          "matching MD5 cannot override a conflicting durable SHA-256");

  auto md5Chart = makeChart();
  md5Chart.Meta.SHA256.clear();
  auto md5Fallback = makeReplay();
  md5Fallback.provenance.stages = {
      provenanceStage({}, std::string(32, 'B'), windows(-15'000)),
      provenanceStage({}, std::string(32, 'c'), windows(-16'000)),
  };
  const std::vector<ReplayData> md5Attempts = {md5Fallback};
  const auto md5Groups =
      practice::analyzeCompatibleAttempts(md5Chart, md5Attempts);
  require(md5Groups.front().conditions.windowResolution ==
                  practice::TimingWindowResolution::Resolved &&
              md5Groups.front().conditions.effectiveJudgeWindows ==
                  windows(-15'000),
          "MD5 is used when durable SHA-256 is unavailable");

  auto ambiguous = makeReplay();
  ambiguous.provenance.stages = {
      provenanceStage(std::string(64, 'a'), std::string(32, 'b'),
                      windows(-17'000)),
      provenanceStage(std::string(64, 'A'), std::string(32, 'B'),
                      windows(-18'000)),
  };
  const std::vector<ReplayData> ambiguousAttempts = {ambiguous};
  const auto ambiguousGroups =
      practice::analyzeCompatibleAttempts(chart, ambiguousAttempts);
  require(ambiguousGroups.front().conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              ambiguousGroups.front().conditions.effectiveJudgeWindows.empty(),
          "ambiguous durable stage identity selects no conditions");
}

void testUnresolvedTimingConditionsAlwaysUseSingletonGroups() {
  auto chart = makeChart();
  const auto useNeutralConditions = [](ReplayData &replay) {
    replay.provenance.playback = {};
    replay.provenance.judgeWindowScalePercent = 100;
  };

  auto ambiguousA = makeReplay();
  useNeutralConditions(ambiguousA);
  ambiguousA.provenance.stages = {
      provenanceStage(std::string(64, 'a'), std::string(32, 'b'),
                      windows(-20'000)),
      provenanceStage(std::string(64, 'A'), std::string(32, 'B'),
                      windows(-21'000)),
  };
  auto ambiguousB = ambiguousA;
  ambiguousB.provenance.stages[0].effectiveJudgeWindows = windows(-22'000);
  ambiguousB.provenance.stages[1].effectiveJudgeWindows = windows(-23'000);
  auto sameAmbiguousData = ambiguousA;
  for (ReplayData *replay : {&ambiguousA, &ambiguousB, &sameAmbiguousData}) {
    replay->events.push_back(
        event(ReplayEventAction::Press, 3, 0, Great, -1'000));
  }

  ReplayData legacyA;
  ReplayData legacyB;

  auto resolvedA = makeReplay();
  useNeutralConditions(resolvedA);
  auto resolvedB = resolvedA;

  auto unmatchedA = makeReplay();
  useNeutralConditions(unmatchedA);
  unmatchedA.provenance.stages = {provenanceStage(
      std::string(64, 'c'), std::string(32, 'd'), windows(-24'000))};
  auto unmatchedB = unmatchedA;
  unmatchedA.events.push_back(
      event(ReplayEventAction::Press, 3, 0, Great, -2'000));
  unmatchedB.events = unmatchedA.events;

  ReplayData currentWithoutStagesA;
  currentWithoutStagesA.provenance.ruleset = RulesetDescriptor::Current();
  auto currentWithoutStagesB = currentWithoutStagesA;
  currentWithoutStagesA.events.push_back(
      event(ReplayEventAction::Press, 3, 0, Great, -3'000));
  currentWithoutStagesB.events = currentWithoutStagesA.events;

  const std::vector<ReplayData> attempts = {
      ambiguousA,
      ambiguousB,
      sameAmbiguousData,
      legacyA,
      legacyB,
      resolvedA,
      resolvedB,
      unmatchedA,
      unmatchedB,
      currentWithoutStagesA,
      currentWithoutStagesB,
  };
  const auto groups = practice::analyzeCompatibleAttempts(chart, attempts);

  require(groups.size() == 9,
          "every unresolved attempt forms a singleton compatibility group");
  require(groups[0].attemptIndices == std::vector<std::size_t>({0}) &&
              groups[1].attemptIndices == std::vector<std::size_t>({1}) &&
              groups[2].attemptIndices == std::vector<std::size_t>({2}),
          "different and identical ambiguous attempts remain separate");
  require(groups[3].attemptIndices == std::vector<std::size_t>({3, 4}) &&
              groups[3].conditions.windowResolution ==
                  practice::TimingWindowResolution::LegacyAbsent,
          "genuine legacy timing conditions still group together");
  require(groups[4].attemptIndices == std::vector<std::size_t>({5, 6}) &&
              groups[4].conditions.windowResolution ==
                  practice::TimingWindowResolution::Resolved,
          "resolved exact timing conditions still group together");
  require(groups[5].attemptIndices == std::vector<std::size_t>({7}) &&
              groups[6].attemptIndices == std::vector<std::size_t>({8}),
          "identical unmatched provenance attempts remain separate");
  require(groups[7].attemptIndices == std::vector<std::size_t>({9}) &&
              groups[8].attemptIndices == std::vector<std::size_t>({10}),
          "non-legacy provenance with missing stages remains separate");
  require(groups[0].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              groups[1].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              groups[2].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              groups[5].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              groups[6].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              groups[7].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved &&
              groups[8].conditions.windowResolution ==
                  practice::TimingWindowResolution::Unresolved,
          "ambiguous and unmatched conditions preserve unresolved state");
  for (const std::size_t index : {0U, 1U, 2U, 5U, 6U, 7U, 8U}) {
    require(groups[index].aggregate.overall.samples == 1,
            "unresolved singleton aggregate contains only its attempt");
  }

  const auto unresolvedConditions = groups[0].conditions;
  const auto identicalUnresolvedConditions = unresolvedConditions;
  require(unresolvedConditions == unresolvedConditions &&
              unresolvedConditions == identicalUnresolvedConditions,
          "structural timing-condition equality remains reflexive");
  auto legacyConditions = unresolvedConditions;
  legacyConditions.windowResolution =
      practice::TimingWindowResolution::LegacyAbsent;
  require(!(unresolvedConditions == legacyConditions),
          "unresolved timing conditions never compare as legacy");
}

} // namespace

int main() {
  testTimingAndBreakdowns();
  testEmptyAnalysisHasNullStatistics();
  testHistogramUsesSparseStableBins();
  testHistogramSeparatesOverflowFromFiniteEndpointBins();
  testCompatibleAttemptGroups();
  testTimingConditionsUseStrictDurableStageIdentity();
  testUnresolvedTimingConditionsAlwaysUseSingletonGroups();
  return 0;
}
