#include "scene/play/PlayfieldPresentationCoordinator.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coordinator_test_allocation_fault {
thread_local bool failUntilBuiltInFallback = false;
}

void *operator new(std::size_t size) {
  if (coordinator_test_allocation_fault::failUntilBuiltInFallback) {
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

// The coordinator only transports this borrowed value. Focused tests keep
// rendering implementation dependencies out of this target.
struct RenderContext {};

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::size_t eventIndex(const std::vector<std::string> &events,
                       std::string_view value) {
  const auto found = std::find(events.begin(), events.end(), value);
  return found == events.end()
             ? events.size()
             : static_cast<std::size_t>(found - events.begin());
}

struct PresentationStats {
  int configureCalls = 0;
  int prepareCalls = 0;
  int renderCalls = 0;
  int lanePressedCalls = 0;
  int laneReleasedCalls = 0;
  int judgeCalls = 0;
  int beginCalls = 0;
  int updateCalls = 0;
  int endCalls = 0;
  int cancelCalls = 0;
  int resetCalls = 0;
  int refreshCalls = 0;
  int viewportCalls = 0;
  int viewportGeometryCalls = 0;
  std::uint64_t preparedFrameSerial = 0;
  std::uint64_t receivedBgaSequence = 0;
  std::uint64_t layoutRevision = 7;
  std::uint64_t hitRevision = 11;
  PresentationFrameOutcome prepareOutcome = PresentationFrameOutcome::Ready;
  PresentationFrameResult renderResult;
  skin::ViewportSettings lastViewport;
  skin::UiLogicalRect lastSafeUiBounds;
  PresentationTouchEvent lastTouch;
  const PlayfieldVisualState *preparedState = nullptr;
  const PlayfieldProjectionResult *preparedProjection = nullptr;
  std::vector<std::string> events;
  std::shared_ptr<std::vector<std::string>> sharedOrder;
  bool throwAllocationFailureOnRender = false;
  bool throwAllocationFailureOnPrepare = false;
  bool denyAllocationsAfterNoSubmission = false;
  bool clearAllocationFailureOnRender = false;
  int syntheticReplayGhostCalls = 0;
  std::uint64_t syntheticReplayGhostFrameSerial = 0;
  double syntheticReplayGhostScrollPosition = 0.0;
  double syntheticReplayGhostHispeed = 0.0;
  double syntheticReplayGhostVisibleLaneHeightRatio = 0.0;
  bool syntheticReplayGhostEnabled = false;
  std::size_t syntheticReplayGhostEventCount = 0;
  int syntheticStartLaneIndicatorCalls = 0;
  std::uint64_t syntheticStartLaneIndicatorFrameSerial = 0;
  double syntheticStartLaneIndicatorVisibleLaneHeightRatio = 0.0;
  std::vector<int> syntheticStartLaneIndicatorLanes;
};

void recordEvent(const std::shared_ptr<PresentationStats> &stats,
                 std::string value) {
  if (coordinator_test_allocation_fault::failUntilBuiltInFallback) {
    return;
  }
  stats->events.push_back(value);
  if (stats->sharedOrder) {
    stats->sharedOrder->push_back(std::move(value));
  }
}

class FakeBuiltIn final : public PlayfieldPresentation {
public:
  explicit FakeBuiltIn(std::shared_ptr<PresentationStats> stats)
      : stats_(std::move(stats)) {}

  void configure(const PlayfieldPresentationConfig &) override {
    ++stats_->configureCalls;
  }
  PresentationFrameOutcome
  prepareFrame(const PlayfieldVisualState &state,
               const PlayfieldProjectionResult &projection) override {
    ++stats_->prepareCalls;
    stats_->preparedFrameSerial = state.clock.serial;
    stats_->preparedState = &state;
    stats_->preparedProjection = &projection;
    recordEvent(stats_, "builtin.prepare");
    return stats_->prepareOutcome;
  }
  PresentationFrameResult render(RenderContext &) override {
    ++stats_->renderCalls;
    if (stats_->clearAllocationFailureOnRender) {
      coordinator_test_allocation_fault::failUntilBuiltInFallback = false;
    }
    recordEvent(stats_, "builtin.render");
    PresentationFrameResult result = stats_->renderResult;
    if (result.frameSerial == 0) {
      result.frameSerial = stats_->preparedFrameSerial;
    }
    result.submittedMode = PresentationMode::BuiltIn;
    result.bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn;
    return result;
  }
  gameplay::RealtimeTouchLayout touchLayout() const override {
    return {.revision = stats_->layoutRevision, .laneCount = 1, .keyMode = 7};
  }
  std::uint64_t touchLayoutRevision() const noexcept override {
    return stats_->layoutRevision;
  }
  std::uint64_t touchHitRegionsRevision() const noexcept override {
    return stats_->hitRevision;
  }
  std::vector<PresentationUiHitRegion> touchHitRegions() const override {
    return {{.hit = {.kind = PresentationUiControlKind::LaneCover,
                    .layoutRevision = stats_->layoutRevision}}};
  }
  PresentationUiHit hitTestUiControl(UiLogicalPoint) const override {
    return {.kind = PresentationUiControlKind::LaneCover,
            .layoutRevision = stats_->layoutRevision,
            .permitsLegacyBuiltInFallback = true};
  }
  PresentationTouchResult beginPresentationTouch(
      const PresentationTouchEvent &event) override {
    ++stats_->beginCalls;
    stats_->lastTouch = event;
    return {.consumed = true};
  }
  PresentationTouchResult updatePresentationTouch(
      const PresentationTouchEvent &event) override {
    ++stats_->updateCalls;
    stats_->lastTouch = event;
    return {.consumed = true};
  }
  PresentationTouchResult endPresentationTouch(
      const PresentationTouchEvent &event, bool) override {
    ++stats_->endCalls;
    stats_->lastTouch = event;
    return {.consumed = true};
  }
  void cancelPresentationTouches(long long) override {
    ++stats_->cancelCalls;
    recordEvent(stats_, "builtin.cancel");
  }
  void onLanePressed(int, JudgeResult, long long) override {
    ++stats_->lanePressedCalls;
  }
  void onLaneReleased(int, long long) override {
    ++stats_->laneReleasedCalls;
  }
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
               bool) override {
    ++stats_->judgeCalls;
  }
  void reset() override { ++stats_->resetCalls; }
  void refreshGeometry() override { ++stats_->refreshCalls; }
  PresentationMode activeMode() const noexcept override {
    return PresentationMode::BuiltIn;
  }
  std::optional<PresentationFailure> lastFailure() const override {
    return stats_->renderResult.failure;
  }

private:
  std::shared_ptr<PresentationStats> stats_;
};

class FakeSkin final : public CoordinatedPlaySkinSession {
public:
  FakeSkin(std::shared_ptr<PresentationStats> stats,
           skin::PlaySkinSessionIdentity identity)
      : stats_(std::move(stats)), identity_(std::move(identity)) {}
  ~FakeSkin() override { recordEvent(stats_, "skin.destroy"); }

  const skin::PlaySkinSessionIdentity &identity() const noexcept override {
    return identity_;
  }
  PresentationFrameOutcome
  prepareFrame(const PlayfieldVisualState &state,
               const PlayfieldProjectionResult &projection) override {
    ++stats_->prepareCalls;
    stats_->preparedFrameSerial = state.clock.serial;
    stats_->preparedState = &state;
    stats_->preparedProjection = &projection;
    recordEvent(stats_, "skin.prepare");
    if (stats_->throwAllocationFailureOnPrepare) {
      coordinator_test_allocation_fault::failUntilBuiltInFallback = true;
      throw std::bad_alloc();
    }
    return stats_->prepareOutcome;
  }
  PresentationFrameResult
  render(RenderContext &, const PreparedGameplayBgaFrame &bga,
         IGameplayBgaSubmitter &) override {
    ++stats_->renderCalls;
    stats_->receivedBgaSequence = bga.sequence;
    recordEvent(stats_, "skin.render");
    if (stats_->throwAllocationFailureOnRender) {
      coordinator_test_allocation_fault::failUntilBuiltInFallback = true;
      throw std::bad_alloc();
    }
    PresentationFrameResult result = stats_->renderResult;
    if (result.frameSerial == 0) {
      result.frameSerial = stats_->preparedFrameSerial;
    }
    if (stats_->denyAllocationsAfterNoSubmission) {
      coordinator_test_allocation_fault::failUntilBuiltInFallback = true;
    }
    return result;
  }
  void submitSyntheticReplayGhosts(
      RenderContext &, const skin::SyntheticReplayGhostFrameInput &input) override {
    ++stats_->syntheticReplayGhostCalls;
    stats_->syntheticReplayGhostFrameSerial = input.frameSerial;
    stats_->syntheticReplayGhostScrollPosition = input.currentScrollPosition;
    stats_->syntheticReplayGhostHispeed = input.hispeed;
    stats_->syntheticReplayGhostVisibleLaneHeightRatio =
        input.visibleLaneHeightRatio;
    stats_->syntheticReplayGhostEnabled = input.enabled;
    stats_->syntheticReplayGhostEventCount = input.events.size();
    recordEvent(stats_, "skin.replay_ghost");
  }
  void submitSyntheticStartLaneIndicators(
      RenderContext &,
      const skin::SyntheticStartLaneIndicatorFrameInput &input) override {
    ++stats_->syntheticStartLaneIndicatorCalls;
    stats_->syntheticStartLaneIndicatorFrameSerial = input.frameSerial;
    stats_->syntheticStartLaneIndicatorVisibleLaneHeightRatio =
        input.visibleLaneHeightRatio;
    stats_->syntheticStartLaneIndicatorLanes.assign(input.lanes.begin(),
                                                     input.lanes.end());
    recordEvent(stats_, "skin.start_lane_indicators");
  }
  void setViewport(skin::ViewportSettings viewport) override {
    ++stats_->viewportCalls;
    stats_->lastViewport = viewport;
    recordEvent(stats_, "skin.viewport");
  }
  void updateViewportGeometry(skin::UiLogicalRect safeUiBounds) override {
    ++stats_->viewportGeometryCalls;
    stats_->lastSafeUiBounds = safeUiBounds;
    recordEvent(stats_, "skin.geometry");
  }
  gameplay::RealtimeTouchLayout touchLayout() const override {
    return {.revision = stats_->layoutRevision, .laneCount = 2, .keyMode = 7};
  }
  std::uint64_t touchLayoutRevision() const noexcept override {
    return stats_->layoutRevision;
  }
  std::uint64_t touchHitRegionsRevision() const noexcept override {
    return stats_->hitRevision;
  }
  std::vector<PresentationUiHitRegion> touchHitRegions() const override {
    return {{.hit = {.kind = PresentationUiControlKind::Slider,
                    .layoutRevision = stats_->layoutRevision,
                    .sourceObject = 99}}};
  }
  PresentationUiHit hitTestUiControl(UiLogicalPoint) const override {
    return {.kind = PresentationUiControlKind::Slider,
            .layoutRevision = stats_->layoutRevision,
            .sourceObject = 99};
  }
  PresentationTouchResult beginPresentationTouch(
      const PresentationTouchEvent &event) override {
    ++stats_->beginCalls;
    stats_->lastTouch = event;
    return {.consumed = true, .excludeFromGameplay = true};
  }
  PresentationTouchResult updatePresentationTouch(
      const PresentationTouchEvent &event) override {
    ++stats_->updateCalls;
    stats_->lastTouch = event;
    return {.consumed = true, .excludeFromGameplay = true};
  }
  PresentationTouchResult endPresentationTouch(
      const PresentationTouchEvent &event, bool) override {
    ++stats_->endCalls;
    stats_->lastTouch = event;
    return {.consumed = true, .excludeFromGameplay = true};
  }
  void cancelPresentationTouches(long long) override {
    ++stats_->cancelCalls;
    recordEvent(stats_, "skin.cancel");
  }
  void onLanePressed(int, JudgeResult, long long) override {
    ++stats_->lanePressedCalls;
  }
  void onLaneReleased(int, long long) override {
    ++stats_->laneReleasedCalls;
  }
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
               bool) override {
    ++stats_->judgeCalls;
  }

private:
  std::shared_ptr<PresentationStats> stats_;
  skin::PlaySkinSessionIdentity identity_;
};

