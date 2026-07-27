#include "replay/ReplayPlayback.h"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

replay::ReplaySetup validSetup(int keyMode = 7) {
  replay::ReplaySetup setup;
  setup.chart.md5 = std::string(32, 'b');
  setup.chart.sha256 = std::string(64, 'a');
  setup.chart.keyMode = keyMode;
  setup.longNoteMode = 1;
  return setup;
}

replay::ReplayPlaybackData validPlayback() {
  replay::ReplayPlaybackData data;
  data.setup = validSetup();
  data.input = {
      {.songTimeMicros = -2'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  data.touchSamples = {
      {.action = replay::ReplayTouchAction::Down,
       .fingerId = 9,
       .songTimeMicros = -1'800'000,
       .x = 0.25F,
       .y = 0.5F},
      {.action = replay::ReplayTouchAction::Up,
       .fingerId = 9,
       .songTimeMicros = 1'100'000,
       .x = 0.25F,
       .y = 0.5F},
  };
  data.laneCoverEvents = {
      {.songTimeMicros = -2'000'000,
       .noteStartPositionPercent = 20,
       .resetVisibleTimeReference = false},
  };
  return data;
}

constexpr replay::ReplayTimeBounds kBounds{
    .completionSongTimeMicros = 5'000'000,
};

replay::ReplayPlaybackValidation validate(
    const replay::ReplayPlaybackData &data,
    const replay::ReplayLimits &limits = replay::kReplayLimits) {
  return replay::validateReplayPlayback(
      data, replay::ReplaySetupSource::LocalCapture, kBounds, limits);
}

void testPreRollIsSharedByEveryTimedCollection() {
  auto data = validPlayback();
  data.input.front().songTimeMicros = -30'000'000;
  data.touchSamples.front().songTimeMicros = -30'000'000;
  data.laneCoverEvents.front().songTimeMicros = -30'000'000;
  expect(validate(data).valid(),
         "input, touch, and lane cover all accept exact pre-roll");

  data = validPlayback();
  data.input.front().songTimeMicros = -30'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::InputTime,
         "input before pre-roll is rejected");

  data = validPlayback();
  data.touchSamples.front().songTimeMicros = -30'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::TouchTime,
         "touch before pre-roll is rejected");

  data = validPlayback();
  data.laneCoverEvents.front().songTimeMicros = -30'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::LaneCoverTime,
         "lane cover before pre-roll is rejected");
}

void testOrderingIsRejectedInsteadOfSorted() {
  auto data = validPlayback();
  data.input.back().songTimeMicros = -2'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::InputOrder,
         "decreasing input order is rejected");

  data = validPlayback();
  data.touchSamples.back().songTimeMicros = -1'800'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::TouchOrder,
         "decreasing touch order is rejected");

  data = validPlayback();
  data.laneCoverEvents.push_back(data.laneCoverEvents.front());
  data.laneCoverEvents.back().songTimeMicros = -2'000'001;
  expect(validate(data).issue == replay::ReplayPlaybackIssue::LaneCoverOrder,
         "decreasing lane-cover order is rejected");
}

void testControlAndScratchDirectionMatrix() {
  struct LaneCase {
    int keyMode;
    int player;
    int lane;
  };
  constexpr std::array laneCases{
      LaneCase{5, 1, 4},  LaneCase{7, 1, 6},  LaneCase{9, 1, 8},
      LaneCase{10, 2, 4}, LaneCase{14, 2, 6}, LaneCase{24, 1, 25},
      LaneCase{48, 2, 25},
  };
  for (const auto &test : laneCases) {
    auto data = validPlayback();
    data.setup = validSetup(test.keyMode);
    data.input = {{.songTimeMicros = 0,
                   .control = {.kind = replay::LogicalControlKind::Lane,
                               .player = test.player,
                               .lane = test.lane},
                   .pressed = true}};
    expect(validate(data).valid(), "highest supported lane is valid");
    ++data.input.front().control.lane;
    expect(validate(data).issue ==
               replay::ReplayPlaybackIssue::InputControl,
           "lane above supported layout is invalid");
  }

  for (int keyMode : {5, 7, 10, 14}) {
    for (auto kind : {replay::LogicalControlKind::ScratchClockwise,
                      replay::LogicalControlKind::ScratchCounterClockwise}) {
      auto data = validPlayback();
      data.setup = validSetup(keyMode);
      data.input = {{.songTimeMicros = 0,
                     .control = {.kind = kind,
                                 .player = keyMode >= 10 ? 2 : 1,
                                 .lane = -1},
                     .pressed = true}};
      expect(validate(data).valid(),
             "both stock scratch directions are valid");
    }
  }

  auto scratchless = validPlayback();
  scratchless.setup = validSetup(9);
  scratchless.input = {{
      .songTimeMicros = 0,
      .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                  .player = 1,
                  .lane = -1},
      .pressed = true,
  }};
  expect(validate(scratchless).issue ==
             replay::ReplayPlaybackIssue::InputControl,
         "scratch direction is invalid for scratchless mode");
}

