#include "scene/play/RealtimeTouchInputRouter.h"
#include "scene/play/RealtimeTouchPresentation.h"
#include "scene/play/PlayfieldPresentation.h"
#include "scene/play/PlayfieldProjection.h"
#include "scene/play/VirtualControllerLayout.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>
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

class ConcreteTouchPresentation final : public PlayfieldPresentation {
public:
  void configure(const PlayfieldPresentationConfig &) override {}
  PresentationFrameOutcome
  prepareFrame(const PlayfieldVisualState &state,
               const PlayfieldProjectionResult &projection) override {
    preparedFrameSerial = state.clock.serial;
    if (state.clock.serial == 0 || projection.frameSerial != state.clock.serial) {
      failure = PresentationFailure{
          .diagnostic = skin::SkinDiagnostic{
              .code = "presentation.frame_mismatch",
              .message = "The captured state and projection do not match."},
          .frameSerial = state.clock.serial};
      return PresentationFrameOutcome::CriticalFailure;
    }
    failure.reset();
    return PresentationFrameOutcome::Ready;
  }
  PresentationFrameResult render(RenderContext &) override {
    return {.frameSerial = preparedFrameSerial,
            .outcome = failure.has_value()
                           ? PresentationFrameOutcome::CriticalFailure
                           : PresentationFrameOutcome::Ready,
            .submittedMode = PresentationMode::BuiltIn,
            .bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn,
            .failure = failure};
  }
  gameplay::RealtimeTouchLayout touchLayout() const override {
    gameplay::RealtimeTouchLayout layout;
    layout.revision = touchRevision;
    return layout;
  }
  std::uint64_t touchLayoutRevision() const noexcept override {
    return touchRevision;
  }
  std::uint64_t touchHitRegionsRevision() const noexcept override {
    return hitRevision;
  }
  std::vector<PresentationUiHitRegion> touchHitRegions() const override {
    return hitRegions;
  }
  PresentationUiHit hitTestUiControl(UiLogicalPoint) const override {
    return {};
  }
  PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &) override {
    return {};
  }
  PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &) override {
    return {};
  }
  PresentationTouchResult
  endPresentationTouch(const PresentationTouchEvent &, bool) override {
    return {};
  }
  void cancelPresentationTouches(long long) override {}
  void reset() override {}
  void refreshGeometry() override {}
  PresentationMode activeMode() const noexcept override {
    return PresentationMode::BuiltIn;
  }
  std::optional<PresentationFailure> lastFailure() const override {
    return failure;
  }
  void onLanePressed(int, JudgeResult, long long) override {}
  void onLaneReleased(int, long long) override {}
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
               bool) override {}

  std::uint64_t touchRevision = 1;
  std::uint64_t hitRevision = 1;
  std::uint64_t preparedFrameSerial = 0;
  std::optional<PresentationFailure> failure;
  std::vector<PresentationUiHitRegion> hitRegions;
};

static_assert(!std::is_abstract_v<ConcreteTouchPresentation>,
              "the complete presentation touch surface is concrete");

void testPresentationFrameContractCarriesExactSerialOnFailure() {
  const PresentationFailure failure{
      .entry = {.package = {.directoryName = "default",
                            .collisionKey = "default"},
                .packageRelativePath = "play/play7.luaskin",
                .collisionKey = "play/play7.luaskin"},
      .revisionDigest = "revision",
      .configurationDigest = "configuration",
      .diagnostic = {.code = "presentation.failure",
                     .message = "fixture failure"},
      .frameSerial = 41};
  const PresentationFrameResult result{
      .frameSerial = 41,
      .outcome = PresentationFrameOutcome::CriticalFailure,
      .submittedMode = PresentationMode::BuiltIn,
      .bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn,
      .failure = failure};

  require(result.frameSerial == 41 && result.failure.has_value() &&
              result.failure->frameSerial == result.frameSerial &&
              result.submittedMode == PresentationMode::BuiltIn &&
              result.bgaCompositeMode ==
                  GameplayBgaCompositeMode::FullscreenBuiltIn &&
              !result.preparedBga.has_value(),
          "presentation failure results preserve the exact frame serial and built-in fallback contract");
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
  const auto logicalCenter =
      gameplay::realtimeTouchUiLogicalPoint(0.5F, 0.5F);

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
  requireNear(logicalCenter.x, 960.0F,
              "UI hit testing receives logical x after safe-area removal");
  requireNear(logicalCenter.y, 540.0F,
              "UI hit testing receives logical y after safe-area removal");
}

void testLegacyBuiltInTouchFallbackRequiresExplicitOwnership() {
  require(gameplay::realtimeTouchAllowsLegacyBuiltInControl({}),
          "ordinary unowned touches retain the legacy built-in path");
  require(gameplay::realtimeTouchAllowsLegacyBuiltInControl(
              {.kind = PresentationUiControlKind::LaneCover,
               .permitsLegacyBuiltInFallback = true}),
          "the built-in lane-cover adapter can explicitly retain its legacy path");
  require(!gameplay::realtimeTouchAllowsLegacyBuiltInControl(
              {.kind = PresentationUiControlKind::NativeOverlay}),
          "native overlays never leak into the legacy lane-cover handler");
  require(!gameplay::realtimeTouchAllowsLegacyBuiltInControl(
              {.kind = PresentationUiControlKind::Slider,
               .writer = skin::SkinFloatWriterId{4}}) &&
              !gameplay::realtimeTouchAllowsLegacyBuiltInControl(
                  {.kind = PresentationUiControlKind::LaneCover,
                   .writer = skin::SkinFloatWriterId{5}}),
          "skin-owned slider and lane-cover hits remain authoritative when their writer rejects");
  require(gameplay::realtimeTouchRouterTransitionCanReachWorker(false, true) &&
              !gameplay::realtimeTouchRouterTransitionCanReachWorker(true,
                                                                     false),
          "router cancellation reaches a live worker while raw ingress is detached");
  require(gameplay::realtimeTouchShouldScheduleCancelExpiry(
              gameplay::RealtimeTouchRoutingDisposition::Accepted, true,
              true) &&
              !gameplay::realtimeTouchShouldScheduleCancelExpiry(
                  gameplay::RealtimeTouchRoutingDisposition::Accepted, false,
                  false) &&
              !gameplay::realtimeTouchShouldScheduleCancelExpiry(
                  gameplay::RealtimeTouchRoutingDisposition::Accepted, true,
                  false) &&
              !gameplay::realtimeTouchShouldScheduleCancelExpiry(
                  gameplay::RealtimeTouchRoutingDisposition::RetryRequired,
                  true, true),
          "cancel expiry is scheduled only after auxiliary publication and router acknowledgment");
}

struct InputCapture {
  std::vector<gameplay::RealtimeGameplayInput> events;
  std::vector<gameplay::RealtimeGameplayInput> attempts;
  std::vector<gameplay::RealtimeTouchSample> cancelledTouches;
  bool scratchLongNoteHeld = false;
  int failedPressAttemptsRemaining = 0;
  int failedReleaseAttemptsRemaining = 0;
  int failedCancelAttemptsRemaining = 0;

  static bool emit(void *context,
                   const gameplay::RealtimeGameplayInput &event) {
    auto &capture = *static_cast<InputCapture *>(context);
    capture.attempts.push_back(event);
    if (event.type == gameplay::RealtimeGameplayInputType::Press &&
        capture.failedPressAttemptsRemaining > 0) {
      --capture.failedPressAttemptsRemaining;
      return false;
    }
    if (event.type == gameplay::RealtimeGameplayInputType::Release &&
        capture.failedReleaseAttemptsRemaining > 0) {
      --capture.failedReleaseAttemptsRemaining;
      return false;
    }
    capture.events.push_back(event);
    return true;
  }

