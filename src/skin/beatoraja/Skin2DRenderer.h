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
#include <vector>

struct RenderContext;
namespace rendering {
class SkinQuadBatchRenderer;
}

namespace skin {

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
  integerProperty(const SkinBuiltinPropertySelector &) = 0;
  virtual SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &) = 0;
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
  static constexpr std::size_t maximumProjectedNotes = 32'768;
  static constexpr std::size_t maximumProjectedLongNotes = 32'768;
  static constexpr std::size_t maximumProjectedLines = 8'192;
  static constexpr std::size_t maximumProjectedElements = 73'728;
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

struct SkinFrameEvaluationResult {
  std::optional<SkinCommandBuffer> submitReady;
  std::vector<SkinDiagnostic> diagnostics;
};

class Skin2DRenderer final {
public:
  SkinFrameEvaluationResult evaluateFrame(const SkinFrameInputs &);
  [[nodiscard]] bool submit(const SkinCommandBuffer &,
                            const SkinResourceCatalog &, RenderContext &,
                            rendering::SkinQuadBatchRenderer &) const;

private:
  struct GaugeAnimationState {
    int animation = 0;
    std::int64_t deadlineMillis = 0;
    std::uint64_t epoch = 0;
  };

  std::uint64_t gaugeAnimationSessionSerial_ = 0;
  std::map<SkinObjectId, GaugeAnimationState> gaugeAnimationStates_;
};

#if defined(ASOBMASHOW_SKIN_RENDERER_TESTING)
void resetSkinRendererLookupComparisonsForTesting() noexcept;
[[nodiscard]] std::size_t skinRendererLookupComparisonsForTesting() noexcept;
#endif

} // namespace skin
