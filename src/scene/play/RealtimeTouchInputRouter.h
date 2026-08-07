#pragma once

#include "RealtimeGameplayWorker.h"
#include "../../skin/SkinPresentationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gameplay {

inline constexpr std::size_t kRealtimeTouchFingerCapacity = 32;
inline constexpr std::size_t kRealtimeTouchNativeOverlayCapacity = 3;

struct RealtimeTouchPoint {
  float x = 0.0F;
  float y = 0.0F;

  bool operator==(const RealtimeTouchPoint &) const = default;
};

struct RealtimeTouchCircle {
  RealtimeTouchPoint center;
  float radiusX = 0.0F;
  float radiusY = 0.0F;
};

// Ordered hit region authored by the gameplay skin. Regions are tested in
// vector order, so an earlier region owns a shared edge or overlap.
struct RealtimeTouchLaneRegion {
  RealtimeTouchPoint bottomLeft;
  RealtimeTouchPoint bottomRight;
  RealtimeTouchPoint topLeft;
  RealtimeTouchPoint topRight;
  int lane = -1;
  bool scratch = false;
  // Only the virtual controller opts into angular turntable handling. Skin
  // and built-in playfield scratch regions retain their established flick
  // gesture semantics.
  bool spinScratch = false;
  std::optional<replay::LogicalControl> replayControl;
  // Virtual controls require an actual hit. Skin lanes retain the legacy
  // vertical clamping behavior below their authored playfield.
  bool requiresInside = false;
  std::optional<RealtimeTouchCircle> circle;
};

struct RealtimeTouchLayout {
  // Stable routing identity. Change this only when the selected presentation
  // or authored lane topology changes; it is not a rendered-frame serial.
  std::uint64_t revision = 0;
  RealtimeTouchPoint bottomLeft;
  RealtimeTouchPoint bottomRight;
  RealtimeTouchPoint topLeft;
  RealtimeTouchPoint topRight;
  std::vector<int> lanes;
  std::vector<bool> scratch;
  std::vector<RealtimeTouchLaneRegion> laneRegions;
  std::size_t laneCount = 0;
  int keyMode = 7;
  bool dragMode = false;
};

struct RealtimeTouchUiTransform {
  int renderWidth = 0;
  int renderHeight = 0;
  float uiScaleX = 0.0F;
  float uiScaleY = 0.0F;
  int uiOffsetX = 0;
  int uiOffsetY = 0;
  int uiWidth = 0;
  int uiHeight = 0;

  bool operator==(const RealtimeTouchUiTransform &) const = default;
};

struct RealtimeTouchNativeOverlayRegion {
  bool visible = false;
  float left = 0.0F;
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;

  bool operator==(const RealtimeTouchNativeOverlayRegion &) const = default;
};

struct RealtimeTouchLayoutRefreshKey {
  std::uint64_t layoutRevision = 0;
  std::uint64_t hitRegionRevision = 0;
  RealtimeTouchUiTransform uiTransform;
  std::array<RealtimeTouchNativeOverlayRegion,
             kRealtimeTouchNativeOverlayCapacity>
      nativeOverlays{};

  bool operator==(const RealtimeTouchLayoutRefreshKey &) const = default;
};

struct RealtimeTouchHitSnapshot {
  std::uint64_t layoutRevision = 0;
  RealtimeTouchUiTransform uiTransform;
  std::vector<PresentationUiHitRegion> regionsTopmostFirst;

  [[nodiscard]] std::optional<UiLogicalPoint>
  uiPoint(float normalizedX, float normalizedY) const noexcept;
  [[nodiscard]] std::optional<RealtimeTouchPoint>
  presentationPoint(float normalizedX, float normalizedY) const noexcept;
  [[nodiscard]] PresentationUiHit hitTest(float normalizedX,
                                          float normalizedY) const noexcept;
};

class RealtimeTouchHitSnapshotPublication final {
public:
  [[nodiscard]] bool publish(RealtimeTouchHitSnapshot snapshot) noexcept;
  [[nodiscard]] std::shared_ptr<const RealtimeTouchHitSnapshot>
  acquire() const noexcept;
  void clear() noexcept;

private:
  // Use the shared_ptr atomic free functions for compatibility with the
  // project's libc++, which does not provide atomic<shared_ptr> specialization.
  std::shared_ptr<const RealtimeTouchHitSnapshot> published_;
};