class FakeBga final : public IGameplayBgaSubmitter {
public:
  PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t frameSerial, std::int64_t bgaTimeMicros,
      const GameplayBgaMissState &) override {
    ++prepareCalls;
    lastFrameSerial = frameSerial;
    lastBgaTimeMicros = bgaTimeMicros;
    if (throwAllocationFailureOnPrepare) {
      coordinator_test_allocation_fault::failUntilBuiltInFallback = true;
      throw std::bad_alloc();
    }
    return {.sequence = 700 + frameSerial};
  }
  BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &,
            std::span<const BgaDrawTarget>) override {
    ++preflightCalls;
    return {.ready = true};
  }
  void commitPrepared(const PreparedGameplayBgaFrame &) noexcept override {
    ++commitCalls;
  }
  void submitPrepared(const PreparedGameplayBgaFrame &,
                      const BgaDrawTarget &) noexcept override {
    ++submitCalls;
  }
  void finalizePrepared(const PreparedGameplayBgaFrame &) noexcept override {
    ++finalizeCalls;
  }
  void submitFullscreen(const PreparedGameplayBgaFrame &) noexcept override {
    ++fullscreenCalls;
  }

  int prepareCalls = 0;
  int preflightCalls = 0;
  int commitCalls = 0;
  int submitCalls = 0;
  int finalizeCalls = 0;
  int fullscreenCalls = 0;
  std::uint64_t lastFrameSerial = 0;
  std::int64_t lastBgaTimeMicros = 0;
  bool throwAllocationFailureOnPrepare = false;
};

