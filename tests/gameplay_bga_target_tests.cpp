#include "audio/GameplayBgaFrame.h"
#include "audio/GameplayBgaMissStateTracker.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
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

bool samePoint(const GameplayBgaPoint &left,
               const GameplayBgaPoint &right) {
  return left.x == right.x && left.y == right.y;
}

bool sameClip(const std::optional<GameplayBgaClipRect> &left,
              const std::optional<GameplayBgaClipRect> &right) {
  return left.has_value() == right.has_value() &&
         (!left.has_value() ||
          (left->x == right->x && left->y == right->y &&
           left->width == right->width && left->height == right->height));
}

bool sameTarget(const BgaDrawTarget &left, const BgaDrawTarget &right) {
  return left.role == right.role && left.viewId == right.viewId &&
         samePoint(left.destination[0], right.destination[0]) &&
         samePoint(left.destination[1], right.destination[1]) &&
         samePoint(left.destination[2], right.destination[2]) &&
         samePoint(left.destination[3], right.destination[3]) &&
         left.stretch == right.stretch && left.tint == right.tint &&
         left.blend == right.blend && sameClip(left.clip, right.clip) &&
         left.authoredOrdinal == right.authoredOrdinal;
}

bool sameSurface(const std::optional<PreparedGameplayBgaSurface> &left,
                 const std::optional<PreparedGameplayBgaSurface> &right) {
  return left.has_value() == right.has_value() &&
         (!left.has_value() ||
          (left->role == right->role && left->mediaKind == right->mediaKind &&
           left->surfaceToken == right->surfaceToken &&
           left->sourceWidth == right->sourceWidth &&
           left->sourceHeight == right->sourceHeight));
}

bool sameFrame(const PreparedGameplayBgaFrame &left,
               const PreparedGameplayBgaFrame &right) {
  return left.sequence == right.sequence && left.composition == right.composition &&
         sameSurface(left.base, right.base) && sameSurface(left.layer, right.layer) &&
         sameSurface(left.miss, right.miss);
}

class FakeGameplayBgaSubmitter final : public IGameplayBgaSubmitter {
public:
  explicit FakeGameplayBgaSubmitter(bool *destroyed = nullptr)
      : destroyed_(destroyed) {}

  ~FakeGameplayBgaSubmitter() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  PreparedGameplayBgaFrame
  prepareVisualFrameAt(std::uint64_t frameSerial, std::int64_t bgaTimeMicros,
                       const GameplayBgaMissState &missState) override {
    preparedFrameSerial = frameSerial;
    preparedBgaTimeMicros = bgaTimeMicros;
    preparedMissState = missState;
    return prepared;
  }

  BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) override {
    preflightFrame = frame;
    preflightTargets.assign(targets.begin(), targets.end());
    return {.ready = true};
  }

  void submitPrepared(const PreparedGameplayBgaFrame &frame,
                      const BgaDrawTarget &target) noexcept override {
    submittedPreparedFrame = frame;
    submittedTarget = target;
  }

  void submitFullscreen(const PreparedGameplayBgaFrame &frame) noexcept override {
    submittedFullscreenFrame = frame;
  }

  PreparedGameplayBgaFrame prepared;
  std::uint64_t preparedFrameSerial = 0;
  std::int64_t preparedBgaTimeMicros = 0;
  GameplayBgaMissState preparedMissState;
  PreparedGameplayBgaFrame preflightFrame;
  std::vector<BgaDrawTarget> preflightTargets;
  PreparedGameplayBgaFrame submittedPreparedFrame;
  std::optional<BgaDrawTarget> submittedTarget;
  PreparedGameplayBgaFrame submittedFullscreenFrame;

private:
  bool *destroyed_ = nullptr;
};

void testBgaDrawTargetRoleIsIndependentOfViewId() {
  const BgaDrawTarget target{
      .role = GameplayBgaRole::Miss,
      .viewId = 17,
      .destination = {{{.x = 1.0F, .y = 2.0F},
                       {.x = 3.0F, .y = 4.0F},
                       {.x = 5.0F, .y = 6.0F},
                       {.x = 7.0F, .y = 8.0F}}},
      .stretch = skin::SkinStretchMode::KeepAspectRatioNoExpanding,
      .tint = {0.1F, 0.2F, 0.3F, 0.4F},
      .blend = skin::SkinBlendMode::Additive,
      .clip = GameplayBgaClipRect{.x = 10.0, .y = 20.0, .width = 30.0,
                                   .height = 40.0},
      .authoredOrdinal = 9,
  };

  require(target.role == GameplayBgaRole::Miss && target.viewId == 17 &&
              target.destination[3].x == 7.0F &&
              target.stretch == skin::SkinStretchMode::KeepAspectRatioNoExpanding &&
              target.tint[3] == 0.4F &&
              target.blend == skin::SkinBlendMode::Additive &&
              target.clip.has_value() && target.authoredOrdinal == 9,
          "BGA draw targets carry an explicit role independently of view ID");
}

