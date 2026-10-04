#include "replay/ChartReplayConsumer.h"
#include "replay/ReplayPlaybackDriver.h"
#include "replay/ReplayPlaybackMaterializer.h"
#include "replay/ReplaySetupAdapter.h"

#include "ReplayData.h"
#include "ReplayGhostUtils.h"
#include "ScoreProvenance.h"
#include "bms_parser.hpp"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/GameplayRulesetPolicy.h"
#include "scene/play/Pacemaker.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace replay;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ReplayChartDocument document() {
  ReplayChartDocument value;
  value.timeBounds = {.completionSongTimeMicros = 1'000};
  value.playback.setup.chart = {.md5 = repeated('b', 32),
                                .sha256 = repeated('a', 64),
                                .keyMode = 7};
  value.playback.setup.longNoteMode = 1;
  value.playback.input = {
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  value.playback.touchSamples = {
      {.action = replay::ReplayTouchAction::Down,
       .fingerId = 4,
       .songTimeMicros = 150,
       .x = 0.25F,
       .y = 0.75F},
  };
  value.playback.laneCoverEvents = {
      {.songTimeMicros = 175,
       .noteStartPositionPercent = 37,
       .resetVisibleTimeReference = true},
  };
  return value;
}

result_persistence::ModernChartResult savedResult() {
  result_persistence::ModernChartResult value;
  value.resultId = 17;
  value.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated('b', 32);
  value.score.chartSha256 = repeated('a', 64);
  value.score.chartTitle = "Title";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'123LL;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  return value;
}

void populateOneNoteChart(bms_parser::Chart &chart) {
  chart.Meta.BmsPath = "library/chart.bms";
  chart.Meta.MD5 = repeated('b', 32);
  chart.Meta.SHA256 = repeated('a', 64);
  chart.Meta.KeyMode = 7;
  chart.Meta.Rank = 2;
  chart.Meta.TotalNotes = 1;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 200.0;
  chart.Meta.LnMode = 1;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 500'000;
  timeline->SetNote(0, new bms_parser::Note(1));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
}

bms_parser::Chart oneNoteChart() {
  bms_parser::Chart chart;
  populateOneNoteChart(chart);
  return chart;
}

std::unique_ptr<bms_parser::Chart> oneNoteChartPointer() {
  auto chart = std::make_unique<bms_parser::Chart>();
  populateOneNoteChart(*chart);
  return chart;
}

void testConcreteMaterializerBuildsConsumerTrackDespiteResultDisagreement() {
  auto chart = oneNoteChart();
  auto replay = document();
  replay.timeBounds = {.completionSongTimeMicros = 2'000'000};
  replay.playback.input = {
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 510'000,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };

  auto saved = savedResult();
  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta = chart.Meta;
  provenance.longNoteMode = 1;
  provenance.sourceJudgeRank = chart.Meta.Rank;
  provenance.effectiveJudgeWindows = {
      {PGreat, {-20'000, 20'000}}, {Great, {-50'000, 50'000}},
      {Good, {-100'000, 100'000}}, {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenance.totalNotes = 1;
  provenance.authoredGaugeTotal = 200.0;
  provenance.effectiveGaugeTotal = 200.0;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  saved.score.provenance = makeScoreProvenance(provenance);
  saved.score.maxScore = 2;
  saved.score.chartPath = "library/chart.bms";
  saved.keyMode = 7;
  replay.playback.setup.ruleset = saved.score.provenance.ruleset;
  replay.playback.setup.candidateSelection =
      saved.score.provenance.stages.front().candidateSelection;
  replay.playback.setup.gaugeProfile = saved.score.provenance.gaugeProfile;
  replay.playback.setup.initialGaugeType = saved.score.provenance.gaugeType;
  replay.playback.setup.longNoteMode = 1;
  replay.playback.setup.playback = saved.score.provenance.playback;

  const auto first = ReplayPlaybackMaterializer::materializeForConsumers(
      replay, saved, chart);
  expect(first.state == ReplayPlaybackMaterializationState::ResultMismatch &&
             first.judgedResult.has_value() && first.playable() &&
             first.replayData && !first.diagnostic.empty(),
         "result disagreement remains diagnostic while the consumer track "
         "stays playable");
  if (!first.judgedResult.has_value()) {
    return;
  }

  saved = *first.judgedResult;
  const auto matched = ReplayPlaybackMaterializer::materializeForConsumers(
      replay, saved, chart);
  expect(matched.matched() && matched.replayData &&
             !matched.replayData->events.empty() &&
             matched.replayData->finalScore == saved.score.score &&
             matched.replayData->resultPassedNotes == 1 &&
             matched.replayData->provenance == saved.score.provenance,
         "verified replay yields one in-memory judged track for consumers");
  expect(matched.replayData && matched.replayData->touchSamples.size() == 1 &&
             matched.replayData->laneCoverEvents.size() == 1,
         "consumer track preserves BRD-owned touch and lane-cover streams");
  expect(matched.replayData &&
             matched.replayData->createdAt == "2023-11-14 22:13:20" &&
             matched.replayData->resultAttemptId == saved.attemptId,
         "consumer track retains the historical play time for BEST comparisons");

  // Beatoraja keeps arrival order for simultaneous edges. PG then BAD ends
  // at combo zero; sorting these edges by lane would leave combo one.
  auto beatoChart = oneNoteChart();
  auto *beatoTimeline = beatoChart.Measures.front()->TimeLines.front();
  delete beatoTimeline->Notes[0];
  beatoTimeline->Notes[0] = nullptr;
  beatoTimeline->SetNote(1, new bms_parser::Note(1));
  auto *earlyTimeline = new bms_parser::TimeLine(8, false);
  earlyTimeline->Timing = 350'000;
  earlyTimeline->SetNote(0, new bms_parser::Note(1));
  beatoChart.Measures.front()->TimeLines.insert(
      beatoChart.Measures.front()->TimeLines.begin(), earlyTimeline);
  beatoChart.Meta.TotalNotes = 2;
  auto beatoSaved = saved;
  auto beatoProof = provenance;
  beatoProof.chartMeta = beatoChart.Meta;
  beatoProof.totalNotes = 2;
  beatoProof.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  beatoProof.effectiveJudgeWindows.clear();
  beatoProof.effectiveJudgeContexts = gameplay::compileGameplayJudgeRules(
      GameplayRuleset::Beatoraja, beatoChart.Meta.Rank).contexts;
  beatoSaved.score.provenance = makeScoreProvenance(beatoProof);
  beatoSaved.score.maxScore = 4;
  auto beatoReplay = replay;
  beatoReplay.playback.setup.ruleset = beatoProof.ruleset;
  beatoReplay.playback.setup.candidateSelection =
      beatoSaved.score.provenance.stages.front().candidateSelection;
  beatoReplay.playback.input = {
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 1},
       .pressed = true},
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
       .pressed = true}};
  const auto beatoTrack = ReplayPlaybackMaterializer::materializeForConsumers(
      beatoReplay, beatoSaved, beatoChart);
  expect(beatoTrack.judgedResult && beatoTrack.judgedResult->score.pGreat == 1 &&
             beatoTrack.judgedResult->score.bad == 1 && beatoTrack.endingCombo == 0,
         "Beatoraja equal-time inputs retain their recorded arrival order");

  auto scratchChart = oneNoteChart();
  auto *scratchTimeline = scratchChart.Measures.front()->TimeLines.front();
  delete scratchTimeline->Notes[0];
  scratchTimeline->Notes[0] = nullptr;
  auto *scratchHead = new bms_parser::LongNote(1, bms_parser::LongNoteType::ChargeNote);
  auto *scratchTail = new bms_parser::LongNote(1, bms_parser::LongNoteType::ChargeNote);
  scratchHead->Tail = scratchTail;
  scratchTail->Head = scratchHead;
  scratchTimeline->SetNote(7, scratchHead);
  auto *scratchTailTimeline = new bms_parser::TimeLine(8, false);
  scratchTailTimeline->Timing = 750'000;
  scratchTailTimeline->SetNote(7, scratchTail);
  scratchChart.Measures.front()->TimeLines.push_back(scratchTailTimeline);
  scratchChart.Meta.TotalNotes = 2;
  auto scratchSaved = saved;
  auto scratchProof = provenance;
  scratchProof.chartMeta = scratchChart.Meta;
  scratchProof.totalNotes = 2;
  scratchSaved.score.provenance = makeScoreProvenance(scratchProof);
  scratchSaved.score.maxScore = 4;
  auto scratchReplay = replay;
  scratchReplay.playback.input = {
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::ScratchClockwise, .player = 1, .lane = -1},
       .pressed = true},
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise, .player = 1, .lane = -1},
       .pressed = true},
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::ScratchClockwise, .player = 1, .lane = -1},
       .pressed = false},
      {.songTimeMicros = 750'000,
       .control = {.kind = LogicalControlKind::ScratchClockwise, .player = 1, .lane = -1},
       .pressed = true},
      {.songTimeMicros = 760'000,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise, .player = 1, .lane = -1},
       .pressed = false},
      {.songTimeMicros = 770'000,
       .control = {.kind = LogicalControlKind::ScratchClockwise, .player = 1, .lane = -1},
       .pressed = false}};
  const auto scratchTrack = ReplayPlaybackMaterializer::materializeForConsumers(
      scratchReplay, scratchSaved, scratchChart);
  expect(scratchTrack.judgedResult && scratchTrack.judgedResult->score.pGreat == 2 &&
             scratchTrack.judgedResult->score.bad == 0,
         "LR2 latest raw scratch key snapshots ignore transient opposite-key presses");

  scratchHead->SetType(bms_parser::LongNoteType::LongNote);
  scratchTail->SetType(bms_parser::LongNoteType::LongNote);
  scratchChart.Meta.TotalNotes = 1;
  scratchProof.chartMeta = scratchChart.Meta;
  scratchProof.totalNotes = 1;
  scratchSaved.score.provenance = makeScoreProvenance(scratchProof);
  scratchSaved.score.maxScore = 2;
  scratchReplay.playback.input = {
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::ScratchClockwise, .player = 1, .lane = -1},
       .pressed = true},
      {.songTimeMicros = 600'000,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise, .player = 1, .lane = -1},
       .pressed = true},
      {.songTimeMicros = 760'000,
       .control = {.kind = LogicalControlKind::ScratchClockwise, .player = 1, .lane = -1},
       .pressed = false},
      {.songTimeMicros = 770'000,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise, .player = 1, .lane = -1},
       .pressed = false}};
  const auto classicScratch = ReplayPlaybackMaterializer::materializeForConsumers(
      scratchReplay, scratchSaved, scratchChart);
  expect(classicScratch.judgedResult && classicScratch.judgedResult->score.pGreat == 1 &&
             classicScratch.judgedResult->score.bad == 0,
         "classic scratch opposite press keeps the held tail pending");

  auto sameKeyBatch = replay;
  sameKeyBatch.playback.input[1].songTimeMicros = 500'000;
  const auto latestKey = ReplayPlaybackMaterializer::materializeForConsumers(
      sameKeyBatch, saved, chart);
  expect(latestKey.judgedResult && latestKey.judgedResult->score.pGreat == 0 &&
             latestKey.judgedResult->score.poor == 1,
         "LR2 equal-time same-key transitions judge only the latest physical state");

  auto historical = replay;
  auto historicalSaved = saved;
  historicalSaved.score.provenance.ruleset = {
      .id = "lr2", .version = 3, .scoringModel = "asobmashow-v1",
      .judgementModel = "lr2-v1", .gaugeModel = "lr2-gauge-v1"};
  auto &historicalStage = historicalSaved.score.provenance.stages.front();
  historicalStage.effectiveGaugeTotal = 0.0;
  for (auto &window : historicalStage.effectiveJudgeWindows) {
    if (window.judgement == PGreat) {
      window.earlyMicros = -1;
      window.lateMicros = 1;
    }
  }
  historicalStage.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  historical.playback.setup.ruleset = historicalSaved.score.provenance.ruleset;
  historical.playback.setup.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  historicalSaved.resultFingerprint = result_persistence::modernResultFingerprint(historicalSaved);
  const auto oldTrack = ReplayPlaybackMaterializer::materializeForConsumers(
      historical, historicalSaved, chart);
  expect(oldTrack.playable() && oldTrack.judgedResult && oldTrack.replayData &&
             oldTrack.judgedResult->score.score == saved.score.score &&
             oldTrack.replayData->finalGauge == saved.score.finalGauge &&
             oldTrack.replayData->provenance == historicalSaved.score.provenance &&
             oldTrack.replayData->playbackRuleset == RulesetDescriptor::Current() &&
             oldTrack.replayData->playbackPolicy.has_value() &&
             oldTrack.replayData->playbackPolicy->effectiveGaugeTotal == 200.0 &&
             oldTrack.replayData->playbackPolicy->candidateSelection ==
                 gameplay::CandidateSelectionMode::Combo &&
             oldTrack.replayData->playbackGaugeProfile.has_value(),
         "historical raw input uses current rules without changing saved provenance");

  auto historicalCourseInput = historical;
  historicalCourseInput.playback.input.front().songTimeMicros = 580'000;
  historicalCourseInput.playback.input.back().songTimeMicros = 590'000;
  const auto historicalCourse = ReplayPlaybackMaterializer::materializeForConsumers(
      historicalCourseInput, historicalSaved, chart,
      {.courseJudgement = CourseJudgementConstraint::NoGood,
       .courseGaugeProfile = GaugeProfile::Course5Keys});
  expect(historicalCourse.playable() && historicalCourse.judgedResult &&
             historicalCourse.judgedResult->score.good == 0 &&
             historicalCourse.judgedResult->score.bad == 1 &&
             historicalCourse.replayData->playbackGaugeProfile == GaugeProfile::Course5Keys &&
             historicalCourse.replayData->staleResult,
         "historical rejudging applies current course judgement and explicit gauge constraints");

  auto alteredRateReplay = replay;
  alteredRateReplay.playback.setup.playback = {.percent = 90};
  auto alteredRateSaved = saved;
  alteredRateSaved.score.provenance.playback =
      alteredRateReplay.playback.setup.playback;
  alteredRateSaved.score.clearType = kClearTypeLightAssistedEasyClearRank;
  alteredRateSaved.resultFingerprint =
      result_persistence::modernResultFingerprint(alteredRateSaved);
  const auto alteredRate =
      ReplayPlaybackMaterializer::materializeForConsumers(
          alteredRateReplay, alteredRateSaved, chart);
  expect(alteredRate.matched() && alteredRate.replayData,
         std::string("altered-rate replay preserves the live assisted clear: ") +
             alteredRate.diagnostic);

  auto assistedReplay = replay;
  assistedReplay.playback.setup.assistOption = assist_options::kAssisted;
  auto assistedSaved = saved;
  assistedSaved.score.provenance.assistOption = assist_options::kAssisted;
  assistedSaved.score.provenance.eligibility = ScoreEligibility::Modified;
  assistedSaved.score.clearType = kClearTypeLightAssistedEasyClearRank;
  assistedSaved.resultFingerprint =
      result_persistence::modernResultFingerprint(assistedSaved);
  const auto assisted = ReplayPlaybackMaterializer::materializeForConsumers(
      assistedReplay, assistedSaved, chart);
  expect(assisted.matched() && assisted.replayData &&
             assisted.replayData->assistOption == assist_options::kAssisted &&
             assisted.replayData->clearType == kClearTypeLightAssistedEasyClearRank,
         std::string("paused play uses existing assist metadata during Watch: ") +
             assisted.diagnostic);

  // Java can follow a paired endpoint outside active slots, while the runtime
  // ReplayData adapter can address only active lane/time identities.
  auto *headTimeline = chart.Measures.front()->TimeLines.front();
  delete headTimeline->Notes[0];
  auto *head = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(0, head);
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  tailTimeline->Timing = 750'000;
  tailTimeline->SetNote(0, tail);
  tailTimeline->Notes[0] = nullptr;
  chart.DetachedNotes.emplace_back(tail);
  chart.Measures.front()->TimeLines.push_back(tailTimeline);
  replay.playback.input.back().songTimeMicros = 760'000;
  auto detached = ReplayPlaybackMaterializer::materializeForConsumers(replay, saved, chart);
  expect(detached.judgedResult.has_value(), "detached tail can be rejudged");
  if (detached.judgedResult) {
    saved = *detached.judgedResult;
    detached = ReplayPlaybackMaterializer::materializeForConsumers(replay, saved, chart);
    expect(detached.matched() && detached.replayData &&
               !detached.consumerIdentityCompatible && !detached.diagnostic.empty(),
           "matched detached endpoint retains diagnostic data but reports adapter incompatibility");
    if (detached.replayData) {
      const std::vector<const bms_parser::TimeLine *> timelines{headTimeline, tailTimeline};
      const std::unordered_map<int, std::size_t> lanes{{0, 0}};
      expect(replay_ghost::buildReplayGhostEvents(*detached.replayData, timelines, lanes,
                                                 [](long long time) { return double(time); }).empty(),
             "in-memory ghost projection rejects an unrepresentable detached event identity");
    }
  }
  replay.playback.input.clear();
  const auto unheld = ReplayPlaybackMaterializer::materializeForConsumers(replay, saved, chart);
  expect(unheld.consumerIdentityCompatible,
         "a detached graph alone does not reject a track without detached result events");
}