skin::PlaySkinSessionIdentity identity() {
  return {.sessionSerial = 73,
          .profileId = {.opaque = "profile-a"},
          .entry = {.package = {.directoryName = "pkg",
                                .collisionKey = "pkg"},
                    .packageRelativePath = "play7.luaskin",
                    .collisionKey = "play7.luaskin"},
          .revisionDigest = std::string(64, 'a'),
          .configurationDigest = std::string(64, 'b')};
}

PlayfieldVisualState frame(std::uint64_t serial) {
  PlayfieldVisualState state;
  state.clock.serial = serial;
  state.clock.bgaTimeMicros = static_cast<long long>(serial * 100);
  return state;
}

PresentationFailure skinFailure(std::uint64_t serial,
                                const skin::PlaySkinSessionIdentity &id,
                                std::string code) {
  return {.entry = id.entry,
          .revisionDigest = id.revisionDigest,
          .configurationDigest = id.configurationDigest,
          .diagnostic = {.code = std::move(code), .message = "failure"},
          .frameSerial = serial};
}

void testBuiltInDefaultWarmsAndReusesOneBgaFrame() {
  auto builtIn = std::make_shared<PresentationStats>();
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = {},
      .bga = bga,
  });
  coordinator.configure({.visibleTimeGreenNumber = 321});
  PlayfieldProjectionResult projection;
  auto state = frame(41);
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::Ready,
         "built-in prepare is ready");
  RenderContext render;
  const auto result = coordinator.render(render);
  expect(builtIn->prepareCalls == 1 && builtIn->renderCalls == 1,
         "built-in is warmed and rendered once");
  expect(builtIn->configureCalls == 1 && builtIn->preparedState == &state &&
             builtIn->preparedProjection == &projection,
         "configuration and exact captured frame are forwarded to built-in");
  expect(bga.prepareCalls == 1 && bga.lastFrameSerial == 41 &&
             bga.lastBgaTimeMicros == 4100,
         "BGA is prepared once from the captured frame");
  expect(result.frameSerial == 41 &&
             result.submittedMode == PresentationMode::BuiltIn &&
             result.bgaCompositeMode ==
                 GameplayBgaCompositeMode::FullscreenBuiltIn &&
             result.preparedBga && result.preparedBga->sequence == 741,
         "built-in result carries the exact prepared fullscreen BGA");
  expect(bga.finalizeCalls == 0 && bga.fullscreenCalls == 0,
         "coordinator never finalizes or submits fullscreen BGA");
}

