#include "scene/play/RealtimeTouchInputRouter.h"
#include "scene/play/RealtimeTouchPresentation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace rendering {
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
} // namespace rendering

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void requireNear(float actual, float expected, const char *message) {
  require(std::abs(actual - expected) < 0.0001F, message);
}

void testTouchPresentationUsesUiNormalizedCoordinates() {
  rendering::window_width = 1920;
  rendering::window_height = 1080;
  rendering::render_width = 2400;
  rendering::render_height = 1200;
  rendering::ui_scale_x = 1.0F;
  rendering::ui_scale_y = 1.0F;
  rendering::ui_offset_x = 240;
  rendering::ui_offset_y = 60;

  const auto topLeft =
      gameplay::realtimeTouchPresentationPoint(0.1F, 0.05F);
  const auto center = gameplay::realtimeTouchPresentationPoint(0.5F, 0.5F);
  const auto bottomRight =
      gameplay::realtimeTouchPresentationPoint(0.9F, 0.95F);

  requireNear(topLeft.x, 0.0F,
              "presentation removes the horizontal UI viewport offset");
  requireNear(topLeft.y, 0.0F,
              "presentation removes the vertical UI viewport offset");
  requireNear(center.x, 0.5F,
              "presentation preserves the UI viewport center x");
  requireNear(center.y, 0.5F,
              "presentation preserves the UI viewport center y");
  requireNear(bottomRight.x, 1.0F,
              "presentation maps the UI viewport right edge to one");
  requireNear(bottomRight.y, 1.0F,
              "presentation maps the UI viewport bottom edge to one");
}

struct InputCapture {
  std::vector<gameplay::RealtimeGameplayInput> events;
  std::vector<gameplay::RealtimeTouchSample> cancelledTouches;
  bool scratchLongNoteHeld = false;

  static bool emit(void *context,
                   const gameplay::RealtimeGameplayInput &event) {
    static_cast<InputCapture *>(context)->events.push_back(event);
    return true;
  }

  static bool longScratchNoteHeld(void *context, int) {
    return static_cast<InputCapture *>(context)->scratchLongNoteHeld;
  }

  static bool cancelTouchLifecycle(
      void *context, const gameplay::RealtimeTouchSample &sample) {
    static_cast<InputCapture *>(context)->cancelledTouches.push_back(sample);
    return true;
  }
};

gameplay::RealtimeTouchLayout makeLayout(bool dragMode = false) {
  gameplay::RealtimeTouchLayout layout;
  layout.bottomLeft = {0.1F, 0.9F};
  layout.bottomRight = {0.9F, 0.9F};
  layout.topLeft = {0.3F, 0.1F};
  layout.topRight = {0.7F, 0.1F};
  layout.laneCount = 4;
  layout.lanes[0] = 0;
  layout.lanes[1] = 1;
  layout.lanes[2] = 2;
  layout.lanes[3] = 7;
  layout.scratch[3] = true;
  layout.dragMode = dragMode;
  return layout;
}

void testChartLaneMappingCoversEveryReplayKeyMode() {
  for (const auto &layout : replay::kReplayKeyModeLayouts) {
    const auto first = replay::logicalControlForChartLane(
        layout.keyMode, 0, false);
    require(first == replay::LogicalControl{
                         .kind = replay::LogicalControlKind::Lane,
                         .player = 1,
                         .lane = 0},
            "the first chart lane maps to player one's first logical lane");

    if (layout.players == 2) {
      const int offset = layout.keyMode == 48 ? 26 : 8;
      const auto second = replay::logicalControlForChartLane(
          layout.keyMode, offset, false);
      require(second == replay::LogicalControl{
                            .kind = replay::LogicalControlKind::Lane,
                            .player = 2,
                            .lane = 0},
              "double-play chart lanes use the shared player offset");
    }

    if (layout.hasDirectionalScratch) {
      const auto scratch = replay::logicalControlForChartLane(
          layout.keyMode, 7, true,
          replay::LogicalControlKind::ScratchCounterClockwise);
      require(scratch == replay::LogicalControl{
                             .kind = replay::LogicalControlKind::
                                 ScratchCounterClockwise,
                             .player = 1,
                             .lane = -1},
              "directional scratch maps through the shared lane authority");
    }
  }
}

