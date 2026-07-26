#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayPlaybackDriver.h"
#include "replay/ReplayPlaybackMaterializer.h"
#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/GameplayGaugeRules.h"
#include "scene/play/Judge.h"

#include "bms_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<std::byte> fixture(std::string_view name) {
  const auto path = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests" /
                    "fixtures" / "replay" / name;
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> raw(std::istreambuf_iterator<char>(stream), {});
  std::vector<std::byte> result(raw.size());
  std::transform(raw.begin(), raw.end(), result.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  return result;
}

struct ControlCall {
  bool pressed = false;
  int lane = -1;
  bool backSpin = false;
  bool operator==(const ControlCall &) const = default;
};

class FakeRhythmControl final : public IRhythmControl {
public:
  bms_parser::Note *pressLane(int mainLane, int, double delay) override {
    calls.push_back({.pressed = true, .lane = mainLane});
    delays.push_back(delay);
    return nullptr;
  }

  bms_parser::Note *pressLane(int lane, double delay) override {
    calls.push_back({.pressed = true, .lane = lane});
    delays.push_back(delay);
    return nullptr;
  }

  bms_parser::Note *releaseLane(int lane, double delay,
                                bool isBackSpin) override {
    calls.push_back(
        {.pressed = false, .lane = lane, .backSpin = isBackSpin});
    delays.push_back(delay);
    return nullptr;
  }

  std::vector<ControlCall> calls;
  std::vector<double> delays;
};

void testDrivesPinnedStockReplayAtRecordedTimes() {
  replay::BeatorajaReplayCodec codec;
  const auto decoded = codec.decode(fixture("beatoraja-chart.brd"));
  expect(decoded.chart.has_value(), "pinned Beatoraja chart decodes");
  if (!decoded.chart) {
    return;
  }

  FakeRhythmControl control;
  replay::ReplayPlaybackDriver driver(*decoded.chart, control);
  driver.advanceTo(999);
  expect(control.calls.empty(), "driver waits until the first recorded time");
  driver.advanceTo(2'500);
  expect(control.calls ==
             std::vector<ControlCall>{{true, 0, false}, {false, 0, false},
                                      {true, 7, false}, {false, 7, false}},
         "stock lane and scratch input use exact timestamp order");
  expect(control.delays.front() == 0.0015 && control.delays[1] == 0.001,
         "late frame dispatch preserves each recorded input timestamp as "
         "input delay");
  expect(!driver.finished(), "driver remains active before the final input");
  driver.advanceTo(3'500);
  expect(control.calls ==
             std::vector<ControlCall>{{true, 0, false}, {false, 0, false},
                                      {true, 7, false}, {false, 7, false},
                                      {true, 7, false}, {false, 7, false}},
         "both stock scratch directions target the physical scratch lane");
  expect(driver.finished(), "driver finishes after the final transition");
}

void testDoubleLaneCommandsScratchReversalAndReset() {
  replay::ReplayPlaybackData playback;
  playback.setup.keyMode = 14;
  playback.input = {
      {.songTimeMicros = 100,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 2,
                   .lane = 2},
       .pressed = true},
      {.songTimeMicros = 110,
       .control = {.kind = replay::LogicalControlKind::Start, .player = 1},
       .pressed = true},
      {.songTimeMicros = 120,
       .control = {.kind = replay::LogicalControlKind::Select, .player = 2},
       .pressed = true},
      {.songTimeMicros = 130,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1},
       .pressed = false},
      {.songTimeMicros = 200,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1},
       .pressed = true},
      {.songTimeMicros = 250,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1},
       .pressed = false},
      {.songTimeMicros = 300,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 2,
                   .lane = 2},
       .pressed = false},
  };

  FakeRhythmControl control;
  std::vector<std::tuple<replay::LogicalControlKind, int, bool>> commands;
  replay::ReplayPlaybackDriver driver(
      playback, control,
      [&](const replay::LogicalControl &command, bool pressed) {
        commands.emplace_back(command.kind, command.player, pressed);
      });
  driver.advanceTo(200);
  expect(control.calls ==
             std::vector<ControlCall>{{true, 10, false}, {true, 7, false},
                                      {false, 7, true}, {true, 7, false}},
         "double lanes offset by eight and same-time scratch reversal marks "
         "the release as backspin");
  expect(commands ==
             std::vector<std::tuple<replay::LogicalControlKind, int, bool>>{
                 {replay::LogicalControlKind::Start, 1, true},
                 {replay::LogicalControlKind::Select, 2, true}},
         "Start and Select are delivered without becoming rhythm lanes");

  driver.reset();
  expect(control.calls.back() == ControlCall{false, 10, false},
         "reset releases every effective held rhythm lane");
  expect(!driver.finished(), "reset rewinds playback to the beginning");
  control.calls.clear();
  commands.clear();
  driver.advanceTo(300);
  expect(driver.finished() && control.calls.size() == 6,
         "rewound playback can run to completion again");
}