  static bool longScratchNoteHeld(void *context, int) {
    return static_cast<InputCapture *>(context)->scratchLongNoteHeld;
  }

  static bool cancelTouchLifecycle(
      void *context, const gameplay::RealtimeTouchSample &sample) {
    auto &capture = *static_cast<InputCapture *>(context);
    if (capture.failedCancelAttemptsRemaining > 0) {
      --capture.failedCancelAttemptsRemaining;
      return false;
    }
    capture.cancelledTouches.push_back(sample);
    return true;
  }
};

struct PresentationCapture {
  enum class Call { Begin, Update, End, CancelAll };
  struct Record {
    Call call = Call::Begin;
    PresentationTouchEvent event;
    bool cancelled = false;
  };

  std::vector<Record> records;
  bool consumeTouches = true;

  static PresentationTouchResult begin(
      void *context, const PresentationTouchEvent &event) {
    static_cast<PresentationCapture *>(context)->records.push_back(
        {.call = Call::Begin, .event = event});
    const bool consumed =
        static_cast<PresentationCapture *>(context)->consumeTouches;
    return {.consumed = consumed, .excludeFromGameplay = consumed};
  }

  static PresentationTouchResult update(
      void *context, const PresentationTouchEvent &event) {
    static_cast<PresentationCapture *>(context)->records.push_back(
        {.call = Call::Update, .event = event});
    const bool consumed =
        static_cast<PresentationCapture *>(context)->consumeTouches;
    return {.consumed = consumed, .excludeFromGameplay = consumed};
  }

  static PresentationTouchResult end(void *context,
                                     const PresentationTouchEvent &event,
                                     bool cancelled) {
    static_cast<PresentationCapture *>(context)->records.push_back(
        {.call = Call::End, .event = event, .cancelled = cancelled});
    const bool consumed =
        static_cast<PresentationCapture *>(context)->consumeTouches;
    return {.consumed = consumed, .excludeFromGameplay = consumed};
  }

  static void cancelAll(void *context, long long eventMicros) {
    static_cast<PresentationCapture *>(context)->records.push_back(
        {.call = Call::CancelAll,
         .event = {.eventMicros = eventMicros},
         .cancelled = true});
  }
};

gameplay::RealtimeTouchPresentationSink presentationSink(
    PresentationCapture &capture) {
  return {.context = &capture,
          .begin = &PresentationCapture::begin,
          .update = &PresentationCapture::update,
          .end = &PresentationCapture::end,
          .cancelAll = &PresentationCapture::cancelAll};
}

gameplay::RealtimeTouchLayout makeLayout(bool dragMode = false) {
  gameplay::RealtimeTouchLayout layout;
  layout.revision = 17;
  layout.bottomLeft = {0.1F, 0.9F};
  layout.bottomRight = {0.9F, 0.9F};
  layout.topLeft = {0.3F, 0.1F};
  layout.topRight = {0.7F, 0.1F};
  layout.laneCount = 4;
  layout.lanes = {0, 1, 2, 7};
  layout.scratch = {false, false, false, true};
  layout.dragMode = dragMode;
  return layout;
}

PresentationUiHitRegion rectangleHitRegion(PresentationUiHit hit, float left,
                                           float top, float right,
                                           float bottom) {
  return {.hit = std::move(hit),
          .boundary = {{{left, top},
                        {right, top},
                        {right, bottom},
                        {left, bottom}}}};
}

void testThirdNativeOverlayIsHitTestedAndExcludedFromGameplay() {
  gameplay::RealtimeTouchLayoutRefreshKey refresh;
  refresh.nativeOverlays[2] = {
      .visible = true,
      .left = 40.0F,
      .top = 20.0F,
      .right = 60.0F,
      .bottom = 40.0F,
  };
  require(refresh.nativeOverlays.size() == 3 &&
              refresh.nativeOverlays[2].visible,
          "pause, existing system chrome, and Reset Layout each retain "
          "refresh geometry");

  gameplay::RealtimeTouchHitSnapshot snapshot{
      .layoutRevision = 33,
      .uiTransform = {.renderWidth = 100,
                      .renderHeight = 100,
                      .uiScaleX = 1.0F,
                      .uiScaleY = 1.0F,
                      .uiWidth = 100,
                      .uiHeight = 100},
      .regionsTopmostFirst = {rectangleHitRegion(
          {.kind = PresentationUiControlKind::NativeOverlay}, 40.0F, 20.0F,
          60.0F, 40.0F)}};
  gameplay::RealtimeTouchHitCaptureTracker captures;
  gameplay::RealtimeTouchSample sample{
      .fingerId = 333,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .normalizedX = 0.5F,
      .normalizedY = 0.3F,
      .steadyTimestampMicros = 100};
  gameplay::populateRealtimeTouchPresentationMetadata(sample, snapshot,
                                                       captures);
  sample.excludedFromGameplay =
      sample.presentationHit.kind != PresentationUiControlKind::None;

  InputCapture input;
  gameplay::RealtimeTouchInputRouter router(
      33, makeLayout(), {.context = &input, .emit = &InputCapture::emit});
  PresentationCapture presentation;
  gameplay::RealtimeTouchPresentationDispatcher dispatcher(
      presentationSink(presentation));
  require(sample.presentationHit.kind ==
                  PresentationUiControlKind::NativeOverlay &&
              router.consume(sample) && input.events.empty() &&
              !dispatcher.consume(sample, 100).consumed &&
              !gameplay::realtimeTouchAllowsLegacyBuiltInControl(
                  sample.presentationHit),
          "the third native overlay excludes gameplay, skin dispatch, and "
          "legacy lane-cover authority");
}