enum class RealtimeTouchPhase : std::uint8_t {
  Down,
  Move,
  Up,
  Cancel,
  CancelExpired
};

struct RealtimeTouchSample {
  std::int64_t fingerId = 0;
  RealtimeTouchPhase phase = RealtimeTouchPhase::Move;
  float normalizedX = 0.0F;
  float normalizedY = 0.0F;
  std::int64_t steadyTimestampMicros = 0;
  bool excludedFromGameplay = false;
  PresentationUiHit presentationHit;
  std::optional<UiLogicalPoint> presentationUiPoint;
  std::optional<RealtimeTouchPoint> presentationPoint;

  bool operator==(const RealtimeTouchSample &) const = default;
};

enum class RealtimeTouchRoutingDisposition : std::uint8_t {
  Accepted,
  Inert,
  RetryRequired,
};

[[nodiscard]] inline bool realtimeTouchRoutingPublishesAuxiliary(
    RealtimeTouchRoutingDisposition disposition) noexcept {
  return disposition == RealtimeTouchRoutingDisposition::Accepted;
}

[[nodiscard]] inline bool realtimeTouchRoutingRequiresRecovery(
    RealtimeTouchRoutingDisposition disposition) noexcept {
  return disposition == RealtimeTouchRoutingDisposition::RetryRequired;
}

[[nodiscard]] inline bool realtimeTouchShouldScheduleCancelExpiry(
    RealtimeTouchRoutingDisposition disposition, bool auxiliaryPublished,
    bool cancellationAcknowledged) noexcept {
  return disposition == RealtimeTouchRoutingDisposition::Accepted &&
         auxiliaryPublished && cancellationAcknowledged;
}

// Raw ingress acceptance gates native callback entry, not router-generated
// releases. Once the UIKit sink is detached, cancellation must still reach a
// live worker or a held lane can never be released.
[[nodiscard]] inline bool realtimeTouchRouterTransitionCanReachWorker(
    bool rawIngressAccepting, bool workerAvailable) noexcept {
  (void)rawIngressAccepting;
  return workerAvailable;
}

class RealtimeTouchHitCaptureTracker final {
public:
  [[nodiscard]] PresentationUiHit
  consume(const RealtimeTouchSample &sample,
          const RealtimeTouchHitSnapshot &snapshot) noexcept;
  void reset() noexcept;

private:
  struct Capture {
    std::int64_t pointerId = 0;
    bool active = false;
    PresentationUiHit hit;
  };

  std::array<Capture, kRealtimeTouchFingerCapacity> captures_{};
};

// Populate every value that must remain stable between the native callback
// and main-thread drain. UI logical coordinates are needed only for a captured
// presentation control, while the normalized presentation point is needed for
// ordinary gameplay replay/visualization as well.
void populateRealtimeTouchPresentationMetadata(
    RealtimeTouchSample &sample, const RealtimeTouchHitSnapshot &snapshot,
    RealtimeTouchHitCaptureTracker &captures) noexcept;

// Main-thread presentation adapter. RealtimeTouchInputRouter never calls this
// sink: native input only carries the immutable down-hit alongside its
// excluded gameplay sample, and GamePlayScene dispatches it while draining the
// auxiliary queue.
struct RealtimeTouchPresentationSink {
  void *context = nullptr;
  PresentationTouchResult (*begin)(void *, const PresentationTouchEvent &) =
      nullptr;
  PresentationTouchResult (*update)(void *, const PresentationTouchEvent &) =
      nullptr;
  PresentationTouchResult (*end)(void *, const PresentationTouchEvent &,
                                 bool cancelled) = nullptr;
  void (*cancelAll)(void *, long long eventMicros) = nullptr;
};

class RealtimeTouchPresentationDispatcher {
public:
  RealtimeTouchPresentationDispatcher() = default;
  explicit RealtimeTouchPresentationDispatcher(
      RealtimeTouchPresentationSink sink) noexcept
      : sink_(sink) {}

  void setSink(RealtimeTouchPresentationSink sink) noexcept;
  [[nodiscard]] PresentationTouchResult
  consume(const RealtimeTouchSample &sample, long long eventMicros) noexcept;
  void cancelAll(long long eventMicros) noexcept;
  void reconcileMetadataOverflow(long long eventMicros) noexcept;

private:
  struct Capture {
    std::int64_t pointerId = 0;
    bool active = false;
    PresentationUiHit hit;
    UiLogicalPoint uiPoint;
  };