void testReplayOnlyScratchHandoffChangesOwnershipWithoutPhysicalEdges() {
  replay::ReplayPlaybackData playback;
  playback.setup.keyMode = 7;
  playback.input = {
      {.songTimeMicros = 100,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1},
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 200,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 2},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1},
       .pressed = true,
       .replayOnly = true},
      {.songTimeMicros = 250,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 2},
       .pressed = false},
      {.songTimeMicros = 300,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1},
       .pressed = false},
  };

  FakeRhythmControl control;
  replay::ReplayPlaybackDriver driver(playback, control);
  driver.advanceTo(200);
  expect(control.calls ==
             std::vector<ControlCall>{{true, 7, false}, {true, 2, false}},
         "marked ownership handoff keeps the held physical scratch intact");
  driver.advanceTo(300);
  expect(control.calls == std::vector<ControlCall>{{true, 7, false},
                                                   {true, 2, false},
                                                   {false, 2, false},
                                                   {false, 7, false}},
         "the new logical scratch owner performs the eventual release");

  replay::ReplayPlaybackData invalidLane;
  invalidLane.setup.keyMode = 7;
  invalidLane.input = {
      {.songTimeMicros = 400,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 2},
       .pressed = true,
       .replayOnly = true},
      {.songTimeMicros = 500,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 2},
       .pressed = false,
       .replayOnly = true},
  };
  FakeRhythmControl invalidControl;
  replay::ReplayPlaybackDriver invalidDriver(invalidLane, invalidControl);
  invalidDriver.advanceTo(500);
  expect(invalidControl.calls ==
             std::vector<ControlCall>{{true, 2, false}, {false, 2, false}},
         "invalid replay-only lane markers fail safe as physical input");

  replay::ReplayPlaybackData malformedScratch;
  malformedScratch.setup.keyMode = 7;
  malformedScratch.input = {
      {.songTimeMicros = 600,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1},
       .pressed = true},
      {.songTimeMicros = 700,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = 0},
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 700,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = 0},
       .pressed = true,
       .replayOnly = true},
  };
  FakeRhythmControl malformedControl;
  replay::ReplayPlaybackDriver malformedDriver(malformedScratch,
                                               malformedControl);
  malformedDriver.advanceTo(700);
  expect(malformedControl.calls == std::vector<ControlCall>{{true, 7, false},
                                                            {false, 7, true},
                                                            {true, 7, false}},
         "malformed marked scratch controls cannot suppress physical edges");
}

void testMaterializesRawInputsThroughGameplaySimulation() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 1;
  chart.Meta.Rank = 2;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 1'000'000;
  timeline->SetNote(1, new bms_parser::Note(1));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);

  replay::ReplayPlaybackData playback;
  playback.setup.keyMode = 7;
  playback.setup.longNoteMode = 0;
  playback.setup.initialGaugeType = GaugeType::Normal;
  playback.input = {
      {.songTimeMicros = 1'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 1},
       .pressed = true},
      {.songTimeMicros = 1'010'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 1},
       .pressed = false},
  };
  gameplay::GameplayRulesetPolicy policy;
  policy.judge = gameplay::CompiledGameplayJudge::from(Judge(chart.Meta.Rank));
  policy.gauge = compileGameplayGaugeRules(
      GameplayRuleset::Beatoraja, chart.Meta, GaugeProfile::Standard);

  const auto outcome = replay::materializeReplay(playback, chart, policy);
  expect(outcome.materialized(), "raw replay materialization succeeds");
  if (!outcome.value.has_value()) {
    return;
  }
  expect(outcome.value->attempt.score == 2 &&
             outcome.value->attempt.judgeCounts[PGreat] == 1,
         "materialization judges raw input under the supplied gameplay policy");
  expect(!outcome.value->judgedEvents.empty() &&
             outcome.value->judgedEvents.front().judgement == PGreat,
         "materialization exposes judged annotations for derived consumers");
  expect(!outcome.value->gaugeHistory.empty(),
         "materialization exposes derived gauge history");
  expect(outcome.value->gaugeState.currentGauge ==
             outcome.value->attempt.gauge &&
             outcome.value->gaugeState.gaugeType ==
                 outcome.value->attempt.gaugeType,
         "materialization exposes the complete terminal gauge snapshot");

  auto carriedGauge = outcome.value->gaugeState;
  carriedGauge.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::Hazard)] = true;
  const auto nextStage = replay::materializeReplay(
      playback, chart, policy,
      {.carriedGauge = carriedGauge,
       .carriedCombo = outcome.value->attempt.combo,
       .carriedMaxCombo = outcome.value->attempt.maxCombo});
  expect(nextStage.materialized() &&
             !nextStage.value->judgedEvents.empty() &&
             nextStage.value->judgedEvents.front().combo == 2 &&
             nextStage.value->attempt.combo == 2 &&
             nextStage.value->attempt.maxCombo == 2 &&
             nextStage.value->attempt.gauge >= outcome.value->attempt.gauge &&
             nextStage.value->gaugeState.gaugeValues
                     [gaugeTypeIndex(GaugeType::Normal)] ==
                 nextStage.value->attempt.gauge &&
             nextStage.value->gaugeState.gaugeSurvivalFailed
                 [gaugeTypeIndex(GaugeType::Hazard)],
         "sequential course materialization carries gauge, combo, and "
         "maximum");

  playback.legacy.emplace();
  const auto legacyOutcome =
      replay::materializeReplay(playback, chart, policy);
  expect(legacyOutcome.status ==
             replay::MaterializeOutcome::Status::LegacyTrack,
         "migrated legacy tracks select the isolated legacy adapter instead");
}