void testSkinSuccessSubmitsNoBuiltInWork() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  skinStats->renderResult = {
      .outcome = PresentationFrameOutcome::Ready,
      .submittedMode = PresentationMode::Skin,
      .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin};
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, identity()),
      .bga = bga,
  });
  PlayfieldProjectionResult projection;
  auto state = frame(42);
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::Ready,
         "skin prepare is ready");
  RenderContext render;
  const auto result = coordinator.render(render);
  expect(builtIn->prepareCalls == 1 && builtIn->renderCalls == 0,
         "successful skin frame only warms the built-in");
  expect(builtIn->preparedState == &state && skinStats->preparedState == &state &&
             builtIn->preparedProjection == &projection &&
             skinStats->preparedProjection == &projection,
         "both candidates consume the same state and projection snapshot");
  expect(skinStats->prepareCalls == 1 && skinStats->renderCalls == 1 &&
             skinStats->receivedBgaSequence == 742,
         "skin receives the exact sole prepared BGA");
  expect(result.submittedMode == PresentationMode::Skin &&
             result.bgaCompositeMode == GameplayBgaCompositeMode::EmbeddedSkin,
         "successful skin owns the presentation and embedded BGA");
}

void testSelectedSkinReceivesOptionGatedReplayGhostFrame() {
  const auto makeCoordinator = [](std::shared_ptr<PresentationStats> builtIn,
                                  std::shared_ptr<PresentationStats> skinStats,
                                  FakeBga &bga) {
    return PlayfieldPresentationCoordinator({
        .builtIn = std::make_unique<FakeBuiltIn>(std::move(builtIn)),
        .skin = std::make_unique<FakeSkin>(std::move(skinStats), identity()),
        .bga = bga,
        .replayGhostEvents = {{.lane = 3,
                               .noteTimeMicros = 1'000,
                               .judgeTimeMicros = 1'100,
                               .judgeScrollPosition = 2.5,
                               .judgement = Great}},
    });
  };
  const auto configureAndRender = [&makeCoordinator](bool enabled) {
    auto builtIn = std::make_shared<PresentationStats>();
    auto skinStats = std::make_shared<PresentationStats>();
    skinStats->renderResult = {
        .outcome = PresentationFrameOutcome::Ready,
        .submittedMode = PresentationMode::Skin,
        .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin};
    FakeBga bga;
    auto coordinator = makeCoordinator(builtIn, skinStats, bga);
    coordinator.configure({.replayGhostRenderingEnabled = enabled});
    auto state = frame(enabled ? 61 : 62);
    state.clock.visualTimeMicros = 900;
    state.authority.laneCoverEnabled = true;
    state.authority.laneCoverPercent = 50;
    PlayfieldProjectionResult projection;
    projection.currentScrollPosition = 1.5;
    projection.builtInTraversal = BuiltInRendererTraversal{
        .judgeY = 2.0F,
        .upperBound = 10.0F,
        .noteVisibleUpperBound = 6.0F,
        .hispeed = 1.25F};
    (void)coordinator.prepareFrame(state, projection);
    RenderContext context;
    (void)coordinator.render(context);
    return skinStats;
  };

  const auto enabled = configureAndRender(true);
  expect(enabled->syntheticReplayGhostCalls == 1 &&
             enabled->syntheticReplayGhostFrameSerial == 61 &&
             enabled->syntheticReplayGhostEnabled &&
             enabled->syntheticReplayGhostEventCount == 1 &&
             enabled->syntheticReplayGhostScrollPosition == 1.5 &&
             enabled->syntheticReplayGhostHispeed == 1.25 &&
             enabled->syntheticReplayGhostVisibleLaneHeightRatio == 0.5,
         "selected skin receives the exact replay-ghost projection only when enabled");
  const auto disabled = configureAndRender(false);
  expect(disabled->syntheticReplayGhostCalls == 0,
         "disabled replay ghost option never reaches a selected skin");
}

void testSelectedSkinReceivesPreparationLaneIndicatorsWithoutFallback() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  skinStats->renderResult = {
      .outcome = PresentationFrameOutcome::Ready,
      .submittedMode = PresentationMode::Skin,
      .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin};
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, identity()),
      .bga = bga,
  });
  auto state = frame(63);
  state.authority.startLaneIndicators = {7, 2};
  state.authority.startLaneIndicatorsVisible = true;
  state.authority.laneCoverEnabled = true;
  state.authority.laneCoverPercent = 50;
  PlayfieldProjectionResult projection;
  // The renderer traversal is still from before this frame's Start+scratch
  // cover adjustment. The selected-skin cue must use captured authority,
  // rather than visibly lagging one cover step behind it.
  projection.builtInTraversal = BuiltInRendererTraversal{
      .judgeY = 2.0F,
      .upperBound = 10.0F,
      .noteVisibleUpperBound = 10.0F};
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::Ready,
         "selected-skin preparation indicator frame is ready");
  RenderContext context;
  const auto result = coordinator.render(context);
  expect(result.submittedMode == PresentationMode::Skin &&
             builtIn->renderCalls == 0,
         "preparation indicators preserve the selected skin as the frame owner");
  expect(skinStats->syntheticStartLaneIndicatorCalls == 1 &&
             skinStats->syntheticStartLaneIndicatorFrameSerial == 63 &&
             skinStats->syntheticStartLaneIndicatorVisibleLaneHeightRatio ==
                 0.5 &&
             skinStats->syntheticStartLaneIndicatorLanes ==
                 std::vector<int>({7, 2}) &&
             eventIndex(skinStats->events, "skin.render") <
                 eventIndex(skinStats->events, "skin.start_lane_indicators"),
         "selected skin receives its exact post-skin preparation lane overlay");

  auto hiddenState = frame(64);
  hiddenState.authority.startLaneIndicators = {7, 2};
  hiddenState.authority.startLaneIndicatorsVisible = false;
  (void)coordinator.prepareFrame(hiddenState, projection);
  (void)coordinator.render(context);
  expect(skinStats->syntheticStartLaneIndicatorCalls == 1,
         "hidden preparation indicators are not submitted to the selected skin");
}