  [[nodiscard]] Capture *find(std::int64_t pointerId) noexcept;
  [[nodiscard]] Capture *allocate(std::int64_t pointerId) noexcept;

  RealtimeTouchPresentationSink sink_;
  std::array<Capture, kRealtimeTouchFingerCapacity> captures_{};
};

struct RealtimeTouchInputSink {
  void *context = nullptr;
  bool (*emit)(void *, const RealtimeGameplayInput &) = nullptr;
  bool (*scratchLongNoteHeld)(void *, int lane) = nullptr;
  bool (*cancelTouchLifecycle)(void *, const RealtimeTouchSample &) = nullptr;
};

class RealtimeTouchInputRouter {
public:
  RealtimeTouchInputRouter(std::uint64_t epoch, RealtimeTouchLayout layout,
                           RealtimeTouchInputSink sink) noexcept;

  bool consume(const RealtimeTouchSample &sample) noexcept;
  [[nodiscard]] RealtimeTouchRoutingDisposition
  consumeForPublication(const RealtimeTouchSample &sample) noexcept;
  [[nodiscard]] bool
  acknowledgePublishedCancellation(std::int64_t fingerId) noexcept;
  bool cancelAll(std::int64_t steadyTimestampMicros) noexcept;
  bool updateLayout(RealtimeTouchLayout layout,
                    std::int64_t steadyTimestampMicros) noexcept;
  // Advances the source-compatible stop threshold for active virtual
  // turntable gestures. The caller supplies the monotonic gameplay clock.
  bool advanceSpinScratch(std::int64_t steadyTimestampMicros) noexcept;
  bool setGameplayEnabled(bool enabled,
                          std::int64_t steadyTimestampMicros) noexcept;
  void reset() noexcept;
  [[nodiscard]] float spinScratchRotationDegrees() const noexcept;

private:
  struct FingerState {
    std::int64_t fingerId = 0;
    int lane = -1;
    bool active = false;
    bool excluded = false;
    bool pressed = false;
    bool scratch = false;
    bool spinScratch = false;
    int scratchDirection = 0;
    std::optional<replay::LogicalControl> replayControl;
    float lastX = 0.0F;
    float lastY = 0.0F;
    RealtimeTouchPoint spinCenter;
    float spinPreviousAngleRadians = 0.0F;
    float spinAccumulatedDegrees = 0.0F;
    std::int64_t spinLastStepMicros = 0;
    std::int64_t cancelDeadlineMicros = 0;
    PresentationUiHit presentationHit;
    std::optional<UiLogicalPoint> presentationUiPoint;
    std::optional<RealtimeTouchPoint> presentationPoint;
    bool cancellationPublished = false;
    bool suppressedUntilLift = false;
  };

  [[nodiscard]] std::optional<std::size_t>
  laneIndexAt(float x, float y, bool requireInside) const noexcept;
  [[nodiscard]] static bool
  normalizeLayout(RealtimeTouchLayout &layout) noexcept;
  [[nodiscard]] FingerState *findFinger(std::int64_t fingerId) noexcept;
  [[nodiscard]] FingerState *allocateFinger(std::int64_t fingerId) noexcept;
  [[nodiscard]] bool laneOccupied(int lane,
                                  std::int64_t exceptFinger) const noexcept;
  bool consumeImpl(const RealtimeTouchSample &sample,
                   bool &publishAuxiliary) noexcept;
  bool emit(RealtimeGameplayInputType type, int lane,
            std::optional<replay::LogicalControl> replayControl,
            std::int64_t timestampMicros, bool backSpin = false) noexcept;
  bool beginLane(FingerState &finger, std::size_t laneIndex,
                 const RealtimeTouchSample &sample) noexcept;
  bool releaseLane(FingerState &finger, std::int64_t timestampMicros,
                   bool backSpin = false) noexcept;
  bool handleScratchMove(FingerState &finger,
                         const RealtimeTouchSample &sample) noexcept;
  bool handleSpinScratchMove(FingerState &finger,
                             const RealtimeTouchSample &sample) noexcept;

  std::uint64_t epoch_ = 0;
  RealtimeTouchLayout layout_;
  RealtimeTouchInputSink sink_;
  bool legacyUniformLayout_ = false;
  bool gameplayEnabled_ = true;
  float spinScratchRotationDegrees_ = 0.0F;
  std::array<FingerState, kRealtimeTouchFingerCapacity> fingers_{};
};

} // namespace gameplay
