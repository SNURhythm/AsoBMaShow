#include "scene/play/PlayfieldChartVisualModel.h"
#include "scene/play/PlayfieldVisualState.h"

#include "bms_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bms_parser::TimeLine *addTimeline(bms_parser::Measure &measure,
                                  long long timingMicros, double beat,
                                  double bpm, double scroll = 1.0) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  timeline->BeatPosition = beat;
  timeline->Bpm = bpm;
  timeline->Scroll = scroll;
  measure.TimeLines.push_back(timeline);
  return timeline;
}

struct ChartFixture {
  bms_parser::Chart chart;
  bms_parser::Note *normal = nullptr;
  bms_parser::Note *invisible = nullptr;
  bms_parser::LandmineNote *mine = nullptr;
  bms_parser::LongNote *longHead = nullptr;
  bms_parser::LongNote *longTail = nullptr;

  ChartFixture() {
    chart.Meta.SHA256 = std::string(64, 'a');
    chart.Meta.KeyMode = 7;
    chart.Meta.Title = "Snapshot Song";
    chart.Meta.SubTitle = "Immutable Mix";
    chart.Meta.Artist = "Pointer Free";
    chart.Meta.SubArtist = "Value Copy";
    chart.Meta.Genre = "TEST";
    chart.Meta.Bpm = 120.0;

    auto *measure = new bms_parser::Measure();
    auto *first = addTimeline(*measure, 0, 0.0, 120.0, 1.0);
    first->IsFirstInMeasure = true;
    normal = new bms_parser::Note(1);
    invisible = new bms_parser::Note(2);
    mine = new bms_parser::LandmineNote(12.0F);
    first->SetNote(1, normal);
    first->SetInvisibleNote(2, invisible);
    first->SetLandmineNote(3, mine);

    auto *bgaOnly = addTimeline(*measure, 500'000, 1.0, 120.0, 2.0);
    bgaOnly->BgaBase = 7;

    auto *headTimeline =
        addTimeline(*measure, 1'000'000, 2.0, 150.0, 2.0);
    auto *tailTimeline =
        addTimeline(*measure, 2'000'000, 4.0, 150.0, 0.5);
    longHead = new bms_parser::LongNote(
        3, bms_parser::LongNoteType::HellChargeNote);
    longTail = new bms_parser::LongNote(
        3, bms_parser::LongNoteType::HellChargeNote);
    longHead->Tail = longTail;
    longTail->Head = longHead;
    headTimeline->SetNote(4, longHead);
    tailTimeline->SetNote(4, longTail);
    chart.Measures.push_back(measure);
  }
};

const ChartVisualNote *noteFor(const PlayfieldChartVisualModel &model,
                               int lane, ChartVisualNoteKind kind) {
  for (const auto &note : model.notes) {
    if (note.lane == lane && note.kind == kind) {
      return &note;
    }
  }
  return nullptr;
}

const PresentationTouchPoint *touchFor(
    const std::vector<PresentationTouchPoint> &touches, long long fingerId,
    ReplayTouchAction action) {
  const auto it = std::ranges::find_if(
      touches, [fingerId, action](const PresentationTouchPoint &touch) {
        return touch.fingerId == fingerId && touch.action == action;
      });
  return it == touches.end() ? nullptr : &*it;
}

void testChartModelOwnsStablePointerFreeValues() {
  ChartFixture fixture;
  const auto model = buildPlayfieldChartVisualModel(fixture.chart, 0);

  require(model.chartSha256 == std::string(64, 'a') && model.keyCount == 7,
          "chart identity and key count are copied");
  require(model.laneOrder == fixture.chart.Meta.GetTotalLaneIndices(),
          "authored lane order is copied by value");
  require(model.text.title == "Snapshot Song" &&
              model.text.subtitle == "Immutable Mix" &&
              model.text.artist == "Pointer Free" &&
              model.text.subartist == "Value Copy" &&
              model.text.genre == "TEST",
          "all gameplay chart strings are copied");
  require(model.text.auditedStringProperties.at(12) ==
              "Snapshot Song Immutable Mix" &&
              model.text.auditedStringProperties.at(13) == "TEST" &&
              model.text.auditedStringProperties.at(14) == "Pointer Free" &&
              model.text.auditedStringProperties.at(15) == "Value Copy",
          "Task 1 audited chart string properties are preserved exactly");
  require(model.runtimeStrings() ==
              std::vector<std::string>{"Snapshot Song", "Immutable Mix",
                                       "Pointer Free", "Value Copy", "TEST",
                                       "Snapshot Song Immutable Mix"},
          "runtime glyph strings are stable and deduplicated");
  require(model.timelines.size() == 4 && model.scrollPrefix.size() == 4 &&
              model.timelines[1].bgaOnly &&
              model.timelines[0].sectionLine &&
              model.timelines[2].scrollPosition == 3.0 &&
              model.scrollPrefix[3] == 7.0,
          "timeline identities, BGA-only rows, and scroll prefixes are copied");

  const auto *normal = noteFor(model, 1, ChartVisualNoteKind::Normal);
  const auto *invisible = noteFor(model, 2, ChartVisualNoteKind::Invisible);
  const auto *mine = noteFor(model, 3, ChartVisualNoteKind::Mine);
  const ChartVisualNote *head = nullptr;
  const ChartVisualNote *tail = nullptr;
  for (const auto &note : model.notes) {
    if (note.kind == ChartVisualNoteKind::LongHead) {
      head = &note;
    } else if (note.kind == ChartVisualNoteKind::LongTail) {
      tail = &note;
    }
  }
  require(normal != nullptr && normal->kind == ChartVisualNoteKind::Normal &&
              invisible != nullptr &&
              invisible->kind == ChartVisualNoteKind::Invisible &&
              mine != nullptr && mine->mineDamage == 12 &&
              head != nullptr && tail != nullptr &&
              head->pairId == tail->id && tail->pairId == head->id &&
              head->longNoteMode == ChartLongNoteMode::HCN &&
              tail->longNoteMode == ChartLongNoteMode::HCN,
          "normal, invisible, and paired HCN values use stable IDs");

  fixture.chart.Meta.Title = "Mutated Host Title";
  fixture.normal->IsDead = true;
  require(model.text.title == "Snapshot Song" && !model.notes.empty(),
          "model values do not alias parser or chart state");
}

void testLongNoteModeUsesChartThenOverridePrecedence() {
  ChartFixture fixture;
  fixture.longHead->SetType(bms_parser::LongNoteType::Undefined);
  fixture.longTail->SetType(bms_parser::LongNoteType::Undefined);

  fixture.chart.Meta.LnMode = 2;
  const auto chartMode = buildPlayfieldChartVisualModel(fixture.chart, 3);
  const auto *chartModeHead =
      noteFor(chartMode, 4, ChartVisualNoteKind::LongHead);
  require(chartModeHead != nullptr &&
              chartModeHead->longNoteMode == ChartLongNoteMode::CN,
          "authored chart LN mode takes precedence over the user override");

  fixture.chart.Meta.LnMode = 0;
  const auto overrideMode = buildPlayfieldChartVisualModel(fixture.chart, 3);
  const auto *overrideHead =
      noteFor(overrideMode, 4, ChartVisualNoteKind::LongHead);
  require(overrideHead != nullptr &&
              overrideHead->longNoteMode == ChartLongNoteMode::HCN,
          "user LN mode resolves undefined chart long notes");

  const auto defaultMode = buildPlayfieldChartVisualModel(fixture.chart, 0);
  const auto *defaultHead =
      noteFor(defaultMode, 4, ChartVisualNoteKind::LongHead);
  require(defaultHead != nullptr &&
              defaultHead->longNoteMode == ChartLongNoteMode::LN,
          "undefined long-note mode has a stable classic-LN fallback");
}

struct ObservingPresentation final : IPlayfieldPresentationEvents {
  const PlayfieldVisualStateStore *store = nullptr;
  int pressCount = 0;
  int releaseCount = 0;
  int judgeCount = 0;
  bool observedStoreFirst = true;