void testHeldClassicReplayMatchesLiveJudgementAndBestGhost() {
  for (const auto ruleset : {GameplayRuleset::LR2, GameplayRuleset::Beatoraja}) {
    for (const std::int64_t releaseTime : {600'000LL, 750'000LL, 750'001LL, 750'002LL, 950'000LL, -1LL}) {
      auto chart = oneNoteChart();
      chart.Meta.TotalLongNotes = 1;
      auto *headTimeline = chart.Measures.front()->TimeLines.front();
      delete headTimeline->Notes[0];
      auto *head = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
      auto *tail = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
      head->Tail = tail;
      tail->Head = head;
      headTimeline->SetNote(0, head);
      auto *tailTimeline = new bms_parser::TimeLine(8, false);
      tailTimeline->Timing = 750'000;
      tailTimeline->SetNote(0, tail);
      chart.Measures.front()->TimeLines.push_back(tailTimeline);

      ScoreProvenanceBuildInput input;
      input.chartMeta = chart.Meta;
      input.ruleset = RulesetDescriptor::For(ruleset);
      input.longNoteMode = 1;
      input.sourceJudgeRank = chart.Meta.Rank;
      input.effectiveJudgeContexts = gameplay::compileGameplayJudgeRules(
          ruleset, chart.Meta.Rank).contexts;
      input.candidateSelection = ruleset == GameplayRuleset::LR2
          ? gameplay::CandidateSelectionMode::Combo
          : gameplay::CandidateSelectionMode::Lowest;
      input.totalNotes = 1;
      input.authoredGaugeTotal = 200.0;
      input.effectiveGaugeTotal = 200.0;
      input.inputDevices = {InputDeviceCategory::Keyboard};
      const auto provenance = makeScoreProvenance(input);
      const auto policy = gameplay::buildGameplayRulesetPolicy(
          chart.Meta, {.ruleset = ruleset});
      expect(policy.built(), "classic replay fixture has a valid live policy");
      if (!policy.built()) continue;
      const auto definition = gameplay::buildGameplayDefinition(chart, 1);
      gameplay::GameplaySimulation live(definition,
          {.judge = policy.policy->judge, .gaugeRules = policy.policy->gauge});
      live.applyPressAt(0, 0, {.songTimeMicros = 500'000,
                              .laneBeamTimeMicros = 500'000});
      if (releaseTime < 0 || releaseTime > 750'001) {
        live.advanceTo(750'001, 750'001);
      }
      if (releaseTime >= 0) {
        live.applyReleaseAt(0, {.songTimeMicros = releaseTime,
                                .laneBeamTimeMicros = releaseTime});
      }
      live.advanceTo(2'000'000, 2'000'000);
      RhythmState state(&chart, false, ruleset);
      static_cast<GameplayScoreState &>(state) = live.scoreState();
      std::string diagnostic;
      const auto saved = result_persistence::captureModernChartResult(
          "123e4567-e89b-42d3-a456-426614174000", chart.Meta, state,
          provenance, 1, 1'700'000'000'123LL, diagnostic);
      const bool releasedEarly = releaseTime >= 0 && releaseTime < 750'000;
      expect(saved && (releasedEarly ? saved->score.score < 2
                                    : saved->score.score == 2 && saved->score.pGreat == 1),
             "live classic LN rewards a held tail but penalizes an early lift");
      if (!saved) continue;
      auto replay = document();
      replay.timeBounds = {.completionSongTimeMicros = 2'000'000};
      replay.playback.setup.ruleset = provenance.ruleset;
      replay.playback.setup.candidateSelection = input.candidateSelection;
      replay.playback.input = {{.songTimeMicros = 500'000,
          .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
          .pressed = true}};
      if (releaseTime >= 0) {
        replay.playback.input.push_back({.songTimeMicros = releaseTime,
            .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
            .pressed = false});
      }
      const auto loaded = ReplayPlaybackMaterializer::materializeForConsumers(
          replay, *saved, chart);
      expect(loaded.matched() && loaded.replayData &&
                 loaded.replayData->finalScore == saved->score.score,
             "saved classic LN replay agrees with live score for early, exact, late, and absent lifts");
      if (!loaded.replayData) continue;
      const auto ghosts = replay_ghost::buildReplayGhostEvents(*loaded.replayData,
          std::vector<const bms_parser::TimeLine *>{headTimeline, tailTimeline},
          std::unordered_map<int, size_t>{{0, 0}},
          [](long long time) { return static_cast<double>(time); });
      const auto tailGhost = std::ranges::find_if(ghosts, [](const auto &event) {
        return event.noteTimeMicros == 750'000;
      });
      expect(tailGhost != ghosts.end() &&
                 (releasedEarly ? tailGhost->judgement != PGreat &&
                                      tailGhost->judgeTimeMicros == releaseTime
                                : tailGhost->judgement == PGreat &&
                                      tailGhost->judgeTimeMicros >= 750'000 &&
                                      tailGhost->judgeTimeMicros <= 750'001),
             "classic tail ghost preserves early lifts and automatically completes held tails");
      if (!loaded.matched()) {
        std::cerr << "classic replay case " << gameplayRulesetId(ruleset)
                  << " release=" << releaseTime << ": " << loaded.diagnostic
                  << " score=" << loaded.replayData->finalScore << '\n';
      }
      if (!releasedEarly) {
        const auto pace = pacemaker::targetFromBestSnapshot(
            chart, ScoreBestSnapshot{.score = saved->score.score, .maxScore = 2},
            &*loaded.replayData);
        expect(pace.usesReplayProgression && pace.finalScore == saved->score.score,
               "held classic LN record remains available as a BEST ghost pace");
      }
    }
  }
}

void testClassicReplayCompletionPreservesOtherLaneMineInput() {
  for (const bool initiallyPressed : {false, true}) {
    auto chart = oneNoteChart();
    chart.Meta.TotalLongNotes = 1;
    auto *headTimeline = chart.Measures.front()->TimeLines.front();
    delete headTimeline->Notes[0];
    auto *head = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
    auto *tail = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
    head->Tail = tail;
    tail->Head = head;
    headTimeline->SetNote(0, head);
    auto *tailTimeline = new bms_parser::TimeLine(8, false);
    tailTimeline->Timing = 750'000;
    tailTimeline->SetNote(0, tail);
    chart.Measures.front()->TimeLines.push_back(tailTimeline);
    auto *mineTimeline = new bms_parser::TimeLine(8, false);
    mineTimeline->Timing = 750'001;
    mineTimeline->SetLandmineNote(1, new bms_parser::LandmineNote(10.0F));
    chart.Measures.front()->TimeLines.push_back(mineTimeline);

    ScoreProvenanceBuildInput input;
    input.chartMeta = chart.Meta;
    input.ruleset = RulesetDescriptor::For(GameplayRuleset::LR2);
    input.gaugeType = GaugeType::Hard;
    input.longNoteMode = 1;
    input.sourceJudgeRank = chart.Meta.Rank;
    input.effectiveJudgeContexts = gameplay::compileGameplayJudgeRules(
        GameplayRuleset::LR2, chart.Meta.Rank).contexts;
    input.candidateSelection = gameplay::CandidateSelectionMode::Combo;
    input.totalNotes = 1;
    input.authoredGaugeTotal = 200.0;
    input.effectiveGaugeTotal = 200.0;
    input.inputDevices = {InputDeviceCategory::Keyboard};
    const auto provenance = makeScoreProvenance(input);
    const auto policy = gameplay::buildGameplayRulesetPolicy(
        chart.Meta, {.ruleset = GameplayRuleset::LR2});
    expect(policy.built(), "mine interaction fixture has a valid live policy");
    if (!policy.built()) continue;
    const auto definition = gameplay::buildGameplayDefinition(chart, 1);
    gameplay::GameplaySimulation live(definition,
        {.judge = policy.policy->judge, .gaugeRules = policy.policy->gauge,
         .attempt = {.initialGaugeType = GaugeType::Hard}});
    auto replay = document();
    replay.timeBounds = {.completionSongTimeMicros = 2'000'000};
    replay.playback.setup.ruleset = provenance.ruleset;
    replay.playback.setup.candidateSelection = input.candidateSelection;
    replay.playback.setup.initialGaugeType = GaugeType::Hard;
    replay.playback.input.clear();
    if (initiallyPressed) {
      live.applyPressAt(1, 1, {.songTimeMicros = 400'000,
                              .laneBeamTimeMicros = 400'000});
      replay.playback.input.push_back({.songTimeMicros = 400'000,
          .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 1},
          .pressed = true});
    }
    live.applyPressAt(0, 0, {.songTimeMicros = 500'000,
                            .laneBeamTimeMicros = 500'000});
    replay.playback.input.push_back({.songTimeMicros = 500'000,
        .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
        .pressed = true});
    // The next live update samples the new physical state before the mine
    // passes. Catching up the LN must not insert a mine update before this.
    live.advanceTo(750'000, 750'000);
    const gameplay::GameplayInputContext nextUpdate{
        .songTimeMicros = 750'002, .laneBeamTimeMicros = 750'002};
    if (initiallyPressed) live.applyReleaseAt(1, nextUpdate);
    else live.applyPressAt(1, 1, nextUpdate);
    replay.playback.input.push_back({.songTimeMicros = 750'002,
        .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 1},
        .pressed = !initiallyPressed});
    live.advanceTo(2'000'000, 2'000'000);
    RhythmState state(&chart, false, GameplayRuleset::LR2);
    static_cast<GameplayScoreState &>(state) = live.scoreState();
    std::string diagnostic;
    const auto saved = result_persistence::captureModernChartResult(
        "123e4567-e89b-42d3-a456-426614174000", chart.Meta, state,
        provenance, 1, 1'700'000'000'123LL, diagnostic);
    expect(saved && saved->score.score == 2 &&
               (initiallyPressed ? saved->score.finalGauge == 100.0F
                                 : saved->score.finalGauge < 91.0F),
           "live mine judges the incoming physical state while the LN completes");
    if (!saved) continue;
    const auto loaded = ReplayPlaybackMaterializer::materializeForConsumers(
        replay, *saved, chart);
    expect(loaded.matched() && loaded.replayData &&
               loaded.replayData->finalGauge == saved->score.finalGauge,
           "classic tail catch-up preserves live mine judgement and gauge");
    if (!loaded.replayData) continue;
    const auto mines = std::ranges::count_if(loaded.replayData->events,
        [](const auto &event) { return event.action == ReplayEventAction::Mine; });
    expect(mines == (initiallyPressed ? 0 : 1),
           "classic tail catch-up neither inserts nor swallows other-lane mines");
  }
}

void testConcreteMaterializerSettlesExactTimeMineInput() {
  for (bool initiallyPressed : {false, true}) {
    auto chart = oneNoteChart();
    chart.Measures.front()->TimeLines.front()->SetLandmineNote(
        1, new bms_parser::LandmineNote(4.0F));
    auto replay = document();
    replay.timeBounds = {.completionSongTimeMicros = 2'000'000};
    replay.playback.input.clear();
    if (initiallyPressed) {
      replay.playback.input.push_back({.songTimeMicros = 100'000,
          .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 1},
          .pressed = true});
    }
    replay.playback.input.push_back({.songTimeMicros = 500'000,
        .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
        .pressed = true});
    replay.playback.input.push_back({.songTimeMicros = 500'000,
        .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 1},
        .pressed = !initiallyPressed});
    replay.playback.input.push_back({.songTimeMicros = 510'000,
        .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
        .pressed = false});
    auto saved = savedResult();
    ScoreProvenanceBuildInput provenance;
    provenance.chartMeta = chart.Meta;
    provenance.longNoteMode = 1;
    provenance.sourceJudgeRank = chart.Meta.Rank;
    provenance.effectiveJudgeContexts = gameplay::compileGameplayJudgeRules(
        GameplayRuleset::LR2, chart.Meta.Rank).contexts;
    provenance.totalNotes = 1;
    provenance.authoredGaugeTotal = 200.0;
    provenance.effectiveGaugeTotal = 200.0;
    provenance.inputDevices = {InputDeviceCategory::Keyboard};
    saved.score.provenance = makeScoreProvenance(provenance);
    saved.score.maxScore = 2;
    replay.playback.setup.ruleset = saved.score.provenance.ruleset;
    replay.playback.setup.candidateSelection = saved.score.provenance.stages.front().candidateSelection;
    replay.playback.setup.gaugeProfile = saved.score.provenance.gaugeProfile;
    replay.playback.setup.initialGaugeType = saved.score.provenance.gaugeType;
    const auto outcome = ReplayPlaybackMaterializer::materializeForConsumers(replay, saved, chart);
    expect(outcome.judgedResult.has_value() && outcome.replayData,
           "mine boundary replay materializes after batched input preadvance");
    if (!outcome.replayData || !outcome.judgedResult) continue;
    int mines = 0;
    for (const auto &event : outcome.replayData->events) {
      if (event.action == ReplayEventAction::Mine) ++mines;
    }
    expect(mines == (initiallyPressed ? 0 : 1) &&
               outcome.judgedResult->score.pGreat == 1 &&
               outcome.judgedResult->score.finalGauge == 100.0F,
           "materialized replay applies exact-time mine press/release after settling its entire input batch");
  }
}

void testConsumerSetupAdapterOwnsEveryReplaySetupTranslation() {
  auto replay = document();
  replay.playback.setup.chartRandomSeed = 42;
  replay.playback.setup.chartRandomPrng = bms_parser::Parser::RandomPrngId;
  replay.playback.setup.chartRandomValues = {2, 1, 3};
  replay.playback.setup.player1 = {.option = "RANDOM", .seed = 1234};
  replay.playback.setup.player2 = {.option = "MIRROR", .seed = 5678};
  replay.playback.setup.assistOption = assist_options::kDrag;
  replay.playback.setup.initialGaugeType = GaugeType::Hard;
  replay.playback.setup.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  replay.playback.setup.gaugeAutoShiftLowerBound = GaugeType::Easy;
  replay.playback.setup.initialLaneCoverPercent = 37;
  replay.playback.setup.laneCoverEnabled = true;
  replay.playback.setup.doublePlayOption = DoublePlayOption::Flip;
  replay.playback.setup.playback = {
      .percent = 125, .mode = audio::PlaybackMode::TimeStretch};
  replay.playback.setup.clubMode = true;
  auto provenance = savedResult().score.provenance;
  provenance.doublePlayFlip = true;
  provenance.playback = replay.playback.setup.playback;
  provenance.clubMode = true;

  bms_parser::ChartMeta meta;
  meta.BmsPath = "library/chart.bms";
  meta.MD5 = replay.playback.setup.chart.md5;
  meta.SHA256 = replay.playback.setup.chart.sha256;
  meta.KeyMode = 14;
  replay.playback.setup.chart.keyMode = 14;
  std::string diagnostic;
  const auto translated = makeReplayDataFromSetup(
      replay.playback.setup, provenance, meta, diagnostic);
  expect(translated.has_value() && translated->randomSeed == 42 &&
             translated->randomPrng == bms_parser::Parser::RandomPrngId &&
             translated->randomValues == std::vector<int>({2, 1, 3}) &&
             translated->playOption == "RANDOM" &&
             translated->playOptionSeed == 1234 &&
             translated->playOption2 == "MIRROR" &&
             translated->playOption2Seed == 5678 &&
             translated->assistOption == assist_options::kDrag &&
             translated->initialGaugeType == GaugeType::Hard &&
             translated->initialLaneCoverPercent == 37 &&
             translated->initialLaneCoverEnabled &&
             translated->hasInitialLaneCoverState &&
             translated->provenance == provenance,
         "one adapter projects complete setup for every chart consumer");

  auto alreadyBoundSetup = replay.playback.setup;
  alreadyBoundSetup.chart.sha256 = std::string(64, 'f');
  expect(makeReplayDataFromSetup(alreadyBoundSetup, provenance, meta,
                                 diagnostic)
             .has_value(),
         "setup translation does not repeat consumer identity validation");

  replay.playback.setup.chartRandomSeed =
      static_cast<std::uint64_t>(std::numeric_limits<unsigned int>::max()) + 1;
  expect(!makeReplayDataFromSetup(replay.playback.setup, provenance, meta,
                                  diagnostic),
         "setup translation fails closed when the parser seed cannot round-trip");
}

void testChartConsumerOwnsTheEntireVerifiedPreparationPipeline() {
  auto listed = ModernChartResultRecord{.result = savedResult()};
  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta.MD5 = listed.result.score.chartMd5;
  provenance.chartMeta.SHA256 = listed.result.score.chartSha256;
  provenance.chartMeta.KeyMode = listed.result.keyMode;
  provenance.chartMeta.Rank = 2;
  provenance.chartMeta.TotalNotes = listed.result.score.maxScore / 2;
  provenance.longNoteMode = 1;
  provenance.sourceJudgeRank = 2;
  provenance.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenance.totalNotes = listed.result.score.maxScore / 2;
  provenance.effectiveGaugeTotal = 200.0;
  listed.result.score.provenance = makeScoreProvenance(provenance);
  listed.result.score.longNoteMode = 0;
  listed.result.resultFingerprint =
      result_persistence::modernResultFingerprint(listed.result);
  auto replay = document();
  replay.playback.setup.ruleset = listed.result.score.provenance.ruleset;
  replay.playback.setup.gaugeProfile =
      listed.result.score.provenance.gaugeProfile;
  replay.playback.setup.initialGaugeType =
      listed.result.score.provenance.gaugeType;
  replay.playback.setup.longNoteMode = 1;

  std::vector<std::string> calls;
  bool incompatibleIdentity = false;
  ChartReplayConsumer consumer({
      .loadContext = [&](std::string_view attemptId) {
        calls.emplace_back("context");
        expect(attemptId == listed.result.attemptId,
               "consumer loads the authoritative result and replay first");
        VerifiedChartReplay verified{
            .result = listed.result,
            .document = replay,
            .source = ReplayStageDecodeSource::AsoExtension,
        };
        return ChartReplayContextOutcome{
            .state = ChartReplayContextState::Ready,
            .result = listed.result,
            .verified = std::move(verified),
        };
      },
      .prepareChart = [&](const std::filesystem::path &path,
                          const ReplaySetup &setup,
                          const ScoreProvenance &provenance,
                          std::atomic_bool &, std::string &) {
        calls.emplace_back("prepare");
        expect(path == "selected/chart.bms" &&
                   setup == replay.playback.setup &&
                   provenance == listed.result.score.provenance,
               "consumer passes one verified setup to the sole chart parse");
        auto prepared = oneNoteChartPointer();
        prepared->Meta.LnMode = 0;
        return prepared;
      },
      .materialize = [&](const ReplayChartDocument &document,
                         const result_persistence::ModernChartResult &result,
                         const bms_parser::Chart &) {
        calls.emplace_back("materialize");
        expect(document == replay && result == listed.result,
               "consumer materializes only the verified document and result");
        ReplayPlaybackMaterializationOutcome outcome;
        outcome.state = incompatibleIdentity ? ReplayPlaybackMaterializationState::Matched
                                             : ReplayPlaybackMaterializationState::ResultMismatch;
        outcome.consumerIdentityCompatible = !incompatibleIdentity;
        outcome.replayData = std::make_shared<ReplayData>();
        outcome.diagnostic = "Saved result differs from replay judging.";
        return outcome;
      },
  });

  std::atomic_bool cancelled = false;
  auto loaded = consumer.load(listed, "selected/chart.bms", cancelled);
  expect(loaded.ready() && loaded.chart && loaded.replayData &&
             loaded.state == ChartReplayConsumerState::Ready &&
             loaded.diagnostic ==
                 "Saved result differs from replay judging." &&
             calls == std::vector<std::string>{"context", "prepare",
                                               "materialize"},
         "saved replay actions accept reproducible result drift and preserve the diagnostic");

  incompatibleIdentity = true;
  loaded = consumer.load(listed, "selected/chart.bms", cancelled);
  expect(!loaded.ready() && !loaded.replayData &&
             loaded.state == ChartReplayConsumerState::MaterializationFailed,
         "matching score facts do not authorize an unrepresentable runtime note identity");

  calls.clear();
  ChartReplayConsumer missing({
      .loadContext = [&](std::string_view) {
        calls.emplace_back("context");
        return ChartReplayContextOutcome{
            .state = ChartReplayContextState::FileMissing,
            .result = listed.result,
        };
      },
      .prepareChart = [&](const std::filesystem::path &, const ReplaySetup &,
                          const ScoreProvenance &, std::atomic_bool &,
                          std::string &) {
        calls.emplace_back("unexpected-prepare");
        return std::unique_ptr<bms_parser::Chart>{};
      },
      .materialize = [&](const ReplayChartDocument &,
                         const result_persistence::ModernChartResult &,
                         const bms_parser::Chart &) {
        calls.emplace_back("unexpected-materialize");
        return ReplayPlaybackMaterializationOutcome{};
      },
  });
  loaded = missing.load(listed, "selected/chart.bms", cancelled);
  expect(!loaded.ready() && !loaded.chart && !loaded.replayData &&
             loaded.context.state == ChartReplayContextState::FileMissing &&
             calls == std::vector<std::string>{"context"},
         "missing replay stops before parsing, setup, judging, or consumer "
         "output");
}

void testDriverMergesStreamsWithoutChangingTheirTiming() {
  const auto replay = document();
  ReplayPlaybackDriver driver(replay);
  std::vector<std::string> delivered;
  ReplayPlaybackSink sink{
      .input = [&](const InputTransition &event, std::string &) {
        delivered.push_back("input:" + std::to_string(event.songTimeMicros));
        return true;
      },
      .touch = [&](const replay::ReplayTouchSample &event, std::string &) {
        delivered.push_back("touch:" + std::to_string(event.songTimeMicros));
        return true;
      },
      .laneCover = [&](const replay::ReplayLaneCoverEvent &event,
                       std::string &) {
        delivered.push_back("cover:" + std::to_string(event.songTimeMicros));
        return true;
      }};

  auto advanced = driver.advanceTo(160, sink);
  expect(advanced.state == ReplayPlaybackDriverState::Advanced &&
             delivered ==
                 std::vector<std::string>{"input:100", "touch:150"},
         "driver emits raw logical input and touch at recorded times");
  advanced = driver.advanceTo(200, sink);
  expect(advanced.state == ReplayPlaybackDriverState::Advanced &&
             delivered == std::vector<std::string>{
                              "input:100", "touch:150", "cover:175",
                              "input:200"},
         "driver globally merges lane-cover and input without retiming");
  advanced = driver.advanceTo(1'000, sink);
  expect(advanced.state == ReplayPlaybackDriverState::Complete &&
             driver.complete(),
         "driver completes only at the parsed completion boundary");
}

void testDriverTrustsStructurallyValidatedDocument() {
  auto replay = document();
  std::swap(replay.playback.input.front(), replay.playback.input.back());
  ReplayPlaybackDriver driver(replay);
  expect(driver.valid(),
         "driver does not repeat structural validation owned by the codec");
}

void testDriverRejectsReverseTimeAndBoundsEachAdvance() {
  const auto replay = document();
  ReplayPlaybackDriver driver(replay);
  ReplayPlaybackSink sink;
  expect(driver.advanceTo(200, sink).state ==
             ReplayPlaybackDriverState::Advanced,
         "first monotonic advance succeeds with optional auxiliary sinks");
  expect(driver.advanceTo(199, sink).state ==
             ReplayPlaybackDriverState::NonMonotonicAdvance,
         "reverse playback time fails closed");

  ReplayPlaybackDriver bounded(replay);
  const auto exhausted = bounded.advanceTo(1'000, sink, 2);
  expect(exhausted.state == ReplayPlaybackDriverState::WorkLimitExceeded &&
             !bounded.complete(),
         "per-run event budget bounds adversarial playback work");
}

void testLogicalGameplayAdapterOwnsLaneAndScratchMapping() {
  for (const auto &layout : kReplayKeyModeLayouts) {
    for (int player = 1; player <= layout.players; ++player) {
      for (int lane = 0; lane < layout.laneCodeWidthPerPlayer; ++lane) {
        const LogicalControl logical{.kind = LogicalControlKind::Lane,
                                     .player = player,
                                     .lane = lane};
        const auto physical = physicalChartLaneForLogicalControl(
            layout.keyMode, logical);
        expect(!physical ||
                   logicalControlForChartLane(layout.keyMode, *physical,
                                              false) == logical,
               "logical lane mapping round-trips through one authority");
      }
      if (layout.hasDirectionalScratch) {
        const LogicalControl scratch{
            .kind = LogicalControlKind::ScratchCounterClockwise,
            .player = player,
            .lane = -1};
        const auto physical = physicalChartLaneForLogicalControl(
            layout.keyMode, scratch);
        expect(physical &&
                   logicalControlForChartLane(
                       layout.keyMode, *physical, true,
                       LogicalControlKind::ScratchCounterClockwise) == scratch,
               "directional scratch mapping round-trips through one authority");
      }
    }
  }

  struct Edge {
    bool pressed = false;
    int lane = -1;
    bool backSpin = false;
    double delay = 0.0;
  };
  std::vector<Edge> edges;
  ReplayLogicalGameplayAdapter adapter(
      14,
      {.pressLane = [&](int lane, double delay) {
         edges.push_back({.pressed = true, .lane = lane, .delay = delay});
       },
       .releaseLane = [&](int lane, double delay, bool backSpin) {
         edges.push_back({.pressed = false,
                          .lane = lane,
                          .backSpin = backSpin,
                          .delay = delay});
       }});
  const std::vector<InputTransition> batch{
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 2,
                   .lane = 2},
       .pressed = true},
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
  };
  std::string diagnostic;
  expect(adapter.applyBatch(std::span(batch).first(2), 250, diagnostic) &&
             adapter.applyBatch(std::span(batch).subspan(2), 250, diagnostic),
         "logical replay adapter accepts timestamp batches");
  expect(edges.size() == 4 && edges[0].lane == 10 &&
             edges[0].delay == 0.00015 && edges[1].lane == 7 &&
             !edges[2].pressed && edges[2].lane == 7 &&
             edges[2].backSpin && edges[3].pressed && edges[3].lane == 7,
         "adapter maps double lanes and same-time scratch reversal exactly");
  adapter.reset();
  expect(!edges.back().pressed && edges.back().lane == 10,
         "adapter reset releases every remaining physical lane");
}

void testLogicalGameplayAdapterAllowsOverlappingStockScratchDirections() {
  struct Edge {
    bool pressed = false;
    bool backSpin = false;
    auto operator<=>(const Edge &) const = default;
  };
  std::vector<Edge> edges;
  ReplayLogicalGameplayAdapter adapter(
      7,
      {.pressLane = [&](int lane, double) {
         expect(lane == 7, "overlapping stock scratch stays on its chart lane");
         edges.push_back({.pressed = true});
       },
       .releaseLane = [&](int lane, double, bool backSpin) {
         expect(lane == 7, "overlapping stock scratch stays on its chart lane");
         edges.push_back({.pressed = false, .backSpin = backSpin});
       }});
  const std::vector<InputTransition> input{
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 300,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
      {.songTimeMicros = 400,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
      {.songTimeMicros = 500,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 600,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 700,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
      {.songTimeMicros = 800,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
  };
  std::string diagnostic;
  bool accepted = true;
  for (const auto &transition : input) {
    accepted = adapter.applyBatch(std::span(&transition, 1),
                                  transition.songTimeMicros, diagnostic) &&
               accepted;
  }
  expect(accepted && diagnostic.empty(),
         "independently held stock scratch directions release in either order");
  expect(edges == std::vector<Edge>{{.pressed = true},
                                    {.pressed = false, .backSpin = true},
                                    {.pressed = true},
                                    {.pressed = false},
                                    {.pressed = true},
                                    {.pressed = false, .backSpin = true},
                                    {.pressed = true},
                                    {.pressed = false, .backSpin = true},
                                    {.pressed = true},
                                    {.pressed = false}},
         "stock scratch releases ignore inactive keys and reactivate the "
         "remaining held direction");
}

void testLogicalGameplayAdapterPreservesReplayOnlyScratchHandoffs() {
  struct Edge {
    bool pressed = false;
    bool backSpin = false;
    auto operator<=>(const Edge &) const = default;
  };
  std::vector<Edge> edges;
  ReplayLogicalGameplayAdapter adapter(
      7,
      {.pressLane = [&](int, double) {
         edges.push_back({.pressed = true});
       },
       .releaseLane = [&](int, double, bool backSpin) {
         edges.push_back({.pressed = false, .backSpin = backSpin});
       }});
  const std::vector<InputTransition> initial{
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
  };
  const std::vector<InputTransition> handoff{
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true,
       .replayOnly = true},
  };
  const std::vector<InputTransition> final{
      {.songTimeMicros = 300,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
  };
  std::string diagnostic;
  expect(adapter.applyBatch(initial, 300, diagnostic) &&
             adapter.applyBatch(handoff, 300, diagnostic) &&
             adapter.applyBatch(final, 300, diagnostic) &&
             diagnostic.empty(),
         "canonical replay-only scratch handoff remains playable");
  expect(edges == std::vector<Edge>{{.pressed = true}, {.pressed = false}},
         "replay-only ownership handoff adds no physical scratch edge");
}

void testMaterializerUsesDriverAndOnlyComparesSavedFacts() {
  const auto replay = document();
  const auto saved = savedResult();
  std::vector<std::int64_t> judgedInputs;
  bool finished = false;
  ReplayJudgingSink judge{
      .advanceTo = [](std::int64_t, std::string &) { return true; },
      .applyInput = [&](const InputTransition &event, std::string &) {
        judgedInputs.push_back(event.songTimeMicros);
        return true;
      },
      .finish = [&](std::string &) {
        finished = true;
        return std::optional(saved);
      }};

  const auto matched = ReplayPlaybackMaterializer::materialize(
      replay, saved, judge);
  expect(matched.state == ReplayPlaybackMaterializationState::Matched &&
             judgedInputs == std::vector<std::int64_t>{100, 200} && finished,
         "judged materialization consumes raw input through the shared driver");

  auto different = saved;
  ++different.score.good;
  different.resultFingerprint =
      result_persistence::modernResultFingerprint(different);
  judge.finish = [&](std::string &) { return std::optional(different); };
  judgedInputs.clear();
  const auto mismatch = ReplayPlaybackMaterializer::materialize(
      replay, saved, judge);
  expect(mismatch.state == ReplayPlaybackMaterializationState::ResultMismatch &&
             mismatch.agreement && !mismatch.agreement->agrees() &&
             saved.score.good != different.score.good,
         "materialized facts are compared and never replace saved result facts");
}

void testMaterializationBudgetStopsBeforeResultConstruction() {
  const auto replay = document();
  const auto saved = savedResult();
  bool finished = false;
  ReplayJudgingSink judge{
      .advanceTo = [](std::int64_t, std::string &) { return true; },
      .applyInput = [](const InputTransition &, std::string &) { return true; },
      .finish = [&](std::string &) {
        finished = true;
        return std::optional(saved);
      }};
  const auto bounded = ReplayPlaybackMaterializer::materialize(
      replay, saved, judge, 1);
  expect(bounded.state ==
             ReplayPlaybackMaterializationState::WorkLimitExceeded &&
             !finished,
         "bounded materialization cannot construct facts from a partial replay");
}

} // namespace

void testAbortedRawReplayReconstructsFailureWithoutTrustingSummary() {
  for (const auto gauge : {GaugeType::Hard, GaugeType::ExHard, GaugeType::Hazard,
                           GaugeType::Normal}) {
    for (const auto shift : {GaugeAutoShiftMode::None, GaugeAutoShiftMode::BestClear,
                             GaugeAutoShiftMode::SelectToUnder}) {
      for (const bool midway : {false, true}) {
        auto chart = oneNoteChart();
        auto *timeline = new bms_parser::TimeLine(8, false);
        timeline->Timing = 1'500'000;
        timeline->SetNote(1, new bms_parser::Note(1));
        chart.Measures.front()->TimeLines.push_back(timeline);
        chart.Meta.TotalNotes = 2;
        auto raw = document();
        raw.timeBounds = {.completionSongTimeMicros = midway ? 550'000 : 400'000,
                           .aborted = true};
        raw.playback.setup.initialGaugeType = gauge;
        raw.playback.setup.gaugeAutoShift = shift;
        raw.playback.setup.startingGaugePercent = 100;
        raw.playback.setup.ruleset = RulesetDescriptor::Current();
        raw.playback.touchSamples.clear();
        raw.playback.laneCoverEvents.clear();
        raw.playback.input.clear();
        if (midway) {
          raw.playback.input = {
              {.songTimeMicros = 500'000,
               .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
               .pressed = true},
              {.songTimeMicros = 510'000,
               .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
               .pressed = false}};
        }
        auto saved = savedResult();
        ScoreProvenanceBuildInput provenance;
        provenance.chartMeta = chart.Meta;
        provenance.longNoteMode = 1;
        provenance.sourceJudgeRank = 2;
        provenance.effectiveJudgeWindows = {
            {PGreat, {-20'000, 20'000}}, {Great, {-50'000, 50'000}},
            {Good, {-100'000, 100'000}}, {Bad, {-200'000, 200'000}},
            {Kpoor, {-1'000'000, 0}}};
        provenance.totalNotes = 2;
        provenance.authoredGaugeTotal = 200.0;
        provenance.effectiveGaugeTotal = 200.0;
        provenance.gaugeType = gauge;
        provenance.gaugeAutoShift = shift;
        provenance.startingGaugePercent = 100;
        provenance.inputDevices = {InputDeviceCategory::Keyboard};
        saved.score.provenance = makeScoreProvenance(provenance);
        saved.score.clearType = kClearTypeFullComboRank;
        saved.score.finalGauge = 100;
        saved.resultFingerprint = result_persistence::modernResultFingerprint(saved);
        const auto outcome = ReplayPlaybackMaterializer::materializeForConsumers(
            raw, saved, chart, 128);
        if (!outcome.judgedResult) {
          std::cerr << "Abort fixture rejected: " << outcome.diagnostic << '\n';
        }
        expect(outcome.judgedResult &&
                   outcome.judgedResult->score.clearType == kClearTypeFailedRank &&
                   outcome.judgedResult->score.finalGauge == 0,
               "COR02: raw aborted completion reconstructs failure despite forged successful summary");
        expect(outcome.judgedResult &&
                   outcome.judgedResult->score.pGreat == (midway ? 1 : 0) &&
                   outcome.judgedResult->score.poor == (midway ? 1 : 2) &&
                   outcome.judgedResult->score.comboBreak == (midway ? 1 : 2),
               "COR02: reconstructed abort accounts every remaining note exactly once");
        expect(outcome.playable() && outcome.replayData->staleResult,
               "COR02: reproducible terminal drift is playable with a stale result");
        if (outcome.judgedResult) {
          const auto accepted = ReplayPlaybackMaterializer::materializeForConsumers(
              raw, *outcome.judgedResult, chart, 128);
          expect(accepted.replayData && accepted.replayData->abortedAtSongTimeMicros ==
                                          raw.timeBounds.completionSongTimeMicros,
                 "COR02: agreed consumer track retains the terminal abort boundary");
          raw.timeBounds.aborted = false;
          const auto forgedFlag = ReplayPlaybackMaterializer::materializeForConsumers(
              raw, *outcome.judgedResult, chart, 128);
          expect(forgedFlag.playable() && forgedFlag.replayData->staleResult,
                 "COR02: changed terminal evidence exposes recomputed stale facts");
        }
      }
    }
  }
}

void testAuthoritativeAbortRejectsPostAbortStreams() {
  for (const auto completion : {-1'000'000LL, 1'000'000LL}) {
    for (const int stream : {0, 1, 2}) {
      auto value = document();
      value.timeBounds = {completion, true};
      value.playback.input.clear();
      value.playback.touchSamples.clear();
      value.playback.laneCoverEvents.clear();
      if (stream == 0) {
        value.playback.input.push_back({.songTimeMicros = completion + 1,
            .control = {.kind = LogicalControlKind::Lane, .player = 1, .lane = 0},
            .pressed = true});
      } else if (stream == 1) {
        value.playback.touchSamples.push_back({.action = replay::ReplayTouchAction::Down,
            .fingerId = 1, .songTimeMicros = completion + 1, .x = 0.5F, .y = 0.5F});
      } else {
        value.playback.laneCoverEvents.push_back({.songTimeMicros = completion + 1,
            .noteStartPositionPercent = 20});
      }
      const auto captured = replayCaptureTimeBounds(value.timeBounds,
          value.playback.input, value.playback.touchSamples, value.playback.laneCoverEvents);
      expect(captured == value.timeBounds,
             "post-abort evidence never extends an authoritative abort boundary");
      expect(!validateReplayPlayback(value.playback, ReplaySetupSource::AsoExtension,
                                     captured).valid(),
             "post-abort evidence fails canonical validation instead of moving abort time");
    }
  }
}

void testEmptyAbortDriverUsesConfiguredPreRoll() {
  for (const bool wider : {true, false}) {
    auto limits = kReplayLimits;
    limits.minimumSongTimeMicros = wider ? -60'000'000 : -1'000'000;
    auto value = document();
    value.timeBounds = {wider ? -40'000'000 : -2'000'000, true};
    value.playback.input.clear();
    value.playback.touchSamples.clear();
    value.playback.laneCoverEvents.clear();
    ReplayPlaybackDriver driver(value, limits);
    expect(driver.valid() == wider,
           wider ? "empty -40s abort driver accepts configured -60s pre-roll"
                 : "empty -2s abort driver rejects configured -1s pre-roll");
    if (wider) {
      const auto advanced = driver.advanceTo(-40'000'000, {}, 1);
      expect(advanced.advanced() && driver.complete(),
             "empty abort reaches its signed completion with wider configured pre-roll");
    }
  }
}

int main() {
  testClassicReplayCompletionPreservesOtherLaneMineInput();
  testHeldClassicReplayMatchesLiveJudgementAndBestGhost();
  testConcreteMaterializerSettlesExactTimeMineInput();
  testEmptyAbortDriverUsesConfiguredPreRoll();
  testAuthoritativeAbortRejectsPostAbortStreams();
  testAbortedRawReplayReconstructsFailureWithoutTrustingSummary();
  testDriverMergesStreamsWithoutChangingTheirTiming();
  testDriverTrustsStructurallyValidatedDocument();
  testDriverRejectsReverseTimeAndBoundsEachAdvance();
  testLogicalGameplayAdapterOwnsLaneAndScratchMapping();
  testLogicalGameplayAdapterAllowsOverlappingStockScratchDirections();
  testLogicalGameplayAdapterPreservesReplayOnlyScratchHandoffs();
  testMaterializerUsesDriverAndOnlyComparesSavedFacts();
  testConcreteMaterializerBuildsConsumerTrackDespiteResultDisagreement();
  testConsumerSetupAdapterOwnsEveryReplaySetupTranslation();
  testChartConsumerOwnsTheEntireVerifiedPreparationPipeline();
  testMaterializationBudgetStopsBeforeResultConstruction();
  if (failures != 0) {
    std::cerr << failures << " replay playback driver test(s) failed\n";
    return 1;
  }
  std::cout << "replay playback driver tests passed\n";
  return 0;
}