void testCriticalSkinFailureCancelsBeforeOneWarmFallback() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  skinStats->prepareOutcome = PresentationFrameOutcome::CriticalFailure;
  const auto id = identity();
  skinStats->renderResult = {
      .outcome = PresentationFrameOutcome::CriticalFailure,
      .submittedMode = PresentationMode::BuiltIn,
      .bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn,
      .failure = skinFailure(43, id, "skin.test.critical")};
  // Share one order log so the fallback's ordering is directly observable.
  auto order = std::make_shared<std::vector<std::string>>();
  builtIn->sharedOrder = order;
  skinStats->sharedOrder = order;
  FakeBga bga;
  std::vector<PresentationFailure> recorded;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, id),
      .bga = bga,
      .recordFailure = [&](const PresentationFailure &failure) {
        recorded.push_back(failure);
      },
  });
  PlayfieldProjectionResult projection;
  auto state = frame(43);
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::CriticalFailure,
         "critical skin prepare is surfaced");
  RenderContext render;
  const auto result = coordinator.render(render);
  expect(skinStats->cancelCalls == 1,
         "captured skin touches are cancelled on failure");
  expect(builtIn->renderCalls == 1,
         "critical skin failure renders exactly one warmed built-in frame");
  expect(result.frameSerial == 43 &&
             result.submittedMode == PresentationMode::BuiltIn &&
             result.bgaCompositeMode ==
                 GameplayBgaCompositeMode::FullscreenBuiltIn &&
             result.preparedBga && result.preparedBga->sequence == 743,
         "fallback returns the same prepared BGA for global fullscreen");
  expect(coordinator.activeMode() == PresentationMode::BuiltIn,
         "failed skin is disabled for the rest of the chart");
  expect(recorded.size() == 1 &&
             recorded.front().diagnostic.code == "skin.test.critical",
         "critical failure is recorded once");
  expect(eventIndex(skinStats->events, "skin.cancel") <
             eventIndex(skinStats->events, "skin.destroy"),
         "skin cancellation precedes session destruction");
  expect(eventIndex(*order, "skin.cancel") <
             eventIndex(*order, "builtin.render"),
         "skin cancellation precedes the warmed built-in fallback render");
  auto retryStats = std::make_shared<PresentationStats>();
  coordinator.installSkinSession(
      std::make_unique<FakeSkin>(retryStats, identity()));
  expect(coordinator.activeMode() == PresentationMode::Skin,
         "a later validated session can explicitly retry on the next chart");
}

void testCriticalSelectedSkinPrepareFailureReturnsTransactionDiagnostic() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  skinStats->prepareOutcome = PresentationFrameOutcome::CriticalFailure;
  const auto id = identity();
  skinStats->renderResult = {
      .outcome = PresentationFrameOutcome::CriticalFailure,
      .submittedMode = PresentationMode::BuiltIn,
      .bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn,
      .failure = skinFailure(430, id, "skin.test.transaction_authority")};
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, id),
      .bga = bga,
      .allowBuiltInFallback = false,
  });
  PlayfieldProjectionResult projection;
  auto state = frame(430);
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::CriticalFailure,
         "selected skin prepare failure is surfaced");
  RenderContext render;
  const auto result = coordinator.render(render);
  expect(skinStats->renderCalls == 1 && builtIn->renderCalls == 0,
         "selected path asks the failed skin transaction for its diagnostic");
  expect(result.failure.has_value() &&
             result.failure->diagnostic.code ==
                 "skin.test.transaction_authority",
         "selected path returns the authoritative transaction diagnostic");
}

void testPostDrawRecoverableFailureNeverCreatesHybrid() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  const auto id = identity();
  skinStats->renderResult = {
      .outcome = PresentationFrameOutcome::RecoverableFailure,
      .submittedMode = PresentationMode::Skin,
      .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin,
      .failure = skinFailure(44, id, "skin.test.queue_full")};
  FakeBga bga;
  std::vector<PresentationFailure> recorded;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, id),
      .bga = bga,
      .recordFailure = [&](const PresentationFailure &failure) {
        recorded.push_back(failure);
      },
  });
  PlayfieldProjectionResult projection;
  auto state = frame(44);
  (void)coordinator.prepareFrame(state, projection);
  RenderContext render;
  const auto result = coordinator.render(render);
  expect(result.outcome == PresentationFrameOutcome::RecoverableFailure &&
             result.submittedMode == PresentationMode::Skin &&
             result.bgaCompositeMode == GameplayBgaCompositeMode::EmbeddedSkin,
         "post-draw recoverable failure retains the submitted skin frame");
  expect(builtIn->renderCalls == 0,
         "post-draw failure never submits a built-in hybrid");
  expect(coordinator.activeMode() == PresentationMode::Skin,
         "recoverable post-draw failure keeps the session live");
  expect(recorded.size() == 1,
         "post-draw recoverable failure is recorded once");
}

