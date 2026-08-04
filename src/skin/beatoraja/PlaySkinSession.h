#pragma once

#include "PlaySkinStateBridge.h"
#include "Skin2DRenderer.h"

#include <cstdint>
#include <span>
#include <vector>

namespace skin {

struct SkinWriterInvocation {
  SkinFloatWriterId writer{};
  double normalizedValue = 0.0;
  long long eventMicros = 0;
};

struct PlaySkinSessionFrameContext {
  std::uint64_t sessionSerial = 0;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  const SkinPreparedResourceView &resources;
  const PlaySkinViewport &viewport;
  LuaSkinRuntime &runtime;
  PlaySkinStateBridge &bridge;
  Skin2DRenderer &renderer;
  ISkinGaugeRandomSource *gaugeRandomSource = nullptr;
};

enum class PlaySkinFrameTransactionOutcome : std::uint8_t {
  Rejected,
  Ready,
};

// Entirely value-owned publication. A later coordinator may submit the
// evaluation and enqueue the committed mutations without retaining any frame
// state, projection, callback, or input span borrowed by prepareFrame.
struct PlaySkinFrameTransactionResult {
  std::uint64_t frameSerial = 0;
  PlaySkinFrameTransactionOutcome outcome =
      PlaySkinFrameTransactionOutcome::Rejected;
  SkinFrameEvaluationResult evaluation;
  PlaySkinFrameCommit committed;
  std::vector<SkinDiagnostic> diagnostics;

  [[nodiscard]] bool ready() const noexcept {
    return outcome == PlaySkinFrameTransactionOutcome::Ready &&
           evaluation.submitReady.has_value() &&
           committed.frameSerial == frameSerial && frameSerial != 0;
  }
};

// Narrow frame-transaction core. Activation/resource ownership and
// coordinator submission are deliberately added by later Task 21 slices.
class PlaySkinSession final {
public:
  explicit PlaySkinSession(PlaySkinSessionFrameContext) noexcept;

  [[nodiscard]] PlaySkinFrameTransactionResult prepareFrame(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &,
      std::span<const SkinWriterInvocation> queuedWriters);

private:
  PlaySkinSessionFrameContext context_;
};

} // namespace skin