void testDirectTouchEmitsTimestampedLaneEdges() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      42, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 11,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 123'456}),
          "touch down is accepted");
  require(router.consume({.fingerId = 11,
                          .phase = gameplay::RealtimeTouchPhase::Up,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 124'000}),
          "touch up is accepted");
  require(
      capture.events.size() == 2 && capture.events[0].epoch == 42 &&
          capture.events[0].type ==
              gameplay::RealtimeGameplayInputType::Press &&
          capture.events[0].source ==
              gameplay::RealtimeGameplayInputSource::Touch &&
          capture.events[0].lane == 0 && capture.events[0].hasReplayControl &&
          capture.events[0].replayControl ==
              replay::LogicalControl{.kind = replay::LogicalControlKind::Lane,
                                     .player = 1,
                                     .lane = 0} &&
          capture.events[0].steadyTimestampMicros == 123'456 &&
          capture.events[1].type ==
              gameplay::RealtimeGameplayInputType::Release &&
          capture.events[1].source ==
              gameplay::RealtimeGameplayInputSource::Touch &&
          capture.events[1].lane == 0,
      "native samples preserve their timestamp and lane edge order");
}

void testScratchlessTouchKeepsGameplayIndependentFromBrdMapping() {
  for (const int keyMode : {4, 6, 8}) {
    InputCapture capture;
    auto layout = makeLayout();
    layout.keyMode = keyMode;
    layout.laneCount = static_cast<std::size_t>(keyMode);
    for (int lane = 0; lane < keyMode; ++lane) {
      layout.lanes[static_cast<std::size_t>(lane)] = lane;
      layout.scratch[static_cast<std::size_t>(lane)] = false;
    }
    gameplay::RealtimeTouchInputRouter router(
        42, layout, {.context = &capture, .emit = &InputCapture::emit});

    require(router.consume({.fingerId = keyMode,
                            .phase = gameplay::RealtimeTouchPhase::Down,
                            .normalizedX = 0.21F,
                            .normalizedY = 0.5F,
                            .steadyTimestampMicros = 123'456}),
            "scratchless touch down reaches gameplay");
    require(router.consume({.fingerId = keyMode,
                            .phase = gameplay::RealtimeTouchPhase::Up,
                            .normalizedX = 0.21F,
                            .normalizedY = 0.5F,
                            .steadyTimestampMicros = 124'000}),
            "scratchless touch up reaches gameplay");
    require(capture.events.size() == 2 && capture.events[0].lane == 0 &&
                capture.events[1].lane == 0 &&
                !capture.events[0].hasReplayControl &&
                !capture.events[1].hasReplayControl,
            "non-stock BRD modes keep physical lane edges without inventing "
            "replay controls");
  }
}

void testDragModeChangesLaneWithoutWaitingForAFrame() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      7, makeLayout(true), {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.39F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}),
          "drag touch starts in lane one");
  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.61F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 2}),
          "drag move changes lanes");
  require(capture.events.size() == 3 && capture.events[0].lane == 1 &&
              capture.events[1].type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].lane == 1 &&
              capture.events[2].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[2].lane == 2,
          "drag movement serializes release before the next press");
}

void testScratchFlickEmitsAtomicBackspinAndPressPair() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      9, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 3,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.75F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 10}),
          "scratch touch begins without a key edge");
  require(router.consume({.fingerId = 3,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.75F,
                          .normalizedY = 0.48F,
                          .steadyTimestampMicros = 20}),
          "first scratch direction emits a press");
  require(router.consume({.fingerId = 3,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.75F,
                          .normalizedY = 0.51F,
                          .steadyTimestampMicros = 30}),
          "direction reversal emits backspin release and press");
  require(capture.events.size() == 3 &&
              capture.events[0].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[1].type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].backSpin &&
              capture.events[1].hasReplayControl &&
              capture.events[1].replayControl.kind ==
                  replay::LogicalControlKind::ScratchClockwise &&
              capture.events[2].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[2].hasReplayControl &&
              capture.events[2].replayControl.kind ==
                  replay::LogicalControlKind::ScratchCounterClockwise,
          "scratch reversal remains ordered on the realtime ingress");
}

void testScratchLongNoteIgnoresSmallDirectionJitter() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      11, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .scratchLongNoteHeld = &InputCapture::longScratchNoteHeld});
  require(router.consume({.fingerId = 24,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.75F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 130}) &&
              router.consume({.fingerId = 24,
                              .phase = gameplay::RealtimeTouchPhase::Move,
                              .normalizedX = 0.75F,
                              .normalizedY = 0.48F,
                              .steadyTimestampMicros = 140}),
          "scratch-LN fixture emits its initial direction press");
  capture.scratchLongNoteHeld = true;
  require(router.consume({.fingerId = 24,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.75F,
                          .normalizedY = 0.485F,
                          .steadyTimestampMicros = 150}),
          "small reverse movement is accepted during a scratch LN");
  require(capture.events.size() == 1 &&
              capture.events.front().type ==
                  gameplay::RealtimeGameplayInputType::Press,
          "active scratch LN jitter emits neither release nor re-press");
}

void testNormalModeMapsTouchesBelowProjectedPlayfield() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      5, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 20,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.98F,
                          .steadyTimestampMicros = 50}),
          "normal-mode touch below the judge line is accepted");
  require(capture.events.size() == 1 &&
              capture.events[0].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[0].lane == 1,
          "normal mode preserves horizontal lane mapping below the playfield");
}