void testImmutableHitSnapshotPublishesValueOwnedTopmostGeometry() {
  const PresentationUiHit lower{
      .kind = PresentationUiControlKind::Slider,
      .layoutRevision = 71,
      .sourceObject = 1,
      .authoredOrdinal = 3,
      .writer = skin::SkinFloatWriterId{4}};
  const PresentationUiHit topmost{
      .kind = PresentationUiControlKind::LaneCover,
      .layoutRevision = 71,
      .sourceObject = 2,
      .authoredOrdinal = 9,
      .writer = skin::SkinFloatWriterId{5}};
  gameplay::RealtimeTouchHitSnapshot source{
      .layoutRevision = 17,
      .uiTransform = {.renderWidth = 2400,
                      .renderHeight = 1200,
                      .uiScaleX = 1.0F,
                      .uiScaleY = 1.0F,
                      .uiOffsetX = 240,
                      .uiOffsetY = 60},
      .regionsTopmostFirst = {
          rectangleHitRegion(topmost, 950.0F, 500.0F, 1'050.0F, 600.0F),
          rectangleHitRegion(lower, 900.0F, 450.0F, 1'100.0F, 650.0F)}};
  gameplay::RealtimeTouchHitSnapshotPublication publication;
  require(publication.publish(source),
          "main thread atomically publishes value-owned hit geometry");
  source.regionsTopmostFirst.clear();

  const auto published = publication.acquire();
  require(published && published->layoutRevision == 17 &&
              published->hitTest(0.5F, 0.5F) == topmost,
          "raw hit testing uses the immutable topmost snapshot after the source changes");
}

void testRawHitCaptureFreezesDownIdentityAndResetPermitsReuse() {
  const PresentationUiHit topmost{
      .kind = PresentationUiControlKind::Slider,
      .layoutRevision = 81,
      .sourceObject = 7,
      .authoredOrdinal = 12,
      .writer = skin::SkinFloatWriterId{13}};
  const PresentationUiHit replacement{
      .kind = PresentationUiControlKind::NativeOverlay,
      .layoutRevision = 82,
      .sourceObject = 8,
      .authoredOrdinal = 14};
  gameplay::RealtimeTouchHitSnapshot first{
      .layoutRevision = 21,
      .uiTransform = {.renderWidth = 100,
                      .renderHeight = 100,
                      .uiScaleX = 1.0F,
                      .uiScaleY = 1.0F},
      .regionsTopmostFirst = {
          rectangleHitRegion(topmost, 20.0F, 20.0F, 80.0F, 80.0F)}};
  gameplay::RealtimeTouchHitSnapshot second{
      .layoutRevision = 22,
      .uiTransform = first.uiTransform,
      .regionsTopmostFirst = {
          rectangleHitRegion(replacement, 20.0F, 20.0F, 80.0F, 80.0F)}};
  gameplay::RealtimeTouchHitCaptureTracker captures;
  require(captures.consume({.fingerId = 99,
                            .phase = gameplay::RealtimeTouchPhase::Down,
                            .normalizedX = 0.5F,
                            .normalizedY = 0.5F},
                           first) == topmost &&
              captures.consume({.fingerId = 99,
                                .phase = gameplay::RealtimeTouchPhase::Move,
                                .normalizedX = 0.5F,
                                .normalizedY = 0.5F},
                               second) == topmost,
          "move preserves the exact Down hit across overlapping snapshot replacement");
  captures.reset();
  require(captures.consume({.fingerId = 99,
                            .phase = gameplay::RealtimeTouchPhase::Down,
                            .normalizedX = 0.5F,
                            .normalizedY = 0.5F},
                           second) == replacement,
          "overflow or layout reset clears raw capture state for pointer reuse");
}

void testRawMetadataFreezesNoHitPresentationPoint() {
  gameplay::RealtimeTouchHitSnapshot snapshot{
      .layoutRevision = 91,
      .uiTransform = {.renderWidth = 200,
                      .renderHeight = 100,
                      .uiScaleX = 2.0F,
                      .uiScaleY = 2.0F,
                      .uiOffsetX = 20,
                      .uiOffsetY = 10,
                      .uiWidth = 100,
                      .uiHeight = 50}};
  gameplay::RealtimeTouchHitCaptureTracker captures;
  gameplay::RealtimeTouchSample sample{
      .fingerId = 100,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .normalizedX = 0.5F,
      .normalizedY = 0.5F};
  gameplay::populateRealtimeTouchPresentationMetadata(sample, snapshot,
                                                       captures);
  require(sample.presentationHit.kind == PresentationUiControlKind::None &&
              !sample.presentationUiPoint &&
              sample.presentationPoint ==
                  gameplay::RealtimeTouchPoint{0.4F, 0.4F},
          "raw gameplay touches freeze their presentation point even without a UI hit");
}

void testPresentationDispatchUsesTheRawSnapshotUiPoint() {
  gameplay::RealtimeTouchHitSnapshot snapshot{
      .layoutRevision = 31,
      .uiTransform = {.renderWidth = 200,
                      .renderHeight = 100,
                      .uiScaleX = 2.0F,
                      .uiScaleY = 2.0F,
                      .uiOffsetX = 20,
                      .uiOffsetY = 10,
                      .uiWidth = 100,
                      .uiHeight = 50}};
  const auto frozen = snapshot.uiPoint(0.5F, 0.5F);
  const auto frozenPresentation = snapshot.presentationPoint(0.5F, 0.5F);
  require(frozen == UiLogicalPoint{40.0F, 20.0F},
          "raw publication converts normalized input with its captured transform");
  require(frozenPresentation == gameplay::RealtimeTouchPoint{0.4F, 0.4F},
          "legacy fallback normalization is frozen from the same raw snapshot");

  PresentationCapture presentation;
  gameplay::RealtimeTouchPresentationDispatcher dispatcher(
      presentationSink(presentation));
  const gameplay::RealtimeTouchSample sample{
      .fingerId = 77,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .normalizedX = 0.5F,
      .normalizedY = 0.5F,
      .presentationHit = {.kind = PresentationUiControlKind::Slider,
                          .layoutRevision = 31,
                          .sourceObject = 4,
                          .authoredOrdinal = 5,
                          .writer = skin::SkinFloatWriterId{6}},
      .presentationUiPoint = frozen};
  snapshot.uiTransform = {.renderWidth = 800,
                          .renderHeight = 600,
                          .uiScaleX = 1.0F,
                          .uiScaleY = 1.0F};
  require(dispatcher.consume(sample, 9'000).consumed &&
              presentation.records.size() == 1 &&
              presentation.records.front().event.uiPoint == *frozen,
          "main-thread drain uses the raw sample's frozen UI point after a resize");
}

void testStableTouchLayoutRevisionParticipatesInSwitchDetection() {
  const gameplay::RealtimeTouchLayoutRefreshKey current{
      .layoutRevision = 17,
      .hitRegionRevision = 23,
      .uiTransform = {.renderWidth = 100,
                      .renderHeight = 50,
                      .uiScaleX = 1.0F,
                      .uiScaleY = 1.0F}};
  auto sameFrame = current;
  auto presentationSwitch = current;
  auto hitRegionMotion = current;
  auto overlayVisibilityChange = current;
  presentationSwitch.layoutRevision = 18;
  hitRegionMotion.hitRegionRevision = 24;
  overlayVisibilityChange.nativeOverlays[0] = {
      .visible = true, .left = 10.0F, .top = 20.0F, .right = 30.0F,
      .bottom = 40.0F};
  require(sameFrame == current,
          "rendering another frame does not invalidate stable touch routing");
  require(presentationSwitch != current,
          "presentation revision invalidates routing with unchanged drawable geometry");
  require(hitRegionMotion != current &&
              hitRegionMotion.layoutRevision == current.layoutRevision,
          "lane-cover motion invalidates hit publication without changing lane routing");
  require(overlayVisibilityChange != current,
          "native overlay visibility invalidates hit publication without a presentation rebuild");
}

gameplay::RealtimeTouchLaneRegion makeLaneRegion(
    gameplay::RealtimeTouchPoint bottomLeft,
    gameplay::RealtimeTouchPoint bottomRight,
    gameplay::RealtimeTouchPoint topLeft,
    gameplay::RealtimeTouchPoint topRight, int lane, bool scratch = false) {
  return {.bottomLeft = bottomLeft,
          .bottomRight = bottomRight,
          .topLeft = topLeft,
          .topRight = topRight,
          .lane = lane,
          .scratch = scratch};
}

gameplay::RealtimeTouchLayout makeAuthoredLayout(bool dragMode = false) {
  gameplay::RealtimeTouchLayout layout;
  layout.dragMode = dragMode;
  layout.laneRegions = {
      makeLaneRegion({0.10F, 0.90F}, {0.34F, 0.90F}, {0.24F, 0.10F},
                     {0.36F, 0.10F}, 21),
      makeLaneRegion({0.42F, 0.90F}, {0.80F, 0.90F}, {0.40F, 0.10F},
                     {0.72F, 0.10F}, 47),
  };
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

void testScratchlessTouchPreservesBmsChannelReplayMapping() {
  struct Case {
    int keyMode;
    std::vector<int> visualLanes;
  };
  for (const auto &[keyMode, visualLanes] :
       {Case{4, {0, 1, 3, 4}}, Case{6, {0, 1, 2, 4, 5, 6}},
        Case{8, {7, 0, 1, 2, 3, 4, 5, 6}}}) {
    InputCapture capture;
    auto layout = makeLayout();
    layout.keyMode = keyMode;
    layout.laneCount = visualLanes.size();
    layout.lanes = visualLanes;
    layout.scratch.assign(visualLanes.size(), false);
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
    require(capture.events.size() == 2 &&
                capture.events[0].lane == visualLanes.front() &&
                capture.events[1].lane == visualLanes.front() &&
                capture.events[0].hasReplayControl &&
                capture.events[1].hasReplayControl &&
                capture.events[0].replayControl == replay::LogicalControl{
                    .kind = replay::LogicalControlKind::Lane,
                    .player = 1,
                    .lane = visualLanes.front()} &&
                capture.events[1].replayControl ==
                    capture.events[0].replayControl,
            "non-stock BRD modes preserve their physical BMS channel lane");
  }
}

void testTouchLayoutDoesNotClampChartsAboveSixtyFourLanes() {
  InputCapture capture;
  auto layout = makeLayout();
  layout.keyMode = 65;
  layout.laneCount = 65;
  layout.lanes.clear();
  layout.scratch.assign(layout.laneCount, false);
  for (int lane = 0; lane < static_cast<int>(layout.laneCount); ++lane) {
    layout.lanes.push_back(lane);
  }
  gameplay::RealtimeTouchInputRouter router(
      42, std::move(layout),
      {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 65,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.899F,
                          .normalizedY = 0.9F,
                          .steadyTimestampMicros = 123'456}),
          "touch on a chart above sixty-four lanes reaches gameplay");
  require(capture.events.size() == 1 && capture.events.front().lane == 64 &&
              !capture.events.front().hasReplayControl,
          "touch routing preserves the final dynamic chart lane");
}

void testAuthoredLaneRegionsPreservePerspectiveWidthsAndGaps() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      66, makeAuthoredLayout(), {.context = &capture, .emit = &InputCapture::emit});

  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.25F,
                          .normalizedY = 0.20F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 1,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .steadyTimestampMicros = 2}) &&
              router.consume({.fingerId = 2,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.70F,
                              .normalizedY = 0.20F,
                              .steadyTimestampMicros = 3}) &&
              router.consume({.fingerId = 2,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .steadyTimestampMicros = 4}) &&
              router.consume({.fingerId = 3,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.38F,
                              .normalizedY = 0.50F,
                              .steadyTimestampMicros = 5}),
          "authored lane samples are accepted");
  require(capture.events.size() == 4 && capture.events[0].lane == 21 &&
              capture.events[1].lane == 21 && capture.events[2].lane == 47 &&
              capture.events[3].lane == 47,
          "perspective quads retain their authored widths while gaps stay inert");
}

void testAuthoredLaneRegionsUseFirstMatchForEdgesAndOverlaps() {
  auto layout = makeAuthoredLayout();
  layout.laneRegions = {
      makeLaneRegion({0.10F, 0.90F}, {0.50F, 0.90F}, {0.10F, 0.10F},
                     {0.50F, 0.10F}, 31),
      makeLaneRegion({0.50F, 0.90F}, {0.90F, 0.90F}, {0.50F, 0.10F},
                     {0.90F, 0.10F}, 32),
      makeLaneRegion({0.30F, 0.90F}, {0.49F, 0.90F}, {0.30F, 0.10F},
                     {0.49F, 0.10F}, 33),
  };
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      67, layout, {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.50F,
                          .normalizedY = 0.50F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 1,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .steadyTimestampMicros = 2}) &&
              router.consume({.fingerId = 2,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.40F,
                              .normalizedY = 0.50F,
                              .steadyTimestampMicros = 3}),
          "shared edges and overlaps are routable");
  require(capture.events.size() == 3 && capture.events[0].lane == 31 &&
              capture.events[1].lane == 31 && capture.events[2].lane == 31,
          "the first authored region owns shared edges and overlap priority");
}