void testEventFanoutAndTouchRoutingHaveOneAuthorityTarget() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, identity()),
      .bga = bga,
  });

  coordinator.onLanePressed(2, JudgeResult(Great, -12), 100);
  coordinator.onLaneReleased(2, 200);
  coordinator.onJudge(JudgeResult(PGreat, 3), 5, 10,
                      {.songTimeMicros = 300,
                       .visualTimeMicros = 290,
                       .bgaTimeMicros = 280},
                      true);
  expect(builtIn->lanePressedCalls == 1 && skinStats->lanePressedCalls == 1 &&
             builtIn->laneReleasedCalls == 1 &&
             skinStats->laneReleasedCalls == 1 && builtIn->judgeCalls == 1 &&
             skinStats->judgeCalls == 1,
         "events fan out exactly once to warmed built-in and live skin");

  const auto hit = coordinator.hitTestUiControl({1.0F, 2.0F});
  const PresentationTouchEvent down{
      .pointerId = 9,
      .uiPoint = {1.0F, 2.0F},
      .eventMicros = 400,
      .hit = hit};
  PresentationTouchEvent stale = down;
  ++stale.hit.layoutRevision;
  PresentationTouchEvent forged = down;
  ++forged.hit.sourceObject;
  expect(!coordinator.beginPresentationTouch(stale).consumed &&
             !coordinator.beginPresentationTouch(forged).consumed &&
             skinStats->beginCalls == 0 && builtIn->beginCalls == 0,
         "stale or forged Down fails closed without reaching either target");
  const auto touch = coordinator.beginPresentationTouch(down);
  expect(touch.consumed && touch.excludeFromGameplay &&
             skinStats->beginCalls == 1 && builtIn->beginCalls == 0,
         "touch routes only to the active skin target");
  expect(skinStats->lastTouch.hit.layoutRevision == skinStats->layoutRevision,
         "coordinator translates its public layout identity for the session");

  const auto skinPublicRevision = coordinator.touchLayoutRevision();
  coordinator.clearSkinSession();
  expect(skinStats->cancelCalls == 1,
         "clearing a skin cancels its captured pointers first");
  expect(coordinator.touchLayoutRevision() != skinPublicRevision,
         "presentation replacement advances the public touch topology");
  PresentationTouchEvent oldMove = down;
  oldMove.eventMicros = 450;
  PresentationTouchEvent oldUp = down;
  oldUp.eventMicros = 451;
  expect(!coordinator.updatePresentationTouch(oldMove).consumed &&
             !coordinator.endPresentationTouch(oldUp, false).consumed &&
             builtIn->updateCalls == 0 && builtIn->endCalls == 0,
         "skin capture cannot migrate to built-in after target clear");
  const auto builtInHit = coordinator.hitTestUiControl({1.0F, 2.0F});
  (void)coordinator.beginPresentationTouch(
      {.pointerId = 10,
       .uiPoint = {1.0F, 2.0F},
       .eventMicros = 500,
       .hit = builtInHit});
  expect(builtIn->beginCalls == 1,
         "touch routes only to built-in after skin clear");
}

void testSkinReplacementDoesNotTransferPointerOwnership() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto oldSkin = std::make_shared<PresentationStats>();
  auto newSkin = std::make_shared<PresentationStats>();
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(oldSkin, identity()),
      .bga = bga,
  });
  const PresentationTouchEvent down{
      .pointerId = 88,
      .uiPoint = {4.0F, 5.0F},
      .eventMicros = 600,
      .hit = coordinator.hitTestUiControl({4.0F, 5.0F})};
  expect(coordinator.beginPresentationTouch(down).consumed,
         "old skin owns consumed Down");
  coordinator.installSkinSession(
      std::make_unique<FakeSkin>(newSkin, identity()));
  PresentationTouchEvent later = down;
  later.eventMicros = 610;
  expect(!coordinator.updatePresentationTouch(later).consumed &&
             !coordinator.endPresentationTouch(later, false).consumed &&
             newSkin->updateCalls == 0 && newSkin->endCalls == 0,
         "old pointer ownership cannot cross skin-to-skin replacement");
  expect(oldSkin->cancelCalls == 1,
         "old skin is cancelled before replacement destruction");

  const PresentationTouchEvent replacementDown{
      .pointerId = 89,
      .uiPoint = {4.0F, 5.0F},
      .eventMicros = 620,
      .hit = coordinator.hitTestUiControl({4.0F, 5.0F})};
  expect(coordinator.beginPresentationTouch(replacementDown).consumed &&
             newSkin->beginCalls == 1,
         "replacement accepts only a fresh Down with its generation");
}

void testPointerCaptureTableIsBoundedAndReleasedByCancel() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, identity()),
      .bga = bga,
  });
  const auto hit = coordinator.hitTestUiControl({3.0F, 4.0F});
  for (std::size_t index = 0;
       index < gameplay::kRealtimeTouchFingerCapacity; ++index) {
    expect(coordinator
               .beginPresentationTouch(
                   {.pointerId = static_cast<long long>(1'000 + index),
                    .uiPoint = {3.0F, 4.0F},
                    .eventMicros = static_cast<long long>(700 + index),
                    .hit = hit})
               .consumed,
           "each bounded capture slot accepts one consumed Down");
  }
  expect(!coordinator
              .beginPresentationTouch(
                  {.pointerId = 2'000,
                   .uiPoint = {3.0F, 4.0F},
                   .eventMicros = 800,
                   .hit = hit})
              .consumed &&
             skinStats->beginCalls ==
                 static_cast<int>(gameplay::kRealtimeTouchFingerCapacity),
         "capture 33 fails closed without reaching the skin");
  coordinator.cancelPresentationTouches(900);
  const auto refreshedHit = coordinator.hitTestUiControl({3.0F, 4.0F});
  expect(coordinator
             .beginPresentationTouch({.pointerId = 2'001,
                                      .uiPoint = {3.0F, 4.0F},
                                      .eventMicros = 901,
                                      .hit = refreshedHit})
             .consumed,
         "cancel clears the bounded capture table for fresh Down");
}