void testUiExcludedFingerNeverEmitsGameplayEdges() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      6, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 21,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 60,
                          .excludedFromGameplay = true}) &&
              router.consume({.fingerId = 21,
                              .phase = gameplay::RealtimeTouchPhase::Move,
                              .normalizedX = 0.5F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 70,
                              .excludedFromGameplay = true}) &&
              router.consume({.fingerId = 21,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .normalizedX = 0.5F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 80,
                              .excludedFromGameplay = true}),
          "UI-owned touch lifecycle is accepted without gameplay mutation");
  require(capture.events.empty(),
          "pause and lane-cover fingers never reach gameplay authority");
}

void testLayoutReplacementCancelsOldLaneBeforeNewMapping() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      8, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 22,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 90}),
          "layout fixture presses the old lane");
  auto replacement = makeLayout();
  replacement.lanes[0] = 10;
  require(router.updateLayout(replacement, 100),
          "resized layout replaces routing atomically");
  require(capture.events.size() == 2 &&
              capture.events[1].type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].lane == 0,
          "layout replacement releases the old projected lane first");
}

void testPauseReleasesHeldFingerBeforeDisablingGameplay() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      10, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 23,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 110}),
          "pause fixture presses a lane");
  require(router.consume({.fingerId = 23,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.35F,
                          .normalizedY = 0.45F,
                          .steadyTimestampMicros = 112}),
          "pause fixture tracks the finger's latest presentation point");
  require(router.setGameplayEnabled(false, 115),
          "closing the touch gate releases every active finger");
  require(capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events.back().steadyTimestampMicros == 115,
          "pause publishes the release before touch gameplay is disabled");
  require(capture.cancelledTouches ==
              std::vector<gameplay::RealtimeTouchSample>{{
                  .fingerId = 23,
                  .phase = gameplay::RealtimeTouchPhase::Cancel,
                  .normalizedX = 0.35F,
                  .normalizedY = 0.45F,
                  .steadyTimestampMicros = 115,
              }},
          "pause closes the recorded touch lifecycle before detaching input");
  require(router.setGameplayEnabled(true, 116),
          "resuming reopens touch gameplay");
  require(router.consume({.fingerId = 23,
                          .phase = gameplay::RealtimeTouchPhase::Up,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 120}),
          "the stale post-resume lift is harmless");
  require(capture.events.size() == 2,
          "a finger released at pause cannot strand or duplicate a lane edge");
}

void testCancelledTouchUsesGraceAndContinuationCancelsExpiry() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      12, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 30,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 100'000}) &&
              router.consume({.fingerId = 30,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 110'000}),
          "cancelled touch enters its grace period");
  require(capture.events.size() == 1,
          "cancellation does not release the lane immediately");
  require(router.consume({.fingerId = 30,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.49F,
                          .steadyTimestampMicros = 130'000}) &&
              router.consume(
                  {.fingerId = 30,
                   .phase = gameplay::RealtimeTouchPhase::CancelExpired,
                   .steadyTimestampMicros = 160'000}) &&
              router.consume({.fingerId = 30,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.49F,
                              .steadyTimestampMicros = 170'000}),
          "continued touch cancels the pending expiry and later lifts");
  require(capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events.back().steadyTimestampMicros == 170'000,
          "continued touch stays held until its real lift");

  require(router.consume({.fingerId = 31,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 200'000}) &&
              router.consume({.fingerId = 31,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 210'000}) &&
              router.consume(
                  {.fingerId = 31,
                   .phase = gameplay::RealtimeTouchPhase::CancelExpired,
                   .steadyTimestampMicros = 259'999}),
          "expiry before the grace deadline is ignored");
  require(capture.events.size() == 3,
          "early cancellation expiry emits no release");
  require(router.consume(
              {.fingerId = 31,
               .phase = gameplay::RealtimeTouchPhase::CancelExpired,
               .steadyTimestampMicros = 260'000}),
          "cancel grace expires at fifty milliseconds");
  require(capture.events.size() == 4 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events.back().steadyTimestampMicros == 260'000,
          "an uncontinued cancelled touch releases at the grace deadline");
}

} // namespace

int main() {
  testTouchPresentationUsesUiNormalizedCoordinates();
  testChartLaneMappingCoversEveryReplayKeyMode();
  testDirectTouchEmitsTimestampedLaneEdges();
  testScratchlessTouchKeepsGameplayIndependentFromBrdMapping();
  testDragModeChangesLaneWithoutWaitingForAFrame();
  testScratchFlickEmitsAtomicBackspinAndPressPair();
  testScratchLongNoteIgnoresSmallDirectionJitter();
  testNormalModeMapsTouchesBelowProjectedPlayfield();
  testUiExcludedFingerNeverEmitsGameplayEdges();
  testLayoutReplacementCancelsOldLaneBeforeNewMapping();
  testPauseReleasesHeldFingerBeforeDisablingGameplay();
  testCancelledTouchUsesGraceAndContinuationCancelsExpiry();
  return 0;
}