void testMaterializationHonorsRecordedCandidateSelection() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  chart.Meta.TotalNotes = 2;
  chart.Meta.Rank = 2;
  auto *measure = new bms_parser::Measure();
  auto *earlier = new bms_parser::TimeLine(8, false);
  earlier->Timing = 800'000;
  earlier->SetNote(1, new bms_parser::Note(1));
  measure->TimeLines.push_back(earlier);
  auto *later = new bms_parser::TimeLine(8, false);
  later->Timing = 950'000;
  later->SetNote(1, new bms_parser::Note(2));
  measure->TimeLines.push_back(later);
  chart.Measures.push_back(measure);

  replay::ReplayPlaybackData playback;
  playback.setup.keyMode = 5;
  playback.setup.longNoteMode = 0;
  playback.setup.initialGaugeType = GaugeType::Normal;
  playback.setup.candidateSelection = gameplay::CandidateSelectionMode::Combo;
  playback.input = {
      {.songTimeMicros = 1'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 1},
       .pressed = true},
      {.songTimeMicros = 1'010'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 1},
       .pressed = false},
  };
  gameplay::GameplayRulesetPolicy policy;
  policy.judge = gameplay::CompiledGameplayJudge::from(Judge(chart.Meta.Rank));
  policy.gauge = compileGameplayGaugeRules(
      GameplayRuleset::Beatoraja, chart.Meta, GaugeProfile::Standard);

  const auto outcome = replay::materializeReplay(playback, chart, policy);
  expect(outcome.materialized() && !outcome.value->judgedEvents.empty() &&
             outcome.value->judgedEvents.front().noteTimeMicros == 950'000,
         "raw materialization restores the recorded Combo note selection");
}

void testMaterializationRejectsCompletionTimeOverflow() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  chart.Meta.Rank = 2;

  replay::ReplayPlaybackData playback;
  playback.setup.keyMode = 5;
  playback.setup.longNoteMode = 0;
  playback.setup.initialGaugeType = GaugeType::Normal;
  playback.input = {{
      .songTimeMicros = std::numeric_limits<std::int64_t>::max(),
      .control = {.kind = replay::LogicalControlKind::Lane,
                  .player = 1,
                  .lane = 1},
      .pressed = true,
  }};
  gameplay::GameplayRulesetPolicy policy;
  policy.judge = gameplay::CompiledGameplayJudge::from(Judge(chart.Meta.Rank));
  policy.gauge = compileGameplayGaugeRules(
      GameplayRuleset::Beatoraja, chart.Meta, GaugeProfile::Standard);

  const auto outcome = replay::materializeReplay(playback, chart, policy);
  expect(outcome.status == replay::MaterializeOutcome::Status::Invalid &&
             !outcome.value.has_value(),
         "materialization rejects a completion timestamp that would overflow");
}

} // namespace

int main() {
  testDrivesPinnedStockReplayAtRecordedTimes();
  testDoubleLaneCommandsScratchReversalAndReset();
  testReplayOnlyScratchHandoffChangesOwnershipWithoutPhysicalEdges();
  testMaterializesRawInputsThroughGameplaySimulation();
  testMaterializationHonorsRecordedCandidateSelection();
  testMaterializationRejectsCompletionTimeOverflow();
  if (failures != 0) {
    std::cerr << failures << " replay playback driver test(s) failed\n";
    return 1;
  }
  std::cout << "replay playback driver tests passed\n";
  return 0;
}