void testAuthoredScratchRegionsFollowTheirOwnPlacement() {
  auto layout = makeAuthoredLayout();
  layout.laneRegions = {
      makeLaneRegion({0.10F, 0.90F}, {0.30F, 0.90F}, {0.10F, 0.10F},
                     {0.30F, 0.10F}, 7, true),
      makeLaneRegion({0.40F, 0.90F}, {0.60F, 0.90F}, {0.40F, 0.10F},
                     {0.60F, 0.10F}, 3),
      makeLaneRegion({0.70F, 0.90F}, {0.90F, 0.90F}, {0.70F, 0.10F},
                     {0.90F, 0.10F}, 15, true),
  };
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      68, layout, {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.50F,
                          .normalizedY = 0.50F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 2,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.20F,
                              .normalizedY = 0.50F,
                              .steadyTimestampMicros = 2}) &&
              router.consume({.fingerId = 2,
                              .phase = gameplay::RealtimeTouchPhase::Move,
                              .normalizedX = 0.20F,
                              .normalizedY = 0.48F,
                              .steadyTimestampMicros = 3}) &&
              router.consume({.fingerId = 3,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.80F,
                              .normalizedY = 0.50F,
                              .steadyTimestampMicros = 4}) &&
              router.consume({.fingerId = 3,
                              .phase = gameplay::RealtimeTouchPhase::Move,
                              .normalizedX = 0.80F,
                              .normalizedY = 0.48F,
                              .steadyTimestampMicros = 5}),
          "center normal and left/right scratch authored regions accept touch input");
  require(capture.events.size() == 3 && capture.events[0].lane == 3 &&
              capture.events[1].lane == 7 && capture.events[2].lane == 15 &&
              capture.events[1].replayControl.kind ==
                  replay::LogicalControlKind::ScratchClockwise,
          "scratch behavior follows authored geometry rather than lane order");
}

void testAuthoredLayoutReplacementCancelsTheOldRegion() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      69, makeAuthoredLayout(), {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.25F,
                          .normalizedY = 0.20F,
                          .steadyTimestampMicros = 1}),
          "authored layout begins a lane press");
  auto replacement = makeAuthoredLayout();
  replacement.laneRegions.front().lane = 99;
  require(router.updateLayout(replacement, 2),
          "authored layout replacement is accepted");
  require(capture.events.size() == 2 && capture.events[1].type ==
                                          gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].lane == 21,
          "replacing authored regions releases the old lane before switching");
}

void testLegacyLayoutAdapterKeepsItsRightSharedEdgeOwner() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      70, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.35F,
                          .normalizedY = 0.50F,
                          .steadyTimestampMicros = 1}),
          "legacy layout accepts its lane boundary");
  require(capture.events.size() == 1 && capture.events.front().lane == 1,
          "the built-in uniform adapter preserves legacy right-edge ownership");
}

