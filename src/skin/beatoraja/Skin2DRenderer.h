#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "BeatorajaSkinModel.h"
#include "LuaSkinRuntime.h"
#include "SkinDrawCommand.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

struct RenderContext;
struct PreparedGameplayBgaFrame;
class IGameplayBgaSubmitter;
namespace rendering {
class SkinQuadBatchRenderer;
}

namespace skin {

class PlaySkinSession;
class Skin2DRenderer;

// Move-only proof that PlaySkinSession has already opened the matching Lua
// callback frame. Only the session may mint it and only the renderer may
// consume it, preventing evaluator callers from expressing ambiguous frame
// ownership with a boolean flag.
class SkinExternalFrameOwnership final {
public:
  SkinExternalFrameOwnership(SkinExternalFrameOwnership &&) noexcept = default;
  SkinExternalFrameOwnership &
  operator=(SkinExternalFrameOwnership &&) noexcept = default;
  SkinExternalFrameOwnership(const SkinExternalFrameOwnership &) = delete;
  SkinExternalFrameOwnership &
  operator=(const SkinExternalFrameOwnership &) = delete;

private:
  SkinExternalFrameOwnership(std::uint64_t frameSerial,
                             std::uint64_t sessionSerial) noexcept
      : frameSerial_(frameSerial), sessionSerial_(sessionSerial) {}

  std::uint64_t frameSerial_ = 0;
  std::uint64_t sessionSerial_ = 0;
  bool consumed_ = false;

  friend class PlaySkinSession;
  friend class Skin2DRenderer;
};

template <typename T> struct SkinPropertyLookup {
  T value{};
  bool supported = false;
};

enum class SkinProjectedNoteKind : std::uint8_t {
  Normal,
  Invisible,
  Mine,
};

struct SkinProjectedNoteView {
  std::uint32_t visualId = 0;
  int lane = -1;
  SkinProjectedNoteKind kind = SkinProjectedNoteKind::Normal;
  // Live gameplay snapshots carry an abstract scroll delta plus the
  // LaneRenderer hispeed. Hand-authored/test projections leave this empty and
  // retain the historical pixel-displacement representation.
  std::optional<double> scrollSpeed;
  double authoredYDisplacement = 0.0;
  bool judged = false;
  std::uint32_t submissionOrdinal = 0;
};

enum class SkinProjectedLongNoteMode : std::uint8_t { LN, CN, HCN };

struct SkinProjectedLongNoteView {
  std::uint32_t headVisualId = 0;
  std::uint32_t tailVisualId = 0;
  int lane = -1;
  SkinProjectedLongNoteMode mode = SkinProjectedLongNoteMode::LN;
  std::optional<double> scrollSpeed;
  double headAuthoredYDisplacement = 0.0;
  double tailAuthoredYDisplacement = 0.0;
  bool active = false;
  bool damaged = false;
  bool reactive = false;
  bool headJudged = false;
  bool tailJudged = false;
  std::uint32_t submissionOrdinal = 0;
};

enum class SkinProjectedLineKind : std::uint8_t {
  Group,
  Bpm,
  Stop,
  Time,
};

struct SkinProjectedLineView {
  std::uint32_t timelineVisualId = 0;
  SkinProjectedLineKind kind = SkinProjectedLineKind::Time;
  std::optional<double> scrollSpeed;
  double authoredYDisplacement = 0.0;
  std::uint32_t submissionOrdinal = 0;
};

struct SkinGaugeStateView {
  bool supported = false;
  // Render-ready value. A result-screen bridge must apply Beatoraja's
  // start/end reveal curve before publishing this snapshot.
  double value = 0.0;
  int gaugeType = 0;
  double minimum = 0.0;
  double maximum = 100.0;
  double border = 0.0;
};

struct SkinJudgeStateView {
  bool supported = false;
  std::optional<int> optionalZeroBasedGrade;
  std::int64_t combo = 0;
  bool maximumGauge = false;
};

struct SkinNoteExpansionStateView {
  bool supported = false;
  // Render-ready elapsed milliseconds since the current quarter-note pulse.
  // The bridge owns chart/replay timing and Task 14 only applies Beatoraja's
  // fixed 9 ms expansion plus 150 ms contraction curve.
  std::int64_t elapsedSinceQuarterNoteMillis = 0;
};

class ISkinFrameState {
public:
  virtual ~ISkinFrameState() = default;
  virtual std::uint64_t frameSerial() const noexcept = 0;
  virtual SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) = 0;
  virtual SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain =
                      SkinIntegerPropertyDomain::IntegerValue) = 0;
  virtual SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain = SkinFloatPropertyDomain::Rate) = 0;
  virtual SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) = 0;
  virtual SkinPropertyLookup<ConfigOffset> offsetProperty(int) = 0;
  virtual std::int64_t timerProperty(const SkinBuiltinPropertySelector &) = 0;
  virtual std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept = 0;
  virtual std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept = 0;
  virtual std::span<const SkinProjectedLineView>
  projectedLines() const noexcept = 0;
  virtual SkinGaugeStateView gaugeState() const noexcept = 0;
  virtual SkinJudgeStateView judgeState(int player) const noexcept = 0;
  virtual SkinNoteExpansionStateView noteExpansionState() const noexcept = 0;
};

class ISkinGaugeRandomSource {
public:
  virtual ~ISkinGaugeRandomSource() = default;
  virtual std::optional<std::uint32_t>
  next(SkinObjectId object, std::uint64_t animationEpoch,
       std::uint32_t exclusiveUpperBound) = 0;
};

struct SkinCommandPolicy {
  static constexpr int maximumBeatorajaOffsetId = 199;
  static constexpr std::size_t maximumCommands = 131'072;
  static constexpr std::size_t maximumGlyphInstances = 65'536;
  static constexpr std::size_t maximumPrimitiveVertices = 524'288;
};

