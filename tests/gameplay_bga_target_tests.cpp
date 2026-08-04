#include "audio/GameplayBgaFrame.h"
#include "audio/GameplayBgaMissStateTracker.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <vector>

namespace {

void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

PlayfieldJudgeEventClock clockAt(long long bgaTimeMicros,
                                 long long visualTimeMicros = 0) {
  return {.songTimeMicros = 0,
          .visualTimeMicros = visualTimeMicros,
          .bgaTimeMicros = bgaTimeMicros};
}

void testPreparedFrameRetainsExplicitSurfaceRoles() {
  PreparedGameplayBgaFrame frame{
      .sequence = 42,
      .composition = GameplayBgaComposition::BaseThenLayer,
      .base = PreparedGameplayBgaSurface{.role = GameplayBgaRole::Base,
                                         .mediaKind = GameplayBgaMediaKind::Image,
                                         .surfaceToken = 7,
                                         .sourceWidth = 640,
                                         .sourceHeight = 480},
      .layer = PreparedGameplayBgaSurface{.role = GameplayBgaRole::Layer,
                                          .mediaKind = GameplayBgaMediaKind::Video,
                                          .surfaceToken = 8,
                                          .sourceWidth = 1280,
                                          .sourceHeight = 720},
  };

  require(frame.sequence == 42 && frame.base.has_value() &&
              frame.layer.has_value() && !frame.miss.has_value(),
          "prepared BGA frame keeps its optional role surfaces");
  require(frame.base->role == GameplayBgaRole::Base &&
              frame.layer->role == GameplayBgaRole::Layer &&
              frame.layer->mediaKind == GameplayBgaMediaKind::Video,
          "prepared BGA surfaces retain explicit roles independent of view IDs");
}

void testNoneJudgeDoesNotTriggerMissState() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(None, 0), 0, clockAt(123));

  const auto state = tracker.snapshot();
  require(!state.active && state.triggerSerial == 0,
          "None judgement never creates a miss trigger");
}

void testComboZeroUsesBgaClockAndRepeatedZeroRetriggers() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(Great, 0), 0, clockAt(123, 900'000));
  auto state = tracker.snapshot();
  require(state.active && state.startedBgaMicros == 123 &&
              state.durationMicros == kDefaultMissLayerDurationMicros &&
              state.triggerSerial == 1,
          "a real combo-zero judgement triggers from BGA time, not visual time");

  tracker.onJudge(JudgeResult(Kpoor, 0), 0, clockAt(456, 1));
  state = tracker.snapshot();
  require(state.active && state.startedBgaMicros == 456 &&
              state.triggerSerial == 2,
          "repeated Kpoor at combo zero restarts the miss sequence");
}

void testZeroStartAndFrameBoundariesAreDeterministic() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(PGreat, 0), 0, clockAt(0));
  auto state = tracker.snapshot();
  require(state.active && state.startedBgaMicros == 0 &&
              state.triggerSerial == 1 && !state.isActiveAt(0) &&
              !state.isActiveAt(499'999) &&
              !state.frameIndexAt(0, 4).has_value(),
          "BGA timestamp zero is retained but preserves the pinned invisible "
          "misslayertime sentinel");

  tracker.onJudge(JudgeResult(Poor, 0), 0, clockAt(1));
  state = tracker.snapshot();
  require(state.frameIndexAt(1, 4) == 0 &&
              state.frameIndexAt(166'667, 4) == 0 &&
              state.frameIndexAt(166'668, 4) == 1 &&
              state.frameIndexAt(333'334, 4) == 1 &&
              state.frameIndexAt(333'335, 4) == 2 &&
              state.frameIndexAt(500'000, 4) == 2 &&
              !state.frameIndexAt(500'001, 4).has_value(),
          "four-frame miss indexing matches the end-exclusive pinned boundaries");
  require(!state.frameIndexAt(1, 0).has_value() &&
              state.frameIndexAt(1, 1) == 0,
          "empty and single-frame sequences have deterministic selection");
  require(!state.isActiveAt(0) && state.isActiveAt(1),
          "backward seek recomputes activity without mutating the trigger");

  tracker.reset();
  state = tracker.snapshot();
  require(!state.active && state.startedBgaMicros == 0 &&
              state.durationMicros == kDefaultMissLayerDurationMicros &&
              state.triggerSerial == 0,
          "reset restores the deterministic default miss state");
}

void testMissCompositionSuppressesBaseAndLayerWithoutFallback() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(Poor, 0), 0, clockAt(100));
  const auto activeMiss = tracker.snapshot();
  const auto noSequence = SelectGameplayBgaMissComposition(
      std::nullopt, activeMiss, 100);
  require(noSequence.composition == GameplayBgaComposition::BaseThenLayer &&
              !noSequence.resourceId.has_value(),
          "an absent channel-06 sequence leaves base and layer visible");

  const auto beforeStart = SelectGameplayBgaMissComposition(
      std::span<const int>{}, activeMiss, 99);
  const auto atEnd = SelectGameplayBgaMissComposition(
      std::span<const int>{}, activeMiss,
      100 + kDefaultMissLayerDurationMicros);
  require(beforeStart.composition == GameplayBgaComposition::BaseThenLayer &&
              atEnd.composition == GameplayBgaComposition::BaseThenLayer,
          "miss suppression is inactive before its start and at its exclusive end");

  const auto zeroFrames = SelectGameplayBgaMissComposition(
      std::span<const int>{}, activeMiss, 100);
  require(zeroFrames.composition == GameplayBgaComposition::MissOnly &&
              !zeroFrames.resourceId.has_value(),
          "an active empty sequence suppresses base and layer without a resource");

  const std::vector<int> oneFrame{41};
  const auto one = SelectGameplayBgaMissComposition(
      std::span<const int>(oneFrame), activeMiss, 300'000);
  require(one.composition == GameplayBgaComposition::MissOnly &&
              one.resourceId == 41,
          "a one-frame miss sequence selects its only authored resource");

  const std::vector<int> fourFrames{10, 20, 30, 40};
  const auto first = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 100);
  const auto second = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 166'767);
  const auto third = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 333'434);
  const auto fourth = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 499'999);
  require(first.resourceId == 10 && second.resourceId == 20 &&
              third.resourceId == 30 && fourth.resourceId == 30,
          "a four-frame miss sequence preserves the pinned end-exclusive mapping");

  const std::vector<int> blankFrame{kGameplayBgaAuthoredBlank};
  const auto blank = SelectGameplayBgaMissComposition(
      std::span<const int>(blankFrame), activeMiss, 100);
  require(blank.composition == GameplayBgaComposition::MissOnly &&
              !blank.resourceId.has_value(),
          "an authored blank suppresses base and layer without fallback");

  const auto afterSeekBack = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 100);
  require(afterSeekBack.resourceId == 10,
          "backward BGA time recomputes the selected miss frame without mutation");
}

} // namespace

int main() {
  testPreparedFrameRetainsExplicitSurfaceRoles();
  testNoneJudgeDoesNotTriggerMissState();
  testComboZeroUsesBgaClockAndRepeatedZeroRetriggers();
  testZeroStartAndFrameBoundariesAreDeterministic();
  testMissCompositionSuppressesBaseAndLayerWithoutFallback();
  return 0;
}
