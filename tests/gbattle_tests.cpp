#include "../src/GBattleMode.h"

#include <iostream>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual " << (actual) \
              << std::endl;                                                    \
    return 1;                                                                  \
  }

#define ASSERT_TRUE(value, label)                                              \
  if (!(value)) {                                                              \
    std::cerr << label << " expected true" << std::endl;                       \
    return 1;                                                                  \
  }

namespace {
bms_parser::Chart makeTwoNoteChart() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;

  auto *measure = new bms_parser::Measure();
  auto *first = new bms_parser::TimeLine(8, false);
  first->Timing = 1000000;
  first->SetNote(1, new bms_parser::Note(1));
  measure->TimeLines.push_back(first);

  auto *second = new bms_parser::TimeLine(8, false);
  second->Timing = 2000000;
  second->SetNote(2, new bms_parser::Note(2));
  measure->TimeLines.push_back(second);

  chart.Measures.push_back(measure);
  return chart;
}

ReplayData makeRecordReplay() {
  ReplayData replay;
  replay.finalScore = 3;
  replay.events.push_back({
      .action = ReplayEventAction::Press,
      .lane = 1,
      .noteTimeMicros = 1000000,
      .songTimeMicros = 1001000,
      .judgeTimeMicros = 1001000,
      .judgement = PGreat,
      .score = 2,
  });
  replay.events.push_back({
      .action = ReplayEventAction::Press,
      .lane = 2,
      .noteTimeMicros = 2000000,
      .songTimeMicros = 2004000,
      .judgeTimeMicros = 2004000,
      .judgement = Great,
      .score = 3,
  });
  return replay;
}
} // namespace

int main() {
  auto chart = makeTwoNoteChart();
  ReplayData replay = makeRecordReplay();

  const pacemaker::Target target = gbattle::targetFromRecord(chart, replay);
  ASSERT_TRUE(target.enabled, "target enabled");
  ASSERT_EQ(std::string("G-BATTLE"), target.label, "target label");
  ASSERT_TRUE(target.usesReplayProgression, "uses replay progression");
  ASSERT_EQ(3, target.finalScore, "final score");
  ASSERT_EQ(4, target.maxScore, "max score");
  ASSERT_EQ(2, target.totalNotes, "total notes");
  ASSERT_EQ(3U, target.scoreAfterNotes.size(), "progression size");
  ASSERT_EQ(0, target.scoreAfterNotes[0], "initial score");
  ASSERT_EQ(2, target.scoreAfterNotes[1], "first note score");
  ASSERT_EQ(3, target.scoreAfterNotes[2], "second note score");

  RhythmState state(&chart, false);
  state.judgeCount[PGreat] = 2;
  const auto resultPacemaker =
      gbattle::resultPacemakerDataFromRecord(chart, state, replay);
  ASSERT_TRUE(resultPacemaker.has_value(), "result pacemaker enabled");
  ASSERT_EQ(std::string("G-BATTLE"), resultPacemaker->label,
            "result pacemaker label");
  ASSERT_EQ(3, resultPacemaker->targetScore, "result target score");
  ASSERT_EQ(1, resultPacemaker->delta, "result score delta");
  ASSERT_TRUE(resultPacemaker->usesReplayProgression,
              "result uses replay progression");

  replay.autoPlay = true;
  const pacemaker::Target autoPlayTarget =
      gbattle::targetFromRecord(chart, replay);
  ASSERT_TRUE(!autoPlayTarget.enabled, "autoplay is not a target");

  replay = makeRecordReplay();
  replay.finalScore = 1;
  const pacemaker::Target inconsistentTarget =
      gbattle::targetFromRecord(chart, replay);
  ASSERT_TRUE(!inconsistentTarget.enabled,
              "inconsistent final score is rejected");

  return 0;
}