void testBgaSubmitterPreflightsExactMultipleTargets() {
  FakeGameplayBgaSubmitter submitter;
  const PreparedGameplayBgaFrame frame{
      .sequence = 77,
      .composition = GameplayBgaComposition::BaseThenLayer,
  };
  const std::array targets{
      BgaDrawTarget{.role = GameplayBgaRole::Base,
                    .viewId = 3,
                    .destination = {{{.x = 1.0F, .y = 1.0F},
                                     {.x = 2.0F, .y = 1.0F},
                                     {.x = 2.0F, .y = 2.0F},
                                     {.x = 1.0F, .y = 2.0F}}},
                    .authoredOrdinal = 4},
      BgaDrawTarget{.role = GameplayBgaRole::Layer,
                    .viewId = 3,
                    .destination = {{{.x = 10.0F, .y = 20.0F},
                                     {.x = 30.0F, .y = 20.0F},
                                     {.x = 30.0F, .y = 40.0F},
                                     {.x = 10.0F, .y = 40.0F}}},
                    .stretch = skin::SkinStretchMode::NoResize,
                    .tint = {0.5F, 0.6F, 0.7F, 0.8F},
                    .blend = skin::SkinBlendMode::Multiply,
                    .clip = GameplayBgaClipRect{.x = 11.0, .y = 12.0,
                                                 .width = 13.0, .height = 14.0},
                    .authoredOrdinal = 5},
  };

  const auto result = submitter.preflight(frame, targets);
  require(result.ready && !result.failure.has_value() &&
              sameFrame(submitter.preflightFrame, frame) &&
              submitter.preflightTargets.size() == targets.size() &&
              sameTarget(submitter.preflightTargets[0], targets[0]) &&
              sameTarget(submitter.preflightTargets[1], targets[1]),
          "BGA preflight receives each exact authored draw target");
}

void testBgaSubmitterPreparedFrameIsAnImmutableValue() {
  static_assert(std::is_same_v<
                decltype(std::declval<IGameplayBgaSubmitter &>()
                             .prepareVisualFrameAt(
                                 0, 0,
                                 std::declval<const GameplayBgaMissState &>())),
                PreparedGameplayBgaFrame>);

  FakeGameplayBgaSubmitter submitter;
  submitter.prepared = {
      .sequence = 91,
      .composition = GameplayBgaComposition::MissOnly,
      .miss = PreparedGameplayBgaSurface{.role = GameplayBgaRole::Miss,
                                         .surfaceToken = 123},
  };
  const GameplayBgaMissState state{.active = true, .startedBgaMicros = 100};
  const auto prepared = submitter.prepareVisualFrameAt(55, 200, state);
  const auto original = prepared;
  const BgaDrawTarget target{.role = GameplayBgaRole::Miss, .viewId = 8};

  submitter.submitPrepared(prepared, target);
  submitter.submitFullscreen(prepared);
  require(submitter.preparedFrameSerial == 55 &&
              submitter.preparedBgaTimeMicros == 200 &&
              submitter.preparedMissState.active &&
              sameFrame(prepared, original) &&
              sameFrame(submitter.submittedPreparedFrame, original) &&
              sameFrame(submitter.submittedFullscreenFrame, original),
          "prepared BGA frames are returned by value and remain unchanged by submission");
}

void testBgaSubmitterHasVirtualDestruction() {
  bool destroyed = false;
  std::unique_ptr<IGameplayBgaSubmitter> submitter =
      std::make_unique<FakeGameplayBgaSubmitter>(&destroyed);
  submitter.reset();
  require(destroyed, "BGA submitters destroy derived implementations virtually");
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
  testBgaDrawTargetRoleIsIndependentOfViewId();
  testBgaSubmitterPreflightsExactMultipleTargets();
  testBgaSubmitterPreparedFrameIsAnImmutableValue();
  testBgaSubmitterHasVirtualDestruction();
  testPreparedFrameRetainsExplicitSurfaceRoles();
  testNoneJudgeDoesNotTriggerMissState();
  testComboZeroUsesBgaClockAndRepeatedZeroRetriggers();
  testZeroStartAndFrameBoundariesAreDeterministic();
  testMissCompositionSuppressesBaseAndLayerWithoutFallback();
  return 0;
}