void testLegacyLayoutAdapterStillClampsOutsideTheTrapezoid() {
  auto layout = makeLayout();
  layout.scratch.assign(layout.laneCount, false);
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      71, layout, {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 1,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = -0.5F,
                          .normalizedY = 1.2F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 1,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .steadyTimestampMicros = 2}) &&
              router.consume({.fingerId = 2,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 1.5F,
                              .normalizedY = -0.2F,
                              .steadyTimestampMicros = 3}),
          "legacy non-drag input accepts samples outside its trapezoid");
  require(capture.events.size() == 3 && capture.events[0].lane == 0 &&
              capture.events[1].lane == 0 && capture.events[2].lane == 7,
          "legacy uniform layout retains horizontal and vertical clamping");
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

void testPresentationTouchIsDeliveredOnceAndLayoutSwitchCancelsCapture() {
  InputCapture input;
  PresentationCapture presentation;
  gameplay::RealtimeTouchPresentationDispatcher dispatcher(
      presentationSink(presentation));
  gameplay::RealtimeTouchInputRouter router(
      72, makeLayout(),
      {.context = &input,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  const PresentationUiHit topmost{
      .kind = PresentationUiControlKind::Slider,
      .layoutRevision = 44,
      .sourceObject = 5,
      .authoredOrdinal = 9,
      .writer = skin::SkinFloatWriterId{2}};
  const PresentationUiHit overlappedLower{
      .kind = PresentationUiControlKind::Slider,
      .layoutRevision = 44,
      .sourceObject = 4,
      .authoredOrdinal = 8,
      .writer = skin::SkinFloatWriterId{1}};
  const gameplay::RealtimeTouchSample down{
      .fingerId = 41,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .normalizedX = 0.31F,
      .normalizedY = 0.50F,
      .steadyTimestampMicros = 300,
      .excludedFromGameplay = true,
      .presentationHit = topmost,
      .presentationUiPoint = UiLogicalPoint{31.0F, 50.0F}};
  const gameplay::RealtimeTouchSample move{
      .fingerId = 41,
      .phase = gameplay::RealtimeTouchPhase::Move,
      .normalizedX = 0.35F,
      .normalizedY = 0.45F,
      .steadyTimestampMicros = 310,
      .excludedFromGameplay = true,
      .presentationHit = overlappedLower,
      .presentationUiPoint = UiLogicalPoint{35.0F, 45.0F}};
  require(router.consume(down) && router.consume(move),
          "captured presentation touch is excluded by gameplay routing");
  require(dispatcher.consume(down, 1'300).consumed &&
              dispatcher.consume(move, 1'310).consumed,
          "main-thread presentation dispatch consumes begin and update");
  require(router.updateLayout(makeLayout(), 320),
          "layout replacement cancels presentation-owned router state");
  require(input.cancelledTouches.size() == 1 &&
              input.cancelledTouches.front().presentationHit == topmost,
          "layout cancellation preserves the originally captured topmost control");
  require(dispatcher.consume(input.cancelledTouches.front(), 1'320).consumed,
          "the synthesized layout cancellation reaches presentation dispatch");
  require(presentation.records.size() == 3 &&
              presentation.records[0].call ==
                  PresentationCapture::Call::Begin &&
              presentation.records[0].event.pointerId == 41 &&
              presentation.records[0].event.uiPoint ==
                  UiLogicalPoint{31.0F, 50.0F} &&
              presentation.records[0].event.hit == topmost &&
              presentation.records[1].call ==
                  PresentationCapture::Call::Update &&
              presentation.records[1].event.hit == topmost &&
              presentation.records[2].call ==
                  PresentationCapture::Call::End &&
              presentation.records[2].event.hit == topmost &&
              presentation.records[2].cancelled,
          "presentation receives begin, update, and cancel with the exact captured topmost hit");
  require(input.events.empty(),
          "captured presentation lifecycle never emits gameplay edges");
}

void testPresentationSessionSwitchCancelsAllCapturesOnce() {
  PresentationCapture presentation;
  gameplay::RealtimeTouchPresentationDispatcher dispatcher(
      presentationSink(presentation));
  const gameplay::RealtimeTouchSample down{
      .fingerId = 42,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .normalizedX = 0.2F,
      .normalizedY = 0.3F,
      .presentationHit = {.kind = PresentationUiControlKind::Slider,
                          .authoredOrdinal = 12,
                          .writer = skin::SkinFloatWriterId{7}},
      .presentationUiPoint = UiLogicalPoint{20.0F, 30.0F}};
  require(dispatcher.consume(down, 2'000).consumed,
          "session fixture begins one presentation capture");
  dispatcher.reconcileMetadataOverflow(2'100);
  dispatcher.reconcileMetadataOverflow(2'200);
  const gameplay::RealtimeTouchSample staleUp{
      .fingerId = 42,
      .phase = gameplay::RealtimeTouchPhase::Up,
      .normalizedX = 0.2F,
      .normalizedY = 0.3F,
      .presentationHit = down.presentationHit};
  require(!dispatcher.consume(staleUp, 2'300).consumed,
          "stale lift after a session switch is inert");
  require(presentation.records.size() == 2 &&
              presentation.records[0].call ==
                  PresentationCapture::Call::Begin &&
              presentation.records[1].call ==
                  PresentationCapture::Call::CancelAll &&
              presentation.records[1].event.eventMicros == 2'100,
          "session switch cancels all active captures exactly once");

  const gameplay::RealtimeTouchSample reusedDown{
      .fingerId = 42,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .presentationHit = {.kind = PresentationUiControlKind::Slider,
                          .layoutRevision = 13,
                          .sourceObject = 8,
                          .authoredOrdinal = 14,
                          .writer = skin::SkinFloatWriterId{9}},
      .presentationUiPoint = UiLogicalPoint{25.0F, 35.0F}};
  require(dispatcher.consume(reusedDown, 2'400).consumed &&
              presentation.records.back().event.hit ==
                  reusedDown.presentationHit,
          "metadata overflow reconciliation permits the pointer ID to begin again");
}

void testUnconsumedPresentationBeginDoesNotCapturePointer() {
  PresentationCapture presentation;
  presentation.consumeTouches = false;
  gameplay::RealtimeTouchPresentationDispatcher dispatcher(
      presentationSink(presentation));
  for (std::int64_t pointer = 0;
       pointer < static_cast<std::int64_t>(
                     gameplay::kRealtimeTouchFingerCapacity);
       ++pointer) {
    const gameplay::RealtimeTouchSample down{
        .fingerId = pointer,
        .phase = gameplay::RealtimeTouchPhase::Down,
        .presentationHit = {
            .kind = PresentationUiControlKind::LaneCover,
            .permitsLegacyBuiltInFallback = true},
        .presentationUiPoint = UiLogicalPoint{10.0F, 20.0F}};
    require(!dispatcher.consume(down, 3'000 + pointer).consumed,
            "unconsumed built-in presentation begin remains router-owned");
  }
  const gameplay::RealtimeTouchSample staleMove{
      .fingerId = 0,
      .phase = gameplay::RealtimeTouchPhase::Move,
      .presentationHit = {.kind = PresentationUiControlKind::LaneCover}};
  require(!dispatcher.consume(staleMove, 3'100).consumed,
          "unconsumed begin receives no later update");
  const gameplay::RealtimeTouchSample staleUp{
      .fingerId = 0,
      .phase = gameplay::RealtimeTouchPhase::Up,
      .presentationHit = {.kind = PresentationUiControlKind::LaneCover}};
  require(!dispatcher.consume(staleUp, 3'101).consumed,
          "unconsumed begin receives no later end");
  dispatcher.cancelAll(3'102);
  require(presentation.records.size() ==
              gameplay::kRealtimeTouchFingerCapacity,
          "unconsumed begins do not participate in session cancellation");

  presentation.consumeTouches = true;
  const gameplay::RealtimeTouchSample acceptedDown{
      .fingerId = 999,
      .phase = gameplay::RealtimeTouchPhase::Down,
      .presentationHit = {.kind = PresentationUiControlKind::Slider},
      .presentationUiPoint = UiLogicalPoint{30.0F, 40.0F}};
  require(dispatcher.consume(acceptedDown, 3'200).consumed,
          "rejected begins release every bounded capture slot for reuse");
  dispatcher.cancelAll(3'300);
  require(presentation.records.size() ==
              gameplay::kRealtimeTouchFingerCapacity + 2 &&
              presentation.records[gameplay::kRealtimeTouchFingerCapacity]
                      .call == PresentationCapture::Call::Begin &&
              presentation.records.back().call ==
                  PresentationCapture::Call::CancelAll,
          "only the later consumed begin participates in session cancellation");
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

void testFailedLayoutCancellationRetainsOwnershipAndRetriesAtomically() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      81, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 51,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}),
          "failed-layout fixture presses the old lane");
  auto replacement = makeLayout();
  replacement.lanes[0] = 10;
  capture.failedReleaseAttemptsRemaining = 1;
  require(!router.updateLayout(replacement, 2) &&
              capture.events.size() == 1 &&
              capture.cancelledTouches.size() == 1,
          "failed release rejects layout replacement while preserving its cancellation record");
  require(router.updateLayout(replacement, 3) &&
              capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events.back().lane == 0 &&
              capture.cancelledTouches.size() == 1,
          "retry releases the retained old lane without duplicating lifecycle cancellation");
  require(router.consume({.fingerId = 52,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 4}) &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events.back().lane == 10,
          "only the successful retry commits the replacement mapping");
}

void testFailedLifecycleCancellationRetainsLaneUntilRetry() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      82, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 61,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 10}),
          "failed-cancel fixture presses one lane");
  capture.failedCancelAttemptsRemaining = 1;
  require(!router.setGameplayEnabled(false, 11) &&
              capture.events.size() == 1 &&
              capture.cancelledTouches.empty(),
          "failed lifecycle publication leaves local and worker lane ownership paired");
  require(router.setGameplayEnabled(true, 12) &&
              capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.cancelledTouches.size() == 1,
          "resume retries cancellation before accepting new gameplay touches");
}

void testFailedMoveToPresentationOwnershipKeepsLaneUntilReleaseRetry() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      83, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 71,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 20}),
          "presentation-transition fixture presses a gameplay lane");
  capture.failedReleaseAttemptsRemaining = 1;
  require(!router.consume(
              {.fingerId = 71,
               .phase = gameplay::RealtimeTouchPhase::Move,
               .normalizedX = 0.4F,
               .normalizedY = 0.4F,
               .steadyTimestampMicros = 21,
               .excludedFromGameplay = true,
               .presentationHit = {
                   .kind = PresentationUiControlKind::Slider,
                   .writer = skin::SkinFloatWriterId{4}},
               .presentationUiPoint = UiLogicalPoint{40.0F, 40.0F}}),
          "failed release rejects transfer to presentation ownership");
  require(router.consume({.fingerId = 71,
                          .phase = gameplay::RealtimeTouchPhase::Up,
                          .normalizedX = 0.4F,
                          .normalizedY = 0.4F,
                          .steadyTimestampMicros = 22}) &&
              capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release,
          "later lift retries the retained gameplay release instead of bypassing it as presentation-owned");
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

void testCancelledDragPointerCannotReenterOnMoveAfterResume() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      84, makeLayout(true),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 88,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}) &&
              router.setGameplayEnabled(false, 2) &&
              router.setGameplayEnabled(true, 3),
          "forced cancellation releases a drag pointer and resumes routing");
  const std::size_t eventCountAfterResume = capture.events.size();
  require(router.consume({.fingerId = 88,
                          .phase = gameplay::RealtimeTouchPhase::Move,
                          .normalizedX = 0.69F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 4}) &&
              capture.events.size() == eventCountAfterResume,
          "a still-down cancelled pointer cannot claim the new layout from a stale Move");
  require(router.consume({.fingerId = 88,
                          .phase = gameplay::RealtimeTouchPhase::Up,
                          .normalizedX = 0.69F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 5}) &&
              router.consume({.fingerId = 88,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 6}) &&
              capture.events.size() == eventCountAfterResume + 1 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Press,
          "the physical lift clears the tombstone and a later Down may claim a lane");
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

void testCancelledTouchDownReleasesOldLaneBeforeStartingNewContact() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      85, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 91,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 91,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 2}) &&
              router.consume({.fingerId = 91,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.50F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 3}),
          "a new Down during cancel grace starts a new physical contact");
  require(capture.events.size() == 3 &&
              capture.events[0].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[0].lane == 0 &&
              capture.events[1].type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].lane == 0 &&
              capture.events[1].steadyTimestampMicros == 3 &&
              capture.events[2].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[2].lane != 0,
          "restart releases the old lane exactly once before pressing the new target");
  require(capture.cancelledTouches.empty(),
          "cancel grace restart does not duplicate forced-cancellation lifecycle metadata");
}