void testAllocationDeniedAcrossEveryPrecommitFailureStillFallsBack() {
  enum class Scenario { SkinPrepare, BgaPrepare, SkinRender, NoSubmission };
  const auto expectedIdentity = identity();
  const std::array scenarios{
      std::pair{Scenario::SkinPrepare, "skin.presentation.prepare_failed"},
      std::pair{Scenario::BgaPrepare, "skin.presentation.bga_prepare_failed"},
      std::pair{Scenario::SkinRender, "skin.presentation.render_failed"},
      std::pair{Scenario::NoSubmission,
                "skin.presentation.frame_not_submitted"}};

  std::uint64_t serial = 46;
  for (const auto &[scenario, expectedCode] : scenarios) {
    auto builtIn = std::make_shared<PresentationStats>();
    auto skinStats = std::make_shared<PresentationStats>();
    builtIn->clearAllocationFailureOnRender = true;
    skinStats->throwAllocationFailureOnPrepare =
        scenario == Scenario::SkinPrepare;
    skinStats->throwAllocationFailureOnRender =
        scenario == Scenario::SkinRender;
    skinStats->denyAllocationsAfterNoSubmission =
        scenario == Scenario::NoSubmission;
    FakeBga bga;
    bga.throwAllocationFailureOnPrepare = scenario == Scenario::BgaPrepare;
    std::vector<PresentationFailure> recorded;
    PlayfieldPresentationCoordinator coordinator({
        .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
        .skin = std::make_unique<FakeSkin>(skinStats, expectedIdentity),
        .bga = bga,
        .recordFailure = [&](const PresentationFailure &failure) {
          recorded.push_back(failure);
        },
    });
    PlayfieldProjectionResult projection;
    auto state = frame(serial++);
    const auto prepared = coordinator.prepareFrame(state, projection);
    expect(prepared == (scenario == Scenario::SkinPrepare ||
                                scenario == Scenario::BgaPrepare
                            ? PresentationFrameOutcome::CriticalFailure
                            : PresentationFrameOutcome::Ready),
           "allocation scenario exposes its exact prepare outcome");
    RenderContext render;
    const auto result = coordinator.render(render);
    expect(!coordinator_test_allocation_fault::failUntilBuiltInFallback,
           "warmed built-in was reached while allocations remained denied");
    expect(skinStats->cancelCalls == 1 && builtIn->renderCalls == 1 &&
               result.submittedMode == PresentationMode::BuiltIn &&
               result.bgaCompositeMode ==
                   GameplayBgaCompositeMode::FullscreenBuiltIn,
           "precommit failure cancels/destroys skin then renders one fallback");
    expect(result.failure && result.failure->diagnostic.code == expectedCode &&
               result.failure->entry == expectedIdentity.entry &&
               result.failure->revisionDigest ==
                   expectedIdentity.revisionDigest &&
               result.failure->configurationDigest ==
                   expectedIdentity.configurationDigest,
           "prebuilt fallback retains exact code and activation identity");
    expect(recorded.size() == 1,
           "failure reporting happens after allocation-safe fallback");
  }
}

void testResetLayoutAppliesFitBeforeOneImmutablePersistenceRequest() {
  for (const auto disposition : {
           GameplayViewportPersistenceDisposition::Queued,
           GameplayViewportPersistenceDisposition::Deferred,
           GameplayViewportPersistenceDisposition::Rejected}) {
    auto builtIn = std::make_shared<PresentationStats>();
    auto skinStats = std::make_shared<PresentationStats>();
    FakeBga bga;
    const auto expectedIdentity = identity();
    int persistenceCalls = 0;
    skin::PlaySkinSessionIdentity persistedIdentity;
    skin::ViewportSettings persistedViewport;
    PlayfieldPresentationCoordinator coordinator({
        .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
        .skin = std::make_unique<FakeSkin>(skinStats, expectedIdentity),
        .bga = bga,
        .persistViewport = [&](const skin::PlaySkinSessionIdentity &session,
                               skin::ViewportSettings viewport) {
          ++persistenceCalls;
          recordEvent(skinStats, "persist");
          persistedIdentity = session;
          persistedViewport = viewport;
          GameplayViewportPersistenceResult result{
              .disposition = disposition};
          if (disposition !=
              GameplayViewportPersistenceDisposition::Queued) {
            result.diagnostic = skin::SkinDiagnostic{
                .code = disposition ==
                                GameplayViewportPersistenceDisposition::Deferred
                            ? "skin.test.deferred"
                            : "skin.test.rejected",
                .message = "status"};
          }
          return result;
        },
    });
    expect(coordinator.resetLayoutToFit(),
           "live session applies Fit even when persistence is not queued");
    expect(skinStats->viewportCalls == 1 &&
               skinStats->lastViewport.mode == skin::ViewportMode::Fit,
           "Fit is applied immediately to the current session");
    expect(persistenceCalls == 1 && persistedViewport.mode ==
                                        skin::ViewportMode::Fit,
           "Fit persistence is requested exactly once");
    expect(persistedIdentity.sessionSerial == expectedIdentity.sessionSerial &&
               persistedIdentity.profileId == expectedIdentity.profileId &&
               persistedIdentity.entry == expectedIdentity.entry &&
               persistedIdentity.revisionDigest ==
                   expectedIdentity.revisionDigest &&
               persistedIdentity.configurationDigest ==
                   expectedIdentity.configurationDigest,
           "persistence receives the immutable five-field session identity");
    expect(eventIndex(skinStats->events, "skin.viewport") <
               eventIndex(skinStats->events, "persist"),
           "current Fit geometry changes before durable persistence");
    expect(coordinator.activeMode() == PresentationMode::Skin,
           "persistence disposition never reverts or hides current Fit");
    expect(coordinator.lastFailure().has_value() ==
               (disposition !=
                GameplayViewportPersistenceDisposition::Queued),
           "Queued succeeds while Deferred/Rejected report status");
  }
}

