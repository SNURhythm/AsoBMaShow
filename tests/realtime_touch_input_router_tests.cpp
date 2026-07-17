#include "scene/play/RealtimeTouchInputRouter.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct InputCapture {
  std::vector<gameplay::RealtimeGameplayInput> events;

  static bool emit(void *context,
                   const gameplay::RealtimeGameplayInput &event) {
    static_cast<InputCapture *>(context)->events.push_back(event);
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
  require(capture.events.size() == 2 && capture.events[0].epoch == 42 &&
              capture.events[0].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[0].lane == 0 &&
              capture.events[0].steadyTimestampMicros == 123'456 &&
              capture.events[1].type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].lane == 0,
          "native samples preserve their timestamp and lane edge order");
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
              capture.events[2].type ==
                  gameplay::RealtimeGameplayInputType::Press,
          "scratch reversal remains ordered on the realtime ingress");
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

} // namespace

int main() {
  testDirectTouchEmitsTimestampedLaneEdges();
  testDragModeChangesLaneWithoutWaitingForAFrame();
  testScratchFlickEmitsAtomicBackspinAndPressPair();
  testNormalModeMapsTouchesBelowProjectedPlayfield();
  testUiExcludedFingerNeverEmitsGameplayEdges();
  testLayoutReplacementCancelsOldLaneBeforeNewMapping();
  return 0;
}