void testCancelledTouchDownRetainsOldLaneWhenRestartReleaseFails() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      86, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 92,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 92,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 2}),
          "restart-failure fixture holds its initial lane through cancel grace");
  capture.failedReleaseAttemptsRemaining = 1;
  const auto rejected = router.consumeForPublication(
      {.fingerId = 92,
       .phase = gameplay::RealtimeTouchPhase::Down,
       .normalizedX = 0.50F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 3});
  require(rejected == gameplay::RealtimeTouchRoutingDisposition::RetryRequired &&
              !gameplay::realtimeTouchRoutingPublishesAuxiliary(rejected) &&
              gameplay::realtimeTouchRoutingRequiresRecovery(rejected) &&
              capture.events.size() == 1 && capture.attempts.size() == 2 &&
              capture.attempts.back().type ==
                  gameplay::RealtimeGameplayInputType::Release,
          "failed restart release rejects publication and requests fail-closed recovery without replacing old ownership");
  require(router.consume({.fingerId = 92,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.50F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 4}) &&
              capture.events.size() == 3 &&
              capture.events[1].type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events[1].lane == 0 &&
              capture.events[2].type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events[2].lane != 0,
          "the same new Down retries the retained release before beginning its fresh lane");
  require(capture.cancelledTouches.empty(),
          "retrying a grace restart keeps forced-cancellation metadata exact");
}

void testDuplicateDownWithoutCancelDoesNotDuplicatePress() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      87, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});
  const auto first = router.consumeForPublication(
      {.fingerId = 93,
       .phase = gameplay::RealtimeTouchPhase::Down,
       .normalizedX = 0.31F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 1});
  const auto duplicate = router.consumeForPublication(
      {.fingerId = 93,
       .phase = gameplay::RealtimeTouchPhase::Down,
       .normalizedX = 0.50F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 2});
  require(first == gameplay::RealtimeTouchRoutingDisposition::Accepted &&
              gameplay::realtimeTouchRoutingPublishesAuxiliary(first) &&
              duplicate == gameplay::RealtimeTouchRoutingDisposition::Inert &&
              !gameplay::realtimeTouchRoutingPublishesAuxiliary(duplicate) &&
              !gameplay::realtimeTouchRoutingRequiresRecovery(duplicate) &&
              capture.events.size() == 1 &&
              capture.events.front().type ==
                  gameplay::RealtimeGameplayInputType::Press &&
              capture.events.front().lane == 0,
          "duplicate Down without cancellation leaves the original press untouched");
  require(router.consume({.fingerId = 93,
                          .phase = gameplay::RealtimeTouchPhase::Up,
                          .normalizedX = 0.50F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 3}) &&
              capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release &&
              capture.events.back().lane == 0,
          "the eventual lift releases the original contact exactly once");
}