void testDuplicatePrepareCannotAdvanceBgaTwice() {
  auto builtIn = std::make_shared<PresentationStats>();
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = {},
      .bga = bga,
  });
  PlayfieldProjectionResult projection;
  auto state = frame(45);
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::Ready,
         "first frame prepare succeeds");
  expect(coordinator.prepareFrame(state, projection) ==
             PresentationFrameOutcome::CriticalFailure,
         "second prepare is rejected while original remains pending");
  expect(bga.prepareCalls == 1 && builtIn->prepareCalls == 1,
         "duplicate prepare cannot update BGA or presentations twice");
  RenderContext render;
  const auto result = coordinator.render(render);
  expect(result.preparedBga && result.preparedBga->sequence == 745,
         "original pending frame remains renderable after duplicate rejection");
}

void testSafeAreaReplacementCancelsBeforeRefreshingBothPresentations() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto skinStats = std::make_shared<PresentationStats>();
  FakeBga bga;
  PlayfieldPresentationCoordinator coordinator({
      .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
      .skin = std::make_unique<FakeSkin>(skinStats, identity()),
      .bga = bga,
  });
  const auto hit = coordinator.hitTestUiControl({.x = 10.0F, .y = 20.0F});
  expect(coordinator.beginPresentationTouch(
             {.pointerId = 71,
              .uiPoint = {.x = 10.0F, .y = 20.0F},
              .eventMicros = 3,
              .hit = hit})
             .consumed,
         "safe-area fixture owns one skin pointer before rotation");

  const skin::UiLogicalRect safe{
      .x = 30.0, .y = 20.0, .width = 1220.0, .height = 680.0};
  coordinator.updateSkinViewportGeometry(safe);
  expect(skinStats->cancelCalls == 1 &&
             eventIndex(skinStats->events, "skin.cancel") <
                 eventIndex(skinStats->events, "skin.geometry"),
         "rotation cancels captured skin ownership before geometry replacement");
  expect(builtIn->refreshCalls == 1 && skinStats->viewportGeometryCalls == 1 &&
             skinStats->lastSafeUiBounds.x == safe.x &&
             skinStats->lastSafeUiBounds.y == safe.y &&
             skinStats->lastSafeUiBounds.width == safe.width &&
             skinStats->lastSafeUiBounds.height == safe.height,
         "one safe-area replacement refreshes the warmed built-in and live skin");
  expect(coordinator.updatePresentationTouch(
             {.pointerId = 71,
              .uiPoint = {.x = 12.0F, .y = 22.0F},
              .eventMicros = 4,
              .hit = hit}) == PresentationTouchResult{},
         "pre-rotation pointer ownership cannot cross into new geometry");
}

void testResetAndDestructorCancelBeforeDestroyingSkin() {
  auto builtIn = std::make_shared<PresentationStats>();
  auto replacedSkin = std::make_shared<PresentationStats>();
  auto resetSkin = std::make_shared<PresentationStats>();
  auto destructorSkin = std::make_shared<PresentationStats>();
  FakeBga bga;
  {
    PlayfieldPresentationCoordinator coordinator({
        .builtIn = std::make_unique<FakeBuiltIn>(builtIn),
        .skin = std::make_unique<FakeSkin>(replacedSkin, identity()),
        .bga = bga,
    });
    coordinator.installSkinSession(
        std::make_unique<FakeSkin>(resetSkin, identity()));
    expect(replacedSkin->cancelCalls == 1 &&
               eventIndex(replacedSkin->events, "skin.cancel") <
                   eventIndex(replacedSkin->events, "skin.destroy"),
           "replacement cancels old skin captures before destruction");
    coordinator.reset();
    expect(resetSkin->cancelCalls == 1 && builtIn->resetCalls == 1,
           "reset cancels skin and resets warmed built-in");
    coordinator.installSkinSession(
        std::make_unique<FakeSkin>(destructorSkin, identity()));
  }
  expect(destructorSkin->cancelCalls == 1,
         "coordinator destructor cancels live skin captures");
  expect(eventIndex(destructorSkin->events, "skin.cancel") <
             eventIndex(destructorSkin->events, "skin.destroy"),
         "destructor cancellation precedes skin destruction");
}

} // namespace

int main() {
  testBuiltInDefaultWarmsAndReusesOneBgaFrame();
  testSkinSuccessSubmitsNoBuiltInWork();
  testSelectedSkinReceivesOptionGatedReplayGhostFrame();
  testSelectedSkinReceivesPreparationLaneIndicatorsWithoutFallback();
  testCriticalSkinFailureCancelsBeforeOneWarmFallback();
  testCriticalSelectedSkinPrepareFailureReturnsTransactionDiagnostic();
  testPostDrawRecoverableFailureNeverCreatesHybrid();
  testEventFanoutAndTouchRoutingHaveOneAuthorityTarget();
  testSkinReplacementDoesNotTransferPointerOwnership();
  testPointerCaptureTableIsBoundedAndReleasedByCancel();
  testAllocationDeniedAcrossEveryPrecommitFailureStillFallsBack();
  testResetLayoutAppliesFitBeforeOneImmutablePersistenceRequest();
  testDuplicatePrepareCannotAdvanceBgaTwice();
  testSafeAreaReplacementCancelsBeforeRefreshingBothPresentations();
  testResetAndDestructorCancelBeforeDestroyingSkin();
  if (failures != 0) {
    std::cerr << failures << " coordinator test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "playfield presentation coordinator tests passed\n";
  return EXIT_SUCCESS;
}