void testRedundancyAndScratchOwnershipHandoff() {
  auto redundant = validPlayback();
  redundant.input.insert(redundant.input.begin() + 1,
                         redundant.input.front());
  expect(validate(redundant).issue ==
             replay::ReplayPlaybackIssue::RedundantInput,
         "duplicate logical state transition is rejected");

  replay::ReplayPlaybackData handoff;
  handoff.setup = validSetup(7);
  handoff.input = {
      {.songTimeMicros = 0,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 1,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 1,
       .control = {
           .kind = replay::LogicalControlKind::ScratchCounterClockwise,
           .player = 1,
           .lane = -1},
       .pressed = true,
       .replayOnly = true},
  };
  expect(validate(handoff).valid(),
         "same-timestamp opposite scratch ownership handoff is valid");
  handoff.input.back().songTimeMicros = 2;
  expect(validate(handoff).issue ==
             replay::ReplayPlaybackIssue::ScratchHandoff,
         "delayed replay-only scratch handoff is rejected");
}

void testCollectionCountsAndSupplementalValues() {
  replay::ReplayLimits limits = replay::kReplayLimits;
  limits.maxInputTransitions = 2;
  limits.maxTouchSamples = 2;
  limits.maxLaneCoverEvents = 1;
  expect(validate(validPlayback(), limits).valid(),
         "collections at custom maxima are accepted");

  auto data = validPlayback();
  data.input.push_back({.songTimeMicros = 2'000'000,
                        .control = {
                            .kind = replay::LogicalControlKind::Lane,
                            .player = 1,
                            .lane = 1},
                        .pressed = true});
  expect(validate(data, limits).issue ==
             replay::ReplayPlaybackIssue::InputCount,
         "input count above limit is rejected");

  data = validPlayback();
  data.touchSamples.push_back(data.touchSamples.back());
  data.touchSamples.back().songTimeMicros = 2'000'000;
  expect(validate(data, limits).issue ==
             replay::ReplayPlaybackIssue::TouchCount,
         "touch count above limit is rejected");

  data = validPlayback();
  data.laneCoverEvents.push_back(data.laneCoverEvents.back());
  expect(validate(data, limits).issue ==
             replay::ReplayPlaybackIssue::LaneCoverCount,
         "lane-cover count above limit is rejected");

  data = validPlayback();
  data.touchSamples.front().x = 1.01F;
  expect(validate(data).issue ==
             replay::ReplayPlaybackIssue::TouchCoordinate,
         "touch coordinates must remain normalized");

  data = validPlayback();
  data.laneCoverEvents.front().noteStartPositionPercent = 101;
  expect(validate(data).issue ==
             replay::ReplayPlaybackIssue::LaneCoverPercent,
         "lane-cover percentage is bounded");
}

void testCourseEnvelopeSupportsMixedStageSources() {
  replay::CourseReplayPlaybackData empty;
  expect(replay::validateCourseReplayPlayback(empty, {}, {}).issue ==
             replay::ReplayPlaybackIssue::CourseStageCount,
         "empty course replay is rejected");

  replay::CourseReplayPlaybackData course;
  course.stages = {validPlayback(), validPlayback()};
  course.stages[1].setup.chart.md5.clear();
  course.stages[1].setup.ruleset =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  course.restMicrosAfterStage = {3'600'000'000LL, 0};
  const std::array sources{
      replay::ReplaySetupSource::LocalCapture,
      replay::ReplaySetupSource::StockBeatoraja,
  };
  const std::array bounds{kBounds, kBounds};
  expect(replay::validateCourseReplayPlayback(course, sources, bounds).valid(),
         "course accepts per-stage setup authority and bounded rest");

  course.restMicrosAfterStage[0] = 3'600'000'001LL;
  const auto badRest =
      replay::validateCourseReplayPlayback(course, sources, bounds);
  expect(badRest.issue == replay::ReplayPlaybackIssue::CourseRest &&
             badRest.stageIndex == 0,
         "course reports the stage with oversized rest");

  course.restMicrosAfterStage[0] = -1;
  const auto negativeRest =
      replay::validateCourseReplayPlayback(course, sources, bounds);
  expect(negativeRest.issue == replay::ReplayPlaybackIssue::CourseRest &&
             negativeRest.stageIndex == 0,
         "course reports the stage with negative rest");

  course.restMicrosAfterStage = {0};
  expect(replay::validateCourseReplayPlayback(course, sources, bounds).issue ==
             replay::ReplayPlaybackIssue::CourseShape,
         "course stage/source/bounds/rest counts must agree");

  course.restMicrosAfterStage = {0, 0};
  replay::ReplayLimits oneStage = replay::kReplayLimits;
  oneStage.maxCourseStages = 1;
  expect(replay::validateCourseReplayPlayback(
             course, sources, bounds, oneStage).issue ==
             replay::ReplayPlaybackIssue::CourseStageCount,
         "course stage count uses the shared limit");
}

} // namespace

int main() {
  testPreRollIsSharedByEveryTimedCollection();
  testOrderingIsRejectedInsteadOfSorted();
  testControlAndScratchDirectionMatrix();
  testRedundancyAndScratchOwnershipHandoff();
  testCollectionCountsAndSupplementalValues();
  testCourseEnvelopeSupportsMixedStageSources();
  if (failures != 0) {
    std::cerr << failures << " replay playback test(s) failed\n";
    return 1;
  }
  std::cout << "replay playback tests passed\n";
  return 0;
}