void testRejectedInitialDownSuppressesStaleMoveUntilLift() {
  InputCapture capture;
  capture.failedPressAttemptsRemaining = 1;
  gameplay::RealtimeTouchInputRouter router(
      88, makeLayout(true), {.context = &capture, .emit = &InputCapture::emit});
  const auto rejected = router.consumeForPublication(
      {.fingerId = 94,
       .phase = gameplay::RealtimeTouchPhase::Down,
       .normalizedX = 0.31F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 1});
  const auto staleMove = router.consumeForPublication(
      {.fingerId = 94,
       .phase = gameplay::RealtimeTouchPhase::Move,
       .normalizedX = 0.50F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 2});
  const auto staleCancel = router.consumeForPublication(
      {.fingerId = 94,
       .phase = gameplay::RealtimeTouchPhase::Cancel,
       .normalizedX = 0.50F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 3});
  const auto lift = router.consumeForPublication(
      {.fingerId = 94,
       .phase = gameplay::RealtimeTouchPhase::Up,
       .normalizedX = 0.50F,
       .normalizedY = 0.5F,
       .steadyTimestampMicros = 4});
  require(rejected == gameplay::RealtimeTouchRoutingDisposition::RetryRequired &&
              staleMove == gameplay::RealtimeTouchRoutingDisposition::Inert &&
              staleCancel == gameplay::RealtimeTouchRoutingDisposition::Inert &&
              lift == gameplay::RealtimeTouchRoutingDisposition::Inert &&
              capture.events.empty(),
          "a rejected initial Down suppresses every stale lifecycle sample from gameplay and auxiliary publication");
  require(router.consume({.fingerId = 94,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 5}) &&
              capture.events.size() == 1 &&
              capture.events.front().type ==
                  gameplay::RealtimeGameplayInputType::Press,
          "physical lift clears the rejected-contact tombstone for the next Down");
}

void testPublishedNativeCancelIsNotSynthesizedAgain() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      89, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 95,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 95,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 2}) &&
              router.acknowledgePublishedCancellation(95) &&
              router.cancelAll(3),
          "the router accepts acknowledgment only for a live native cancellation");
  require(capture.cancelledTouches.empty() && capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release,
          "global recovery releases an acknowledged cancellation without publishing a duplicate Cancel");
}

void testUnpublishedNativeCancelIsSynthesizedDuringRecovery() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      90, makeLayout(),
      {.context = &capture,
       .emit = &InputCapture::emit,
       .cancelTouchLifecycle = &InputCapture::cancelTouchLifecycle});
  require(router.consume({.fingerId = 96,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 1}) &&
              router.consume({.fingerId = 96,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 2}) &&
              router.cancelAll(3),
          "recovery completes when the native Cancel could not be published");
  require(capture.cancelledTouches.size() == 1 &&
              capture.cancelledTouches.front().fingerId == 96 &&
              capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release,
          "recovery synthesizes exactly one missing cancellation before release");
}