struct SkinFrameInputs {
  std::uint64_t frameSerial = 0;
  std::uint64_t sessionSerial = 0;
  std::int64_t visualTimeMicros = 0;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  const SkinPreparedResourceView &resources;
  const PlaySkinViewport &viewport;
  LuaSkinRuntime &runtime;
  ISkinFrameState &state;
  ISkinGaugeRandomSource *gaugeRandomSource = nullptr;
};

struct SkinSliderInteractionGeometry {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  PresentationUiControlKind kind = PresentationUiControlKind::Slider;
  // SkinSlider.mousePressed tests the prepared destination region, not the
  // rate-displaced knob rectangle.  Keep that base region and its directional
  // hit strip in authored coordinates so input routing remains render-free.
  AuthoredRect authoredDestination;
  AuthoredRect authoredHitRegion;
  AuthoredPoint valueZero;
  AuthoredPoint valueOne;
  std::uint8_t direction = 0;
  double range = 0.0;
  bool changeable = false;
  std::optional<SkinFloatWriterId> writer;
};

struct SkinImageInteractionGeometry {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  AuthoredRect authoredRegion;
  SkinEventBindingId event{};
  int clickMode = 0;
};

using SkinInteractionControl =
    std::variant<SkinSliderInteractionGeometry, SkinImageInteractionGeometry>;

struct SkinLaneInteractionRegion {
  SkinObjectId sourceObject = 0;
  int authoredLane = -1;
  AuthoredRect authoredRegion;
};

struct SkinLaneGroupInteractionRegion {
  SkinObjectId sourceObject = 0;
  std::size_t authoredGroup = 0;
  AuthoredRect authoredRegion;
};

// Immutable-by-publication frame snapshot.  Consumers receive this only after
// the matching command buffer has evaluated successfully; no callback or
// writer is retained or invoked by the geometry queue.
struct SkinInteractionLayout {
  std::uint64_t frameSerial = 0;
  // Stable for the lifetime of one immutable gameplay-skin session. A normal
  // frame publication advances frameSerial but retains this revision.
  std::uint64_t revision = 0;
  Affine2D uiToAuthored;
  UiLogicalRect safeUiBounds;
  std::vector<SkinSliderInteractionGeometry> slidersTopmostFirst;
  std::vector<SkinImageInteractionGeometry> imagesTopmostFirst;
  // Pinned Skin.mousePressed walks every visible SkinObject in reverse draw
  // order. This heterogeneous sequence retains that cross-object ordering.
  std::vector<SkinInteractionControl> controlsTopmostFirst;
  std::vector<SkinLaneInteractionRegion> laneRegions;
  std::vector<SkinLaneGroupInteractionRegion> laneGroupRegions;

  [[nodiscard]] std::optional<AuthoredPoint>
  authoredPointForUi(double x, double y) const noexcept;
  [[nodiscard]] PresentationUiHit
  hitTestUiControl(UiLogicalPoint point) const noexcept;
  [[nodiscard]] std::vector<PresentationUiHitRegion>
  uiHitRegions() const;
  [[nodiscard]] std::optional<SkinWriterInvocation>
  writerInvocationFor(const PresentationUiHit &hit, UiLogicalPoint point,
                      long long eventMicros) const noexcept;
  [[nodiscard]] std::optional<SkinEventInvocation>
  eventInvocationFor(const PresentationUiHit &hit, UiLogicalPoint point,
                     long long eventMicros) const noexcept;
};

struct SkinFrameEvaluationResult {
  std::optional<SkinCommandBuffer> submitReady;
  std::optional<SkinInteractionLayout> interactionLayout;
  std::vector<SkinDiagnostic> diagnostics;
};

class Skin2DRenderer final {
public:
  // Legacy adapter: existing standalone evaluators retain internal ownership
  // of LuaSkinRuntime::beginFrame until coordinator migration is complete.
  SkinFrameEvaluationResult evaluateFrame(const SkinFrameInputs &);
  // Session path: the typed token proves beginFrame already occurred, so
  // evaluation must not begin the runtime a second time.
  SkinFrameEvaluationResult evaluateFrame(
      const SkinFrameInputs &, SkinExternalFrameOwnership &&);
  [[nodiscard]] bool submit(const SkinCommandBuffer &,
                            const SkinResourceCatalog &, RenderContext &,
                            rendering::SkinQuadBatchRenderer &) const;
  // False is a pre-commit fallback decision. Once the BGA/quad commit point
  // is crossed, this boundary cannot unwind into coordinator fallback.
  [[nodiscard]] bool submit(
      const SkinCommandBuffer &, const SkinPreparedResourceView &,
      RenderContext &, rendering::SkinQuadBatchRenderer &,
      const PreparedGameplayBgaFrame &, IGameplayBgaSubmitter &) const
      noexcept;

private:
  SkinFrameEvaluationResult evaluateFrameImpl(const SkinFrameInputs &,
                                               bool beginRuntimeFrame);

  struct GaugeAnimationState {
    int animation = 0;
    std::int64_t deadlineMillis = 0;
    std::uint64_t epoch = 0;
  };

  std::uint64_t gaugeAnimationSessionSerial_ = 0;
  std::map<SkinObjectId, GaugeAnimationState> gaugeAnimationStates_;
  std::uint64_t externalOwnershipSessionSerial_ = 0;
  std::uint64_t lastExternalOwnershipFrameSerial_ = 0;
};

#if defined(ASOBMASHOW_SKIN_RENDERER_TESTING)
void resetSkinRendererLookupComparisonsForTesting() noexcept;
[[nodiscard]] std::size_t skinRendererLookupComparisonsForTesting() noexcept;
#endif

} // namespace skin
