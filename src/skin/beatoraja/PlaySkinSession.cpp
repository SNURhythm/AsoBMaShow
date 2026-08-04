#include "PlaySkinSession.h"

#include <string>
#include <utility>

namespace skin {
namespace {

class FrameDiscardGuard final {
public:
  explicit FrameDiscardGuard(PlaySkinStateBridge &bridge) noexcept
      : bridge_(&bridge) {}

  ~FrameDiscardGuard() {
    if (bridge_ != nullptr) {
      bridge_->discardFrame();
    }
  }

  void release() noexcept { bridge_ = nullptr; }

private:
  PlaySkinStateBridge *bridge_ = nullptr;
};

void appendDiagnostics(std::vector<SkinDiagnostic> &destination,
                       std::span<const SkinDiagnostic> source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

SkinDiagnostic transactionDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

} // namespace

PlaySkinSession::PlaySkinSession(PlaySkinSessionFrameContext context) noexcept
    : context_(context) {}

PlaySkinFrameTransactionResult PlaySkinSession::prepareFrame(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection,
    std::span<const SkinWriterInvocation> queuedWriters) {
  PlaySkinFrameTransactionResult result{.frameSerial = state.clock.serial};
  if (context_.sessionSerial == 0) {
    result.diagnostics.push_back(transactionDiagnostic(
        "skin.session.serial_invalid",
        "Gameplay skin session serial must be nonzero."));
    return result;
  }
  context_.bridge.beginFrame(state, projection);
  FrameDiscardGuard discard(context_.bridge);
  if (context_.bridge.frameSerial() != state.clock.serial ||
      state.clock.serial == 0) {
    appendDiagnostics(result.diagnostics, context_.bridge.diagnostics());
    return result;
  }

  const auto begun = context_.runtime.beginFrame(state.clock.serial);
  if (!begun.ok) {
    result.diagnostics.push_back(begun.failure.value_or(
        transactionDiagnostic("skin.session.frame.begin",
                              "Lua runtime rejected the session frame.")));
    return result;
  }
  SkinExternalFrameOwnership ownership(state.clock.serial,
                                       context_.sessionSerial);

  for (const auto &writer : queuedWriters) {
    // Pinned FloatWriter receives only the normalized value. eventMicros is
    // retained at this transaction boundary for later diagnostic/persistence
    // integration and never changes callback ordering.
    (void)writer.eventMicros;
    const auto invoked =
        context_.bridge.invokeWriter(writer.writer, writer.normalizedValue);
    if (invoked.status != SkinHostCallStatus::Completed) {
      appendDiagnostics(result.diagnostics, invoked.diagnostics);
      return result;
    }
  }

  const auto customObjects = context_.bridge.updateCustomObjects();
  if (customObjects.status != SkinHostCallStatus::Completed) {
    appendDiagnostics(result.diagnostics, customObjects.diagnostics);
    return result;
  }

  result.evaluation = context_.renderer.evaluateFrame(
      {.frameSerial = state.clock.serial,
       .sessionSerial = context_.sessionSerial,
       .visualTimeMicros = state.clock.visualTimeMicros,
       .model = context_.model,
       .configuration = context_.configuration,
       .resources = context_.resources,
       .viewport = context_.viewport,
       .runtime = context_.runtime,
       .state = context_.bridge,
       .gaugeRandomSource = context_.gaugeRandomSource},
      std::move(ownership));
  appendDiagnostics(result.diagnostics, context_.bridge.diagnostics());
  if (!result.evaluation.submitReady) {
    return result;
  }

  result.committed = context_.bridge.commitFrame();
  discard.release();
  if (result.committed.frameSerial != state.clock.serial) {
    result.committed = {};
    result.evaluation.submitReady.reset();
    result.evaluation.interactionLayout.reset();
    result.diagnostics.push_back(transactionDiagnostic(
        "skin.session.frame.commit",
        "Bridge commit did not match the evaluated frame serial."));
    return result;
  }
  result.outcome = PlaySkinFrameTransactionOutcome::Ready;
  return result;
}

} // namespace skin