void testFailedCancelExpiryRequestsRecovery() {
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      91, makeLayout(), {.context = &capture, .emit = &InputCapture::emit});
  require(router.consume({.fingerId = 97,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = 0.31F,
                          .normalizedY = 0.5F,
                          .steadyTimestampMicros = 100'000}) &&
              router.consume({.fingerId = 97,
                              .phase = gameplay::RealtimeTouchPhase::Cancel,
                              .normalizedX = 0.31F,
                              .normalizedY = 0.5F,
                              .steadyTimestampMicros = 110'000}),
          "expiry-recovery fixture enters cancellation grace");
  capture.failedReleaseAttemptsRemaining = 1;
  const auto expiry = router.consumeForPublication(
      {.fingerId = 97,
       .phase = gameplay::RealtimeTouchPhase::CancelExpired,
       .steadyTimestampMicros = 160'000});
  require(expiry == gameplay::RealtimeTouchRoutingDisposition::RetryRequired &&
              gameplay::realtimeTouchRoutingRequiresRecovery(expiry) &&
              router.cancelAll(160'001) && capture.events.size() == 2 &&
              capture.events.back().type ==
                  gameplay::RealtimeGameplayInputType::Release,
          "a failed grace-expiry release remains owned and succeeds through fail-closed recovery");
}

void testVirtualControllerLayoutRoutesKeysAndSystemControls() {
  input::VirtualControllerConfig config;
  config.enabled = true;
  config.centerX = 0.5F;
  config.centerY = 0.70F;
  config.buttonSize = 0.10F;
  config.keyGap = 0.20F;
  const gameplay::VirtualControllerCanvas canvas{
      .x = 0.0F, .y = 0.0F, .width = 1000.0F, .height = 600.0F};
  const auto controller =
      gameplay::makeVirtualControllerLayout(config, 7, canvas);
  require(controller.valid() && controller.elements.size() == 10,
          "the 7-key virtual controller has scratch, seven keys, Start, and Select");

  const auto findElement = [&](gameplay::VirtualControllerControl control,
                               int keyPosition = -1)
      -> const gameplay::VirtualControllerElement * {
    for (const auto &element : controller.elements) {
      if (element.control == control && element.keyPosition == keyPosition) {
        return &element;
      }
    }
    return nullptr;
  };
  const auto *scratch =
      findElement(gameplay::VirtualControllerControl::Scratch);
  const auto *keyOne = findElement(gameplay::VirtualControllerControl::Key, 0);
  const auto *keyTwo = findElement(gameplay::VirtualControllerControl::Key, 1);
  const auto *start = findElement(gameplay::VirtualControllerControl::Start);
  const auto *select = findElement(gameplay::VirtualControllerControl::Select);
  require(scratch != nullptr && scratch->scratch && scratch->lane == 7 &&
              keyOne != nullptr && keyOne->lane == 0 && keyTwo != nullptr &&
              keyTwo->lane == 1 && keyTwo->bounds.y < keyOne->bounds.y &&
              start != nullptr &&
              start->replayControl == replay::LogicalControl{
                                          .kind = replay::LogicalControlKind::Start,
                                          .player = 1,
                                          .lane = -1} &&
              select != nullptr &&
              select->replayControl == replay::LogicalControl{
                                           .kind = replay::LogicalControlKind::Select,
                                           .player = 1,
                                           .lane = -1},
          "virtual controls preserve canonical lanes and the alternating key rows");

  const gameplay::RealtimeTouchUiTransform transform{
      .renderWidth = 1000,
      .renderHeight = 600,
      .uiScaleX = 1.0F,
      .uiScaleY = 1.0F,
      .uiWidth = 1000,
      .uiHeight = 600,
  };
  gameplay::RealtimeTouchHitSnapshot circularHitSnapshot{
      .uiTransform = transform,
      .regionsTopmostFirst = {
          {.hit = {.kind = PresentationUiControlKind::VirtualController},
           .boundary = {{{470.0F, 270.0F},
                         {530.0F, 270.0F},
                         {530.0F, 330.0F},
                         {470.0F, 330.0F}}},
           .circle = PresentationUiCircle{
               .center = {.x = 500.0F, .y = 300.0F}, .radius = 30.0F}}}};
  require(circularHitSnapshot.hitTest(0.5F, 0.5F).kind ==
                  PresentationUiControlKind::VirtualController &&
              circularHitSnapshot.hitTest(0.529F, 0.529F).kind ==
                  PresentationUiControlKind::None,
          "the scratch overlay uses its circular geometry instead of a bounding box");
  gameplay::RealtimeTouchLayout touchLayout;
  touchLayout.revision = 1;
  touchLayout.keyMode = 7;
  touchLayout.laneRegions =
      gameplay::makeVirtualControllerTouchRegions(controller, transform);
  InputCapture capture;
  gameplay::RealtimeTouchInputRouter router(
      92, std::move(touchLayout), {.context = &capture, .emit = &InputCapture::emit});
  const auto center = [&](const gameplay::VirtualControllerElement &element) {
    return std::pair{(element.bounds.x + element.bounds.width * 0.5F) /
                         canvas.width,
                     (element.bounds.y + element.bounds.height * 0.5F) /
                         canvas.height};
  };
  const auto [keyX, keyY] = center(*keyOne);
  const auto [startX, startY] = center(*start);
  require(router.consume({.fingerId = 201,
                          .phase = gameplay::RealtimeTouchPhase::Down,
                          .normalizedX = keyX,
                          .normalizedY = keyY,
                          .steadyTimestampMicros = 10}) &&
              router.consume({.fingerId = 201,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .normalizedX = keyX,
                              .normalizedY = keyY,
                              .steadyTimestampMicros = 11}) &&
              router.consume({.fingerId = 202,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = startX,
                              .normalizedY = startY,
                              .steadyTimestampMicros = 12}) &&
              router.consume({.fingerId = 202,
                              .phase = gameplay::RealtimeTouchPhase::Up,
                              .normalizedX = startX,
                              .normalizedY = startY,
                              .steadyTimestampMicros = 13}),
          "virtual control contacts are routable through the real-time router");
  require(capture.events.size() == 4 && capture.events[0].lane == 0 &&
              capture.events[1].lane == 0 && capture.events[2].lane == -1 &&
              capture.events[3].lane == -1 &&
              capture.events[2].replayControl.kind ==
                  replay::LogicalControlKind::Start &&
              capture.events[3].replayControl.kind ==
                  replay::LogicalControlKind::Start,
          "virtual keys and Start emit their canonical replay edges");

  gameplay::RealtimeTouchLayout dragLayout;
  dragLayout.revision = 2;
  dragLayout.keyMode = 7;
  dragLayout.dragMode = true;
  dragLayout.laneRegions =
      gameplay::makeVirtualControllerTouchRegions(controller, transform);
  InputCapture dragCapture;
  gameplay::RealtimeTouchInputRouter dragRouter(
      93, std::move(dragLayout),
      {.context = &dragCapture, .emit = &InputCapture::emit});
  const auto [selectX, selectY] = center(*select);
  require(dragRouter.consume({.fingerId = 203,
                              .phase = gameplay::RealtimeTouchPhase::Down,
                              .normalizedX = startX,
                              .normalizedY = startY,
                              .steadyTimestampMicros = 20}) &&
              dragRouter.consume({.fingerId = 203,
                                  .phase = gameplay::RealtimeTouchPhase::Move,
                                  .normalizedX = selectX,
                                  .normalizedY = selectY,
                                  .steadyTimestampMicros = 21}) &&
              dragRouter.consume({.fingerId = 203,
                                  .phase = gameplay::RealtimeTouchPhase::Up,
                                  .normalizedX = selectX,
                                  .normalizedY = selectY,
                                  .steadyTimestampMicros = 22}) &&
              dragCapture.events.size() == 4 &&
              dragCapture.events[0].replayControl.kind ==
                  replay::LogicalControlKind::Start &&
              dragCapture.events[1].replayControl.kind ==
                  replay::LogicalControlKind::Start &&
              dragCapture.events[2].replayControl.kind ==
                  replay::LogicalControlKind::Select &&
              dragCapture.events[3].replayControl.kind ==
                  replay::LogicalControlKind::Select,
          "drag mode changes between Start and Select instead of conflating their shared lane sentinel");
}

} // namespace

int main() {
  testPresentationFrameContractCarriesExactSerialOnFailure();
  testTouchPresentationUsesUiNormalizedCoordinates();
  testLegacyBuiltInTouchFallbackRequiresExplicitOwnership();
  testImmutableHitSnapshotPublishesValueOwnedTopmostGeometry();
  testRawHitCaptureFreezesDownIdentityAndResetPermitsReuse();
  testRawMetadataFreezesNoHitPresentationPoint();
  testPresentationDispatchUsesTheRawSnapshotUiPoint();
  testStableTouchLayoutRevisionParticipatesInSwitchDetection();
  testThirdNativeOverlayIsHitTestedAndExcludedFromGameplay();
  testChartLaneMappingCoversEveryReplayKeyMode();
  testDirectTouchEmitsTimestampedLaneEdges();
  testScratchlessTouchPreservesBmsChannelReplayMapping();
  testTouchLayoutDoesNotClampChartsAboveSixtyFourLanes();
  testAuthoredLaneRegionsPreservePerspectiveWidthsAndGaps();
  testAuthoredLaneRegionsUseFirstMatchForEdgesAndOverlaps();
  testAuthoredScratchRegionsFollowTheirOwnPlacement();
  testAuthoredLayoutReplacementCancelsTheOldRegion();
  testLegacyLayoutAdapterKeepsItsRightSharedEdgeOwner();
  testLegacyLayoutAdapterStillClampsOutsideTheTrapezoid();
  testDragModeChangesLaneWithoutWaitingForAFrame();
  testScratchFlickEmitsAtomicBackspinAndPressPair();
  testScratchLongNoteIgnoresSmallDirectionJitter();
  testNormalModeMapsTouchesBelowProjectedPlayfield();
  testUiExcludedFingerNeverEmitsGameplayEdges();
  testPresentationTouchIsDeliveredOnceAndLayoutSwitchCancelsCapture();
  testPresentationSessionSwitchCancelsAllCapturesOnce();
  testUnconsumedPresentationBeginDoesNotCapturePointer();
  testLayoutReplacementCancelsOldLaneBeforeNewMapping();
  testFailedLayoutCancellationRetainsOwnershipAndRetriesAtomically();
  testFailedLifecycleCancellationRetainsLaneUntilRetry();
  testFailedMoveToPresentationOwnershipKeepsLaneUntilReleaseRetry();
  testPauseReleasesHeldFingerBeforeDisablingGameplay();
  testCancelledDragPointerCannotReenterOnMoveAfterResume();
  testCancelledTouchUsesGraceAndContinuationCancelsExpiry();
  testCancelledTouchDownReleasesOldLaneBeforeStartingNewContact();
  testCancelledTouchDownRetainsOldLaneWhenRestartReleaseFails();
  testDuplicateDownWithoutCancelDoesNotDuplicatePress();
  testRejectedInitialDownSuppressesStaleMoveUntilLift();
  testPublishedNativeCancelIsNotSynthesizedAgain();
  testUnpublishedNativeCancelIsSynthesizedDuringRecovery();
  testFailedCancelExpiryRequestsRecovery();
  testVirtualControllerLayoutRoutesKeysAndSystemControls();
  return 0;
}