  void onLanePressed(int lane, JudgeResult, long long eventMicros) override {
    ++pressCount;
    const auto snapshot = store->capture({});
    observedStoreFirst =
        observedStoreFirst && snapshot.lanes.at(2).pressed &&
        snapshot.lanes.at(2).pressMicros == eventMicros && lane == 1;
  }

  void onLaneReleased(int lane, long long eventMicros) override {
    ++releaseCount;
    const auto snapshot = store->capture({});
    observedStoreFirst =
        observedStoreFirst && !snapshot.lanes.at(2).pressed &&
        snapshot.lanes.at(2).releaseMicros == eventMicros && lane == 1;
  }

  void onJudge(JudgeResult judge, int combo, int score,
               PlayfieldJudgeEventClock clock,
               bool) override {
    ++judgeCount;
    const auto snapshot = store->capture({});
    observedStoreFirst =
        observedStoreFirst && snapshot.lastJudge.judgement == judge.judgement &&
        snapshot.combo == combo && snapshot.score == score &&
        snapshot.lastJudgeVisualMicros == clock.visualTimeMicros;
  }
};

void testVisualStateCaptureAndFanoutAreCoherentValueSnapshots() {
  ChartFixture fixture;
  const auto model = buildPlayfieldChartVisualModel(fixture.chart, 0);
  PlayfieldVisualStateStore store(model);

  PlayfieldPresentationConfig configuration{
      .visibleTimeGreenNumber = 525,
      .visibleTimeUseMilliseconds = true,
      .visibleTimeBpmStrategy =
          AppSettings::VisibleTimeBpmStrategy::MostPrevalent,
      .playAreaWidth = 6.5F,
      .laneBeamsEnabled = false,
      .laneCoverFloatingEnabled = false,
      .laneBeamLengthPercent = 73,
      .noteStartPositionPercent = 22,
      .laneBeamClockUsesRenderTime = true,
      .showInvisibleNotes = true,
      .judgementIndicatorEnabled = false,
      .judgementIndicatorY = 0.25F,
      .judgementIndicatorWidthScale = 1.5F,
      .judgementIndicatorHudMode = true,
      .judgementIndicatorRangeMilliseconds = 180,
      .judgementTextY = 0.35F,
      .judgementCounterEnabled = true,
      .judgementCounterPosition =
          AppSettings::JudgementCounterPosition::Left,
      .fastSlowCriteria =
          AppSettings::JudgementTimingDisplayCriteria::BadOrBelow,
      .millisecondsCriteria =
          AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow,
      .gaugeBarPosition = AppSettings::GaugeBarPosition::Right,
      .touchVisualizationEnabled = true,
      .replayGhostRenderingEnabled = false,
  };
  store.setConfiguration(configuration);

  PlayfieldAuthorityUpdate authority;
  authority.currentBpm = 175.0;
  authority.judgementCounters[PGreat] = 4;
  authority.comboBreak = 2;
  authority.gaugeType = GaugeType::Hard;
  authority.gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove;
  authority.currentGauge = 64.5F;
  authority.gaugeRules.totalNotes = 321;
  authority.pacemakerTarget = {.enabled = true, .label = "AA"};
  authority.pacemakerStatus = {.enabled = true, .label = "AA", .delta = 12};
  authority.playOptionLabel = "MIRROR";
  authority.autoPlayMarkVisible = true;
  authority.startLaneIndicators = {1, 2};
  authority.startLaneIndicatorsVisible = true;
  authority.laneCoverPercent = 24;
  authority.resetLaneCoverVisibleTimeReference = true;
  store.applyAuthorityUpdate(authority);
  store.setSceneStartMicros(10);
  store.setPlayStartMicros(20);

  const auto firstNoteId = model.notes.front().id;
  store.setNoteState({.id = firstNoteId,
                      .judged = true,
                      .dead = false,
                      .longActive = false,
                      .longDamaged = false,
                      .longReactive = false});
  const auto *longHead = noteFor(model, 4, ChartVisualNoteKind::LongHead);
  const auto *mine = noteFor(model, 3, ChartVisualNoteKind::Mine);
  require(longHead != nullptr && mine != nullptr,
          "fixture exposes stable long-note and mine identities");
  store.setNoteState({.id = longHead->id,
                      .judged = true,
                      .dead = false,
                      .playedTimeMicros = 777,
                      .longActive = true,
                      .longDamaged = true,
                      .longReactive = true});
  store.setNoteState({.id = mine->id, .judged = true, .dead = true});
  store.setLiveTouchPoint(9, ReplayTouchAction::Down, 0.25F, 0.75F, 33);

  ObservingPresentation first;
  first.store = &store;
  ObservingPresentation second;
  second.store = &store;
  PlayfieldPresentationEventFanout fanout(store, first);
  fanout.onLanePressed(1, JudgeResult(PGreat, -250), 0);
  fanout.onJudge(JudgeResult(Great, 1'500), 7, 42,
                 {.songTimeMicros = 100,
                  .visualTimeMicros = 125,
                  .bgaTimeMicros = 150},
                 true);
  fanout.onLaneReleased(1, 200);
  fanout.setPresentationSink(second);
  fanout.onLanePressed(1, JudgeResult(None, 0), 300);

  const PlayfieldFrameClock clock{.serial = 99,
                                  .visualTimeMicros = 1'000,
                                  .gameplayTimeMicros = 900,
                                  .replayTouchTimeMicros = 800,
                                  .bgaTimeMicros = 700};
  const auto captured = store.capture(clock);
  const auto capturedLong = std::ranges::find_if(
      captured.notes,
      [longHead](const auto &note) { return note.id == longHead->id; });
  const auto capturedMine = std::ranges::find_if(
      captured.notes,
      [mine](const auto &note) { return note.id == mine->id; });
  require(first.pressCount == 1 && first.releaseCount == 1 &&
              first.judgeCount == 1 && second.pressCount == 1 &&
              second.releaseCount == 0 && second.judgeCount == 0 &&
              first.observedStoreFirst && second.observedStoreFirst,
          "fanout updates the store first and forwards once to only its current sink");
  require(captured.clock.serial == 99 && captured.configuration == configuration &&
              captured.authority == authority && captured.lanes.at(2).pressed &&
              captured.lanes.at(2).lastPressedJudge.judgement == None &&
              captured.lanes.at(2).pressMicros == 300 &&
              captured.lanes.at(2).releaseMicros == 200 &&
              captured.lanes.at(2).bombMicros == 0 &&
              captured.lastJudge.judgement == Great &&
              captured.lastJudgeVisualMicros == 125 && captured.combo == 7 &&
              captured.score == 42 && captured.fastSlowMicros == 1'500 &&
              captured.notes.front().id == firstNoteId &&
              captured.notes.front().judged &&
              capturedLong != captured.notes.end() &&
              capturedLong->playedTimeMicros == 777 &&
              capturedLong->longActive && capturedLong->longDamaged &&
              capturedLong->longReactive &&
              capturedMine != captured.notes.end() && capturedMine->judged &&
              capturedMine->dead && captured.touches.size() == 1 &&
              captured.touches.front().fingerId == 9 &&
              captured.sceneStartMicros == 10 && captured.playStartMicros == 20,
          "captured frame contains the coherent configuration, authority, event, note, touch, and timer values");

  store.clearLiveTouchPoints();
  store.setSceneStartMicros(999);
  fanout.onLaneReleased(1, 400);
  require(captured.touches.size() == 1 && captured.sceneStartMicros == 10 &&
              captured.lanes.at(2).releaseMicros == 200,
          "a captured DTO remains immutable after later store changes");
  require(captured.lanes.at(2).bombMicros == 0,
          "time zero is a valid bomb timestamp rather than the OFF sentinel");
  require(captured.lanes.front().pressMicros ==
              std::numeric_limits<long long>::min(),
          "untouched lane timestamps use the shared OFF sentinel");
}

void testJudgeTimingIsClampedToThePublicStateWidth() {
  ChartFixture fixture;
  const auto model = buildPlayfieldChartVisualModel(fixture.chart, 0);
  PlayfieldVisualStateStore store(model);
  store.onJudge(
      JudgeResult(Great, std::numeric_limits<long long>::max()), 1, 2,
      {.songTimeMicros = 10, .visualTimeMicros = 20, .bgaTimeMicros = 30},
      true);
  require(store.capture({}).fastSlowMicros == std::numeric_limits<int>::max(),
          "judge timing is clamped instead of overflowing its DTO field");
}

struct BgaCapturePresentation final : IPlayfieldPresentationEvents {
  const PlayfieldVisualStateStore *store = nullptr;
  bool observedCapturedMissState = false;

  void onLanePressed(int, JudgeResult, long long) override {}
  void onLaneReleased(int, long long) override {}
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
               bool) override {
    const auto captured = store->capture({});
    observedCapturedMissState =
        captured.bgaMiss.active && captured.bgaMiss.startedBgaMicros == 321 &&
        captured.bgaMiss.durationMicros == kDefaultMissLayerDurationMicros &&
        captured.bgaMiss.triggerSerial == 1;
  }
};

void testCapturedBgaMissStateTracksJudgesAndResets() {
  ChartFixture fixture;
  const auto model = buildPlayfieldChartVisualModel(fixture.chart, 0);
  PlayfieldVisualStateStore store(model);

  store.onJudge(JudgeResult(None, 0), 0, 0,
                {.songTimeMicros = 1,
                 .visualTimeMicros = 999,
                 .bgaTimeMicros = 123},
                true);
  auto captured = store.capture({});
  require(!captured.bgaMiss.active && captured.bgaMiss.triggerSerial == 0,
          "None judgements do not enter captured BGA miss state");

  store.onJudge(JudgeResult(Great, 0), 0, 1,
                {.songTimeMicros = 2,
                 .visualTimeMicros = 999,
                 .bgaTimeMicros = 0},
                true);
  captured = store.capture({});
  require(captured.bgaMiss.active && captured.bgaMiss.startedBgaMicros == 0 &&
              captured.bgaMiss.durationMicros ==
                  kDefaultMissLayerDurationMicros &&
              captured.bgaMiss.triggerSerial == 1,
          "captured BGA miss state retains a valid zero timestamp");

  store.onJudge(JudgeResult(Kpoor, 0), 0, 2,
                {.songTimeMicros = 3,
                 .visualTimeMicros = 1,
                 .bgaTimeMicros = 456},
                true);
  captured = store.capture({});
  require(captured.bgaMiss.startedBgaMicros == 456 &&
              captured.bgaMiss.triggerSerial == 2,
          "repeated Kpoor-at-zero refreshes the captured BGA miss state");

  store.resetModel(model);
  captured = store.capture({});
  require(!captured.bgaMiss.active && captured.bgaMiss.startedBgaMicros == 0 &&
              captured.bgaMiss.durationMicros ==
                  kDefaultMissLayerDurationMicros &&
              captured.bgaMiss.triggerSerial == 0,
          "model reset clears captured BGA miss state");
}

void testBgaMissStatePropagatesThroughEventFanoutBeforeCapture() {
  ChartFixture fixture;
  PlayfieldVisualStateStore store(
      buildPlayfieldChartVisualModel(fixture.chart, 0));
  BgaCapturePresentation presentation;
  presentation.store = &store;
  PlayfieldPresentationEventFanout fanout(store, presentation);

  fanout.onJudge(JudgeResult(Poor, 0), 0, 1,
                 {.songTimeMicros = 100,
                  .visualTimeMicros = 200,
                  .bgaTimeMicros = 321},
                 true);
  require(presentation.observedCapturedMissState,
          "judge fanout updates BGA miss state before presentation capture");
}

void testEventClockUsesSourceTimeAndIndependentOffsets() {
  const auto clock =
      makePlayfieldJudgeEventClock(1'000'000, 125'000, 50'000);
  require(clock == PlayfieldJudgeEventClock{.songTimeMicros = 1'000'000,
                                             .visualTimeMicros = 875'000,
                                             .bgaTimeMicros = 950'000},
          "judge clocks derive visual and BGA time once from source time");
  require(playfieldVisualEventTimeMicros(1'000'000, 125'000) == 875'000,
          "lane and judge events share the same visual-time domain");
}

void testLiveReleasedTouchesLingerThenPrune() {
  ChartFixture fixture;
  PlayfieldVisualStateStore store(
      buildPlayfieldChartVisualModel(fixture.chart, 0));

  store.setLiveTouchPoint(3, ReplayTouchAction::Down, 0.25F, 0.75F, 100);
  store.setLiveTouchPoint(3, ReplayTouchAction::Up, 0.50F, 0.60F, 200);
  store.setLiveTouchPoint(4, ReplayTouchAction::Down, 0.10F, 0.20F, 100);
  store.setLiveTouchPoint(4, ReplayTouchAction::Cancel, 0.30F, 0.40F, 200);

  const auto atLingerBoundary = store.capture({.replayTouchTimeMicros = 180'200});
  require(atLingerBoundary.touches.size() == 2 &&
              std::ranges::all_of(atLingerBoundary.touches,
                                  [](const auto &touch) {
                                    return touch.action == ReplayTouchAction::Up ||
                                           touch.action == ReplayTouchAction::Cancel;
                                  }),
          "live Up and Cancel replace active touches and remain through the linger boundary");

  const auto afterLinger = store.capture({.replayTouchTimeMicros = 180'201});
  require(afterLinger.touches.empty(),
          "live released touches prune immediately after the 180ms linger period");
}

void testReplayTouchesUseCopiedStableSortedLifecycleValues() {
  ChartFixture fixture;
  PlayfieldVisualStateStore store(
      buildPlayfieldChartVisualModel(fixture.chart, 0));
  std::vector<ReplayTouchSample> samples{
      {.action = ReplayTouchAction::Up,
       .fingerId = 8,
       .songTimeMicros = 300,
       .x = 0.90F,
       .y = 0.80F},
      {.action = ReplayTouchAction::Down,
       .fingerId = 8,
       .songTimeMicros = 100,
       .x = 0.20F,
       .y = 0.30F},
      {.action = ReplayTouchAction::Move,
       .fingerId = 8,
       .songTimeMicros = 200,
       .x = 0.40F,
       .y = 0.50F},
      {.action = ReplayTouchAction::Down,
       .fingerId = 9,
       .songTimeMicros = 200,
       .x = 0.10F,
       .y = 0.20F},
      {.action = ReplayTouchAction::Move,
       .fingerId = 9,
       .songTimeMicros = 200,
       .x = 0.70F,
       .y = 0.80F},
  };
  store.setReplayTouchSamples(samples);
  samples[1].x = 0.99F;

  const auto down = store.capture({.replayTouchTimeMicros = 100});
  const auto *downTouch = touchFor(down.touches, 8, ReplayTouchAction::Down);
  require(downTouch != nullptr && downTouch->normalizedX == 0.20F &&
              downTouch->normalizedY == 0.30F,
          "replay Down advances from a copied, time-sorted touch stream");

  const auto move = store.capture({.replayTouchTimeMicros = 200});
  const auto *moveTouch = touchFor(move.touches, 8, ReplayTouchAction::Move);
  const auto *sameTimeMove =
      touchFor(move.touches, 9, ReplayTouchAction::Move);
  require(moveTouch != nullptr && moveTouch->normalizedX == 0.40F &&
              moveTouch->normalizedY == 0.50F && sameTimeMove != nullptr &&
              sameTimeMove->normalizedX == 0.70F,
          "replay Move replaces active values in stable same-timestamp input order");

  const auto up = store.capture({.replayTouchTimeMicros = 300});
  const auto *upTouch = touchFor(up.touches, 8, ReplayTouchAction::Up);
  require(up.touches.size() == 2 && upTouch != nullptr &&
              upTouch->normalizedX == 0.90F && upTouch->normalizedY == 0.80F,
          "replay Up releases the active touch into the presentation linger state");
}

void testReplayTouchRewindRebuildsLifecycleState() {
  ChartFixture fixture;
  PlayfieldVisualStateStore store(
      buildPlayfieldChartVisualModel(fixture.chart, 0));
  store.setReplayTouchSamples({
      {.action = ReplayTouchAction::Down,
       .fingerId = 12,
       .songTimeMicros = 100,
       .x = 0.25F,
       .y = 0.75F},
      {.action = ReplayTouchAction::Up,
       .fingerId = 12,
       .songTimeMicros = 300,
       .x = 0.50F,
       .y = 0.60F},
  });

  const auto released = store.capture({.replayTouchTimeMicros = 300});
  require(touchFor(released.touches, 12, ReplayTouchAction::Up) != nullptr,
          "replay reaches the released lifecycle before rewind");

  const auto rewound = store.capture({.replayTouchTimeMicros = 100});
  require(rewound.touches.size() == 1 &&
              touchFor(rewound.touches, 12, ReplayTouchAction::Down) != nullptr,
          "replay time rewind clears prior lifecycle state before replaying samples");
}

void testReleasedTouchesStayBoundedAcrossChangingFingerIds() {
  ChartFixture fixture;
  PlayfieldVisualStateStore store(
      buildPlayfieldChartVisualModel(fixture.chart, 0));
  for (long long fingerId = 0; fingerId < 512; ++fingerId) {
    store.setLiveTouchPoint(fingerId, ReplayTouchAction::Down, 0.1F, 0.2F,
                            fingerId);
    store.setLiveTouchPoint(fingerId, ReplayTouchAction::Up, 0.3F, 0.4F,
                            fingerId + 1);
  }

  require(store.capture({.replayTouchTimeMicros = 180'513}).touches.empty(),
          "released touch storage prunes stale changing finger IDs instead of growing unbounded");
}

void testTouchResetClearsLiveAndReplayState() {
  ChartFixture fixture;
  const auto model = buildPlayfieldChartVisualModel(fixture.chart, 0);
  PlayfieldVisualStateStore store(model);
  store.setReplayTouchSamples({{.action = ReplayTouchAction::Down,
                                .fingerId = 1,
                                .songTimeMicros = 0,
                                .x = 0.1F,
                                .y = 0.2F}});
  store.setLiveTouchPoint(2, ReplayTouchAction::Down, 0.3F, 0.4F, 0);
  require(store.capture({}).touches.size() == 2,
          "precondition includes separately captured live and replay touches");

  store.resetModel(model);
  require(store.capture({}).touches.empty(),
          "model reset clears live and replay touch lifecycle state cleanly");
}

} // namespace

int main() {
  testChartModelOwnsStablePointerFreeValues();
  testLongNoteModeUsesChartThenOverridePrecedence();
  testVisualStateCaptureAndFanoutAreCoherentValueSnapshots();
  testJudgeTimingIsClampedToThePublicStateWidth();
  testCapturedBgaMissStateTracksJudgesAndResets();
  testBgaMissStatePropagatesThroughEventFanoutBeforeCapture();
  testEventClockUsesSourceTimeAndIndependentOffsets();
  testLiveReleasedTouchesLingerThenPrune();
  testReplayTouchesUseCopiedStableSortedLifecycleValues();
  testReplayTouchRewindRebuildsLifecycleState();
  testReleasedTouchesStayBoundedAcrossChangingFingerIds();
  testTouchResetClearsLiveAndReplayState();
  return 0;
}
