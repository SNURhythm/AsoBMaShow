#include "PlaySkinSession.h"

#include "GameplaySkinBuiltinCatalog.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinTableDecoder.h"
#include "SkinModelValidator.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"
#include "../../replay/ReplayKeyMode.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace skin {
namespace {

class DeterministicGaugeRandomSource final : public ISkinGaugeRandomSource {
public:
  explicit DeterministicGaugeRandomSource(std::uint64_t seed) noexcept
      : seed_(seed) {}

  std::optional<std::uint32_t>
  next(SkinObjectId object, std::uint64_t animationEpoch,
       std::uint32_t exclusiveUpperBound) override {
    if (exclusiveUpperBound == 0) {
      return std::nullopt;
    }
    std::uint64_t value =
        seed_ ^ (static_cast<std::uint64_t>(object) << 32U) ^ animationEpoch;
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::uint32_t>(value % exclusiveUpperBound);
  }

private:
  std::uint64_t seed_ = 0;
};

std::uint64_t identitySeed(const PlaySkinSessionIdentity &identity) noexcept {
  std::uint64_t result = 1469598103934665603ULL ^ identity.sessionSerial;
  const auto append = [&result](std::string_view text) {
    for (const unsigned char byte : text) {
      result ^= byte;
      result *= 1099511628211ULL;
    }
  };
  append(identity.profileId.opaque);
  append(identity.entry.package.directoryName);
  append(identity.entry.package.collisionKey);
  append(identity.entry.packageRelativePath);
  append(identity.revisionDigest);
  append(identity.configurationDigest);
  return result;
}

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

SkinDiagnostic sessionDiagnostic(std::string code, std::string message,
                                 std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

void appendFailure(std::vector<SkinDiagnostic> &diagnostics,
                   std::optional<SkinDiagnostic> failure,
                   std::string_view fallbackCode,
                   std::string_view fallbackMessage) {
  if (failure) {
    diagnostics.push_back(std::move(*failure));
  } else {
    diagnostics.push_back(sessionDiagnostic(std::string(fallbackCode),
                                            std::string(fallbackMessage)));
  }
}

void appendMovedDiagnostics(std::vector<SkinDiagnostic> &destination,
                            std::vector<SkinDiagnostic> &source) {
  destination.insert(destination.end(),
                     std::make_move_iterator(source.begin()),
                     std::make_move_iterator(source.end()));
}

bool hasErrors(std::span<const SkinDiagnostic> diagnostics) {
  return std::ranges::any_of(diagnostics, [](const SkinDiagnostic &diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

bool cancelled(std::stop_token stop, PlaySkinSessionCreateResult &result) {
  if (!stop.stop_requested()) {
    return false;
  }
  result.cancelled = true;
  return true;
}

void advanceRevision(std::uint64_t &revision) noexcept {
  revision = revision == std::numeric_limits<std::uint64_t>::max()
                 ? 1
                 : revision + 1;
}

SkinDiagnostic firstFailureDiagnostic(
    const PlaySkinFrameTransactionResult &transaction,
    std::string fallbackCode, std::string fallbackMessage) {
  const auto firstError = [](const auto &diagnostics)
      -> std::optional<SkinDiagnostic> {
    const auto found = std::ranges::find_if(
        diagnostics, [](const SkinDiagnostic &diagnostic) {
          return diagnostic.severity == DiagnosticSeverity::Error;
        });
    return found == diagnostics.end()
               ? std::nullopt
               : std::optional<SkinDiagnostic>{*found};
  };
  if (auto diagnostic = firstError(transaction.diagnostics)) {
    return std::move(*diagnostic);
  }
  if (auto diagnostic = firstError(transaction.evaluation.diagnostics)) {
    return std::move(*diagnostic);
  }
  return transactionDiagnostic(std::move(fallbackCode),
                               std::move(fallbackMessage));
}

PresentationFailure presentationFailure(
    const PlaySkinSessionIdentity &identity, std::uint64_t frameSerial,
    SkinDiagnostic diagnostic) {
  return {.entry = identity.entry,
          .revisionDigest = identity.revisionDigest,
          .configurationDigest = identity.configurationDigest,
          .diagnostic = std::move(diagnostic),
          .frameSerial = frameSerial};
}

bool finiteRect(const AuthoredRect &rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width > 0.0 && rect.height > 0.0;
}

} // namespace

struct PlaySkinSession::OwnedActivation final {
  OwnedActivation(SkinRevisionLease revisionValue,
                  PlaySkinSessionIdentity identityValue,
                  const PlayfieldChartVisualModel &chartModelValue,
                  EntryProfileSettings reconciledSettingsValue,
                  ValidatedBeatorajaSkinModel modelValue,
                  BeatorajaSkinConfiguration configurationValue,
                  std::unique_ptr<LuaSkinRuntime> runtimeValue,
                  std::unique_ptr<SkinResourceCatalog> resourcesValue,
                  SkinConfigurationWriteQueue &configurationWritesValue,
                  ViewportSettings viewportSettingsValue,
                  UiLogicalRect safeUiBoundsValue,
                  PlaySkinViewport viewportValue)
      : revision(std::move(revisionValue)),
        identity(std::move(identityValue)), chartModel(&chartModelValue),
        reconciledSettings(std::move(reconciledSettingsValue)),
        model(std::move(modelValue)),
        configuration(std::move(configurationValue)),
        mutationTable(makePinnedSkinEventMutationTableV1()),
        runtime(std::move(runtimeValue)), resources(std::move(resourcesValue)),
        configurationWrites(&configurationWritesValue),
        viewportSettings(viewportSettingsValue),
        safeUiBounds(safeUiBoundsValue), viewport(viewportValue),
        gaugeRandom(std::make_unique<DeterministicGaugeRandomSource>(
            identitySeed(identity))),
        bridge(std::make_unique<PlaySkinStateBridge>(PlaySkinStateBridgeContext{
            .chartModel = *chartModel,
            .model = &model,
            .configuration = configuration,
            .runtime = *runtime,
            .mutationTable = mutationTable})) {}

  // The master revision is declared first and therefore released only after
  // the runtime/filesystem, resource catalog clone, and every borrowed frame
  // consumer have been destroyed.
  SkinRevisionLease revision;
  PlaySkinSessionIdentity identity;
  // Non-owning by PlaySkinSessionContext contract: immutable and guaranteed
  // to outlive this complete activation/session ownership graph.
  const PlayfieldChartVisualModel *chartModel = nullptr;
  EntryProfileSettings reconciledSettings;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  SkinEventMutationTable mutationTable;
  std::unique_ptr<LuaSkinRuntime> runtime;
  std::unique_ptr<SkinResourceCatalog> resources;
  SkinConfigurationWriteQueue *configurationWrites = nullptr;
  ViewportSettings viewportSettings;
  UiLogicalRect safeUiBounds;
  PlaySkinViewport viewport;
  Skin2DRenderer renderer;
  rendering::SkinQuadBatchRenderer quadRenderer;
  std::unique_ptr<ISkinGaugeRandomSource> gaugeRandom;
  std::unique_ptr<PlaySkinStateBridge> bridge;
};

#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
PlaySkinSession::PlaySkinSession(PlaySkinSessionFrameContext context) noexcept
    : owned_(), context_(std::move(context)),
      touchLayoutRevision_(context_.sessionSerial == 0
                               ? 1
                               : context_.sessionSerial),
      touchHitRegionsRevision_(touchLayoutRevision_) {
  context_.identity.sessionSerial = context_.sessionSerial;
}
#endif

PlaySkinSession::PlaySkinSession(
    std::unique_ptr<OwnedActivation> owned)
    : owned_(std::move(owned)),
      context_{.sessionSerial = owned_->identity.sessionSerial,
               .identity = owned_->identity,
               .chartModel = *owned_->chartModel,
               .model = owned_->model,
               .configuration = owned_->configuration,
               .resources = *owned_->resources,
               .viewportSettings = owned_->viewportSettings,
               .viewport = owned_->viewport,
               .runtime = *owned_->runtime,
               .bridge = *owned_->bridge,
               .renderer = owned_->renderer,
               .quadRenderer = owned_->quadRenderer,
               .configurationWrites = *owned_->configurationWrites,
               .gaugeRandomSource = owned_->gaugeRandom.get()},
      touchLayoutRevision_(context_.sessionSerial == 0
                               ? 1
                               : context_.sessionSerial),
      touchHitRegionsRevision_(touchLayoutRevision_) {
  context_.identity.sessionSerial = context_.sessionSerial;
}

PlaySkinSession::~PlaySkinSession() = default;

PlaySkinSessionCreateResult
PlaySkinSession::create(ValidatedSkinActivation activation,
                        PlaySkinSessionContext context) {
  PlaySkinSessionCreateResult result;
  activation.reconciledSettings.viewport = context.viewport;
  result.reconciledSettings = activation.reconciledSettings;

  if (context.sessionSerial == 0) {
    result.diagnostics.push_back(sessionDiagnostic(
        "skin.session.serial_invalid",
        "Gameplay skin session serial must be nonzero."));
    return result;
  }
  if (context.initialState == nullptr || context.initialProjection == nullptr) {
    result.diagnostics.push_back(sessionDiagnostic(
        "skin.session.initial_state_missing",
        "Gameplay skin configuration requires an initialized authoritative "
        "playfield state and projection."));
    return result;
  }
  if (context.initialState->clock.serial == 0 ||
      context.initialProjection->frameSerial !=
          context.initialState->clock.serial) {
    result.diagnostics.push_back(sessionDiagnostic(
        "skin.session.initial_state_invalid",
        "Gameplay skin configuration requires matching nonzero initial "
        "playfield state and projection serials."));
    return result;
  }
  if (cancelled(context.stop, result)) {
    return result;
  }
  if (!context.liveResourceCounters) {
    result.diagnostics.push_back(sessionDiagnostic(
        "skin.session.live_resource_counters_missing",
        "Gameplay skin session requires application-owned resource "
        "accounting."));
    return result;
  }
  if (activation.entry.package != activation.revision.revision().package) {
    result.diagnostics.push_back(sessionDiagnostic(
        "skin.session.activation_package_mismatch",
        "Gameplay skin entry and revision package identities do not match."));
    return result;
  }

  try {
    const SkinRevisionReadView revision = activation.revision.readView();
    auto runtimeFiles = LuaSkinFileSystem::create(
        {.revision = revision,
         .entry = activation.entry,
         .storageRoots = context.storageRoots,
         .profileId = context.profileId,
         .allowDataWrites = true});
    if (!runtimeFiles.fileSystem) {
      result.diagnostics.push_back(sessionDiagnostic(
          "skin_lua_filesystem_create_failed",
          runtimeFiles.failure
              ? runtimeFiles.failure->message
              : "Lua gameplay skin filesystem could not be created",
          runtimeFiles.failure ? runtimeFiles.failure->virtualPath : ""));
      return result;
    }
    if (cancelled(context.stop, result)) {
      return result;
    }

    auto runtime = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(runtimeFiles.fileSystem)});
    if (!runtime.runtime) {
      appendFailure(result.diagnostics, std::move(runtime.failure),
                    "skin_lua_runtime_create_failed",
                    "Lua gameplay runtime could not be created");
      return result;
    }

    LuaSkinTableDecoder decoder;
    auto headerValue = runtime.runtime->loadHeader();
    if (!headerValue.value) {
      appendFailure(result.diagnostics, std::move(headerValue.failure),
                    "skin_lua_header_load_failed",
                    "Lua gameplay skin header could not be loaded");
      return result;
    }
    auto decodedHeader = decoder.decodeHeader(*headerValue.value);
    appendMovedDiagnostics(result.diagnostics, decodedHeader.diagnostics);
    headerValue.value.reset();
    if (!decodedHeader.header || hasErrors(result.diagnostics) ||
        cancelled(context.stop, result)) {
      return result;
    }

    // Reconciliation/resource reads use an equivalent non-writing view while
    // the gameplay runtime exclusively owns its filesystem for both Lua load
    // phases and later profile-isolated data access.
    auto resourceFiles = LuaSkinFileSystem::create(
        {.revision = revision,
         .entry = activation.entry,
         .storageRoots = context.storageRoots,
         .profileId = context.profileId});
    if (!resourceFiles.fileSystem) {
      result.diagnostics.push_back(sessionDiagnostic(
          "skin_lua_filesystem_create_failed",
          resourceFiles.failure
              ? resourceFiles.failure->message
              : "Lua gameplay skin resource filesystem could not be created",
          resourceFiles.failure ? resourceFiles.failure->virtualPath : ""));
      return result;
    }

    auto reconciliation = reconcileSkinConfiguration(
        *decodedHeader.header, &activation.reconciledSettings,
        *resourceFiles.fileSystem);
    appendMovedDiagnostics(result.diagnostics, reconciliation.diagnostics);
    if (!reconciliation.configuration || hasErrors(result.diagnostics) ||
        cancelled(context.stop, result)) {
      return result;
    }
    result.reconciledSettings = reconciliation.reconciledSettings;
    BeatorajaSkinConfiguration configuration =
        std::move(*reconciliation.configuration);
    result.configurationDigest = configuration.lowercaseSha256;
    if (result.configurationDigest.empty() ||
        result.configurationDigest != skinConfigurationDigest(configuration)) {
      result.diagnostics.push_back(sessionDiagnostic(
          "skin_lua_configuration_digest_invalid",
          "Reconciled Lua gameplay skin configuration has an inconsistent "
          "digest."));
      return result;
    }
    if (result.configurationDigest != activation.configurationDigest) {
      result.diagnostics.push_back(sessionDiagnostic(
          "skin.session.configuration_digest_mismatch",
          "Fresh gameplay skin configuration does not match the validated "
          "activation digest."));
      return result;
    }

    SkinEventMutationTable configuredMutationTable =
        makePinnedSkinEventMutationTableV1();
    PlaySkinStateBridge configuredStateBridge({
        .chartModel = context.chartModel,
        .model = nullptr,
        .configuration = configuration,
        .runtime = *runtime.runtime,
        .mutationTable = configuredMutationTable,
    });
    configuredStateBridge.beginFrame(*context.initialState,
                                     *context.initialProjection);
    if (configuredStateBridge.frameSerial() !=
        context.initialState->clock.serial) {
      appendDiagnostics(result.diagnostics, configuredStateBridge.diagnostics());
      configuredStateBridge.discardFrame();
      if (!hasErrors(result.diagnostics)) {
        result.diagnostics.push_back(sessionDiagnostic(
            "skin.session.initial_state_invalid",
            "Gameplay skin configuration could not bind its initialized "
            "authoritative state."));
      }
      return result;
    }
    auto configuredValue = runtime.runtime->loadConfigured(configuration);
    appendDiagnostics(result.diagnostics, configuredStateBridge.diagnostics());
    configuredStateBridge.discardFrame();
    if (!configuredValue.value) {
      appendFailure(result.diagnostics, std::move(configuredValue.failure),
                    "skin_lua_configured_load_failed",
                    "Lua gameplay skin configured phase could not be loaded");
      return result;
    }
    if (cancelled(context.stop, result)) {
      return result;
    }
    const SkinBuiltinBindingCatalogView builtins =
        gameplaySkinBuiltinCatalog();
    auto decodedModel = decoder.decodeGameplay(
        *configuredValue.value,
        {.runtime = *runtime.runtime, .builtins = builtins});
    appendMovedDiagnostics(result.diagnostics, decodedModel.diagnostics);
    configuredValue.value.reset();
    if (!decodedModel.model || hasErrors(result.diagnostics) ||
        cancelled(context.stop, result)) {
      return result;
    }

    SkinModelValidator modelValidator;
    auto validatedModel = modelValidator.validate(
        std::move(*decodedModel.model),
        {.builtins = builtins,
         .callbacks = runtime.runtime->callbackLiveness()});
    appendMovedDiagnostics(result.diagnostics, validatedModel.diagnostics);
    if (!validatedModel.model || hasErrors(result.diagnostics) ||
        cancelled(context.stop, result)) {
      return result;
    }

    const std::vector<std::string> runtimeStrings =
        context.chartModel.runtimeStrings();
    auto planned = context.resourcePreparation.decodeAndPlan(
        {.revision = activation.revision.clone(),
         .entry = activation.entry,
         .fileSystem = *resourceFiles.fileSystem,
         .model = *validatedModel.model,
         .configuration = configuration,
         .requiredRuntimeStrings = runtimeStrings,
         .stop = context.stop});
    appendMovedDiagnostics(result.diagnostics, planned.diagnostics);
    if (planned.cancelled || cancelled(context.stop, result)) {
      result.cancelled = true;
      return result;
    }
    if (!planned.plan || hasErrors(result.diagnostics)) {
      return result;
    }

    auto uploaded = SkinResourceCatalog::upload(std::move(*planned.plan),
                                                context.textureDevice,
                                                context.liveResourceCounters);
    appendMovedDiagnostics(result.diagnostics, uploaded.diagnostics);
    if (!uploaded.catalog || hasErrors(result.diagnostics)) {
      return result;
    }
    if (cancelled(context.stop, result)) {
      return result;
    }

    auto renderPhase = runtime.runtime->enterRenderPhase();
    if (!renderPhase.ok) {
      appendFailure(result.diagnostics, std::move(renderPhase.failure),
                    "skin_lua_render_phase_failed",
                    "Lua gameplay skin could not enter render phase");
      return result;
    }
    uploaded.catalog->enterRenderPhase();

    const auto &header = validatedModel.model->model.header;
    const PlaySkinViewport viewport = evaluatePlaySkinViewport(
        {.width = static_cast<double>(header.width),
         .height = static_cast<double>(header.height)},
        context.safeUiBounds, context.viewport);
    if (!viewport.valid) {
      result.diagnostics.push_back(sessionDiagnostic(
          "skin.session.viewport_invalid",
          "Gameplay skin viewport could not be evaluated."));
      return result;
    }
    if (cancelled(context.stop, result)) {
      return result;
    }

    PlaySkinSessionIdentity identity{
        .sessionSerial = context.sessionSerial,
        .profileId = context.profileId,
        .entry = activation.entry,
        .revisionDigest =
            activation.revision.revision().lowercaseSha256,
        .configurationDigest = result.configurationDigest};
    auto owned = std::make_unique<OwnedActivation>(
        std::move(activation.revision), std::move(identity),
        context.chartModel, result.reconciledSettings,
        std::move(*validatedModel.model), std::move(configuration),
        std::move(runtime.runtime), std::move(uploaded.catalog),
        context.configurationWrites,
        context.viewport, context.safeUiBounds, viewport);
    result.session.reset(new PlaySkinSession(std::move(owned)));
    return result;
  } catch (...) {
    result.session.reset();
    result.diagnostics.push_back(sessionDiagnostic(
        "skin.session.create_failed",
        "Lua gameplay skin session creation failed closed."));
    return result;
  }
}

const PlaySkinSessionIdentity &PlaySkinSession::identity() const noexcept {
  return context_.identity;
}

PlaySkinFrameTransactionResult PlaySkinSession::runFrameTransaction(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection,
    std::span<const SkinWriterInvocation> queuedWriters,
    std::span<const SkinEventInvocation> queuedEvents) {
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

  for (const auto &event : queuedEvents) {
    // SkinObject.mousePressed dispatches the image event on primary pointer
    // down. Touch delivery is frame-bound here, so preserve its single signed
    // argument at the next valid transaction boundary.
    const std::array<int, 1> arguments{event.argument};
    const auto invoked = context_.bridge.invokeEventBinding(
        SkinEventBindingId{event.eventBinding}, arguments);
    if (!invoked.ok()) {
      appendDiagnostics(result.diagnostics, invoked.diagnostics);
      return result;
    }
  }

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
       .visualTimeMicros = skinStateClockMicros(state),
       .model = context_.model,
       .configuration = context_.configuration,
       .resources = context_.resources,
       .viewport = context_.viewport,
       .runtime = context_.runtime,
       .state = context_.bridge,
       .markProcessedNotes = state.configuration.markProcessedNotes,
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

PresentationFrameOutcome PlaySkinSession::preparePendingFrame(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection,
    std::span<const SkinWriterInvocation> queuedWriters,
    std::span<const SkinEventInvocation> queuedEvents,
    std::span<const SkinFrameMutation> extraMutations) {
  if (pendingFrame_) {
    return PresentationFrameOutcome::CriticalFailure;
  }

  auto transaction = runFrameTransaction(state, projection, queuedWriters,
                                         queuedEvents);
  if (transaction.ready() && !extraMutations.empty()) {
    try {
      transaction.committed.orderedMutations.insert(
          transaction.committed.orderedMutations.end(),
          extraMutations.begin(), extraMutations.end());
    } catch (...) {
      transaction.outcome = PlaySkinFrameTransactionOutcome::Rejected;
      transaction.evaluation.submitReady.reset();
      transaction.evaluation.interactionLayout.reset();
      transaction.committed = {};
      transaction.diagnostics.push_back(transactionDiagnostic(
          "skin.session.frame.test_mutation_copy",
          "Test mutation values could not be retained by the pending frame."));
    }
  }
  const bool ready = transaction.ready();
  pendingFrame_.emplace(PendingFrame{.transaction = std::move(transaction)});
  return ready ? PresentationFrameOutcome::Ready
               : PresentationFrameOutcome::CriticalFailure;
}

PresentationFrameOutcome PlaySkinSession::prepareFrame(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection) {
  if (pendingFrame_) {
    return PresentationFrameOutcome::CriticalFailure;
  }
  const std::size_t writerCount = queuedWriterCount_;
  const std::size_t eventCount = queuedEventCount_;
  queuedWriterCount_ = 0;
  queuedEventCount_ = 0;
  return preparePendingFrame(
      state, projection,
      std::span<const SkinWriterInvocation>{queuedWriters_.data(),
                                            writerCount},
      std::span<const SkinEventInvocation>{queuedEvents_.data(), eventCount});
}

#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
PlaySkinFrameTransactionResult PlaySkinSession::prepareFrame(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection,
    std::span<const SkinWriterInvocation> queuedWriters) {
  return runFrameTransaction(state, projection, queuedWriters);
}

PresentationFrameOutcome PlaySkinSession::prepareFrameForTesting(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection,
    std::span<const SkinWriterInvocation> queuedWriters,
    std::span<const SkinFrameMutation> extraMutations) {
  return preparePendingFrame(state, projection, queuedWriters,
                             {}, extraMutations);
}
#endif

PresentationFrameResult PlaySkinSession::render(
    RenderContext &renderContext, const PreparedGameplayBgaFrame &preparedBga,
    IGameplayBgaSubmitter &bgaSubmitter) {
  if (!pendingFrame_) {
    const auto diagnostic = transactionDiagnostic(
        "skin.session.frame.not_prepared",
        "Gameplay skin render requires one pending prepared frame.");
    return {.outcome = PresentationFrameOutcome::CriticalFailure,
            .submittedMode = PresentationMode::BuiltIn,
            .bgaCompositeMode =
                GameplayBgaCompositeMode::FullscreenBuiltIn,
            .failure = presentationFailure(identity(), 0, diagnostic)};
  }

  // Consume before any fallible preflight/submit work. Re-entry and repeat
  // render can therefore never submit or enqueue this frame again.
  PendingFrame pending = std::move(*pendingFrame_);
  pendingFrame_.reset();
  auto &transaction = pending.transaction;
  PresentationFrameResult result{
      .frameSerial = transaction.frameSerial,
      .outcome = PresentationFrameOutcome::CriticalFailure,
      .submittedMode = PresentationMode::BuiltIn,
      .bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn,
      .preparedBga = preparedBga};

  if (!transaction.ready()) {
    clearPublishedGeometry(true);
    result.failure = presentationFailure(
        identity(), transaction.frameSerial,
        firstFailureDiagnostic(transaction,
                               "skin.session.frame.evaluation_failed",
                               "Gameplay skin frame evaluation failed."));
    return result;
  }

  static_assert(
      std::is_nothrow_move_constructible_v<PresentationFrameResult>);
  static_assert(std::is_nothrow_move_assignable_v<
                std::optional<SkinInteractionLayout>>);

  enum class PersistencePreparationFailure : std::uint8_t {
    None,
    Copy,
    Request,
  };
  PersistencePreparationFailure persistenceFailure =
      PersistencePreparationFailure::None;
  std::optional<SkinConfigurationWriteRequest> configurationRequest;
  std::optional<PresentationFrameResult> copyFailureResult;
  std::optional<PresentationFrameResult> requestFailureResult;
  std::optional<PresentationFrameResult> queueFullResult;
  std::optional<PresentationFrameResult> queueClosedResult;

  const bool hasPersistedWrites = std::ranges::any_of(
      transaction.committed.orderedMutations, [](const auto &mutation) {
        return std::holds_alternative<PersistedSkinConfigurationWrite>(
            mutation);
      });
  if (hasPersistedWrites) {
    const auto recoverablePersistenceResult =
        [&](std::string code, std::string message) {
          return PresentationFrameResult{
              .frameSerial = transaction.frameSerial,
              .outcome = PresentationFrameOutcome::RecoverableFailure,
              .submittedMode = PresentationMode::Skin,
              .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin,
              .preparedBga = preparedBga,
              .failure = presentationFailure(
                  identity(), transaction.frameSerial,
                  transactionDiagnostic(std::move(code),
                                        std::move(message)))};
        };

    // The first payload is the allocation-free escape hatch for every later
    // preparation failure. If even it cannot be retained, fail before any
    // renderer commit and return the already value-owned built-in result.
    try {
      copyFailureResult.emplace(recoverablePersistenceResult(
          "skin.session.configuration_write_copy_failed",
          "Persisted skin writes could not be retained before drawing."));
    } catch (...) {
      clearPublishedGeometry(true);
      return result;
    }

    try {
      requestFailureResult.emplace(recoverablePersistenceResult(
          "skin.session.configuration_write_request_failed",
          "Persisted skin write identity could not be retained before "
          "drawing."));
      queueFullResult.emplace(recoverablePersistenceResult(
          "skin.session.configuration_write_queue_full",
          "Persisted skin write queue is full after drawing."));
      queueClosedResult.emplace(recoverablePersistenceResult(
          "skin.session.configuration_write_queue_closed",
          "Persisted skin write queue is closed after drawing."));
    } catch (...) {
      persistenceFailure = PersistencePreparationFailure::Copy;
    }

    std::vector<PersistedSkinConfigurationWrite> persistedWrites;
    if (persistenceFailure == PersistencePreparationFailure::None) {
      try {
        persistedWrites.reserve(
            transaction.committed.orderedMutations.size());
        for (auto &mutation : transaction.committed.orderedMutations) {
          if (auto *persistedWrite =
                  std::get_if<PersistedSkinConfigurationWrite>(&mutation)) {
            persistedWrites.emplace_back(std::move(*persistedWrite));
          }
          // SessionPresentationWrite is consumed by this one-shot
          // publication and has no independent durable authority.
        }
      } catch (...) {
        persistenceFailure = PersistencePreparationFailure::Copy;
      }
    }

    if (persistenceFailure == PersistencePreparationFailure::None) {
      try {
        configurationRequest.emplace(SkinConfigurationWriteRequest{
            .sessionSerial = identity().sessionSerial,
            .profileId = identity().profileId,
            .entry = identity().entry,
            .expectedRevisionDigest = identity().revisionDigest,
            .expectedConfigurationDigest = identity().configurationDigest,
            .frameSerial = transaction.frameSerial,
            .orderedWrites = std::move(persistedWrites)});
      } catch (...) {
        persistenceFailure = PersistencePreparationFailure::Request;
      }
    }
  }

  // Stage interaction geometry before the renderer's commit point. MSVC's
  // Debug STL can allocate an iterator-debug proxy while moving a vector even
  // when the value type reports a nothrow move; doing this here keeps every
  // potentially allocating operation ahead of authored GPU submission.
  if (transaction.evaluation.interactionLayout) {
    transaction.evaluation.interactionLayout->revision =
        touchLayoutRevision_;
    publishedLayout_ =
        std::move(transaction.evaluation.interactionLayout);
  } else {
    publishedLayout_.reset();
    captures_.fill({});
  }

  // The prepared-BGA overload is noexcept and returns false only before its
  // atomic commit point. Session code must not turn a post-commit exception
  // into built-in fallback; the renderer boundary prevents one escaping.
  const bool submitted = context_.renderer.submit(
      *transaction.evaluation.submitReady, context_.resources,
      renderContext, context_.quadRenderer, preparedBga, bgaSubmitter);
  if (!submitted) {
    clearPublishedGeometry(true);
    result.failure = presentationFailure(
        identity(), transaction.frameSerial,
        transactionDiagnostic(
            "skin.session.frame.submit_failed",
            "Gameplay skin frame preflight or submission failed."));
    return result;
  }

  publishedReplayGhostGeometry_ =
      std::move(transaction.evaluation.syntheticReplayGhostGeometry);

  advanceRevision(touchHitRegionsRevision_);

  result.outcome = PresentationFrameOutcome::Ready;
  result.submittedMode = PresentationMode::Skin;
  result.bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin;

  if (persistenceFailure == PersistencePreparationFailure::Copy) {
    return std::move(*copyFailureResult);
  }
  if (persistenceFailure == PersistencePreparationFailure::Request) {
    return std::move(*requestFailureResult);
  }
  if (!configurationRequest) {
    return result;
  }

  const auto enqueued = context_.configurationWrites.enqueue(
      std::move(*configurationRequest));
  if (enqueued == SkinConfigurationEnqueueResult::Enqueued) {
    return result;
  }
  return enqueued == SkinConfigurationEnqueueResult::QueueFull
             ? std::move(*queueFullResult)
             : std::move(*queueClosedResult);
}

void PlaySkinSession::submitSyntheticReplayGhosts(
    RenderContext &renderContext, const SyntheticReplayGhostFrameInput &input) {
  if (!publishedReplayGhostGeometry_ ||
      publishedReplayGhostGeometry_->frameSerial != input.frameSerial) {
    return;
  }
  const SkinCommandBuffer overlay =
      buildSyntheticReplayGhostOverlay(*publishedReplayGhostGeometry_, input);
  if (overlay.commands.empty()) {
    return;
  }
  (void)context_.renderer.submitOverlay(overlay, context_.resources,
                                        renderContext, context_.quadRenderer);
}

void PlaySkinSession::clearPublishedGeometry(bool advanceTopology) noexcept {
  captures_.fill({});
  publishedLayout_.reset();
  publishedReplayGhostGeometry_.reset();
  queuedWriterCount_ = 0;
  queuedEventCount_ = 0;
  if (advanceTopology) {
    advanceRevision(touchLayoutRevision_);
  }
  advanceRevision(touchHitRegionsRevision_);
}

void PlaySkinSession::setViewport(ViewportSettings settings) {
  context_.viewportSettings = settings;
  if (owned_) {
    owned_->viewportSettings = settings;
  }
  updateViewportGeometry(context_.viewport.safeUiBounds);
}

void PlaySkinSession::updateViewportGeometry(UiLogicalRect safeUiBounds) {
  pendingFrame_.reset();
  const auto &header = context_.model.model.header;
  context_.viewport = evaluatePlaySkinViewport(
      {.width = static_cast<double>(header.width),
       .height = static_cast<double>(header.height)},
      safeUiBounds, context_.viewportSettings);
  if (owned_) {
    owned_->safeUiBounds = safeUiBounds;
    owned_->viewport = context_.viewport;
  }
  clearPublishedGeometry(true);
}

gameplay::RealtimeTouchLayout PlaySkinSession::touchLayout() const {
  gameplay::RealtimeTouchLayout result;
  result.revision = touchLayoutRevision_;
  result.keyMode = context_.chartModel.keyCount;
  if (!publishedLayout_ || !context_.viewport.valid ||
      context_.chartModel.laneOrder.empty() || rendering::window_width <= 0 ||
      rendering::window_height <= 0 || rendering::render_width <= 0 ||
      rendering::render_height <= 0 ||
      !std::isfinite(rendering::ui_scale_x) ||
      !std::isfinite(rendering::ui_scale_y) || rendering::ui_scale_x <= 0.0F ||
      rendering::ui_scale_y <= 0.0F) {
    return result;
  }
  const auto &safe = context_.viewport.safeUiBounds;
  if (!std::isfinite(safe.x) || !std::isfinite(safe.y) ||
      !std::isfinite(safe.width) || !std::isfinite(safe.height) ||
      safe.width <= 0.0 || safe.height <= 0.0) {
    return result;
  }
  const auto normalizedPoint = [&](double authoredX,
                                   double authoredY)
      -> std::optional<gameplay::RealtimeTouchPoint> {
    const auto &affine = context_.viewport.authoredToUi;
    const double uiX = affine.m00 * authoredX + affine.m01 * authoredY +
                       affine.tx;
    const double uiY = affine.m10 * authoredX + affine.m11 * authoredY +
                       affine.ty;
    const double x =
        (uiX * static_cast<double>(rendering::ui_scale_x) +
         static_cast<double>(rendering::ui_offset_x)) /
        static_cast<double>(rendering::render_width);
    const double y =
        (uiY * static_cast<double>(rendering::ui_scale_y) +
         static_cast<double>(rendering::ui_offset_y)) /
        static_cast<double>(rendering::render_height);
    if (!std::isfinite(x) || !std::isfinite(y)) {
      return std::nullopt;
    }
    return gameplay::RealtimeTouchPoint{.x = static_cast<float>(x),
                                        .y = static_cast<float>(y)};
  };

  const auto keyLayout = replay::replayKeyModeLayout(result.keyMode);
  try {
    result.lanes.reserve(context_.chartModel.laneOrder.size());
    result.scratch.reserve(context_.chartModel.laneOrder.size());
    result.laneRegions.reserve(context_.chartModel.laneOrder.size());
    for (const int chartLane : context_.chartModel.laneOrder) {
      const auto found = std::ranges::find_if(
          publishedLayout_->laneRegions,
          [chartLane](const SkinLaneInteractionRegion &region) {
            return region.authoredLane == chartLane;
          });
      if (found == publishedLayout_->laneRegions.end() ||
          !finiteRect(found->authoredRegion)) {
        return gameplay::RealtimeTouchLayout{
            .revision = touchLayoutRevision_, .keyMode = result.keyMode};
      }
      const auto &rect = found->authoredRegion;
      const auto bottomLeft = normalizedPoint(rect.x, rect.y);
      const auto bottomRight = normalizedPoint(rect.x + rect.width, rect.y);
      const auto topLeft = normalizedPoint(rect.x, rect.y + rect.height);
      const auto topRight =
          normalizedPoint(rect.x + rect.width, rect.y + rect.height);
      if (!bottomLeft || !bottomRight || !topLeft || !topRight) {
        return gameplay::RealtimeTouchLayout{
            .revision = touchLayoutRevision_, .keyMode = result.keyMode};
      }
      const bool scratch =
          keyLayout && keyLayout->hasDirectionalScratch &&
          (chartLane == 7 || chartLane == 15);
      result.lanes.push_back(chartLane);
      result.scratch.push_back(scratch);
      result.laneRegions.push_back({.bottomLeft = *bottomLeft,
                                    .bottomRight = *bottomRight,
                                    .topLeft = *topLeft,
                                    .topRight = *topRight,
                                    .lane = chartLane,
                                    .scratch = scratch});
    }
  } catch (...) {
    return gameplay::RealtimeTouchLayout{
        .revision = touchLayoutRevision_, .keyMode = result.keyMode};
  }
  result.laneCount = result.laneRegions.size();
  if (!result.laneRegions.empty()) {
    result.bottomLeft = result.laneRegions.front().bottomLeft;
    result.topLeft = result.laneRegions.front().topLeft;
    result.bottomRight = result.laneRegions.back().bottomRight;
    result.topRight = result.laneRegions.back().topRight;
  }
  return result;
}

std::uint64_t PlaySkinSession::touchLayoutRevision() const noexcept {
  return touchLayoutRevision_;
}

std::uint64_t PlaySkinSession::touchHitRegionsRevision() const noexcept {
  return touchHitRegionsRevision_;
}

std::vector<PresentationUiHitRegion>
PlaySkinSession::touchHitRegions() const {
  return publishedLayout_ ? publishedLayout_->uiHitRegions()
                          : std::vector<PresentationUiHitRegion>{};
}

PresentationUiHit
PlaySkinSession::hitTestUiControl(UiLogicalPoint point) const {
  return publishedLayout_ ? publishedLayout_->hitTestUiControl(point)
                          : PresentationUiHit{};
}

PresentationTouchResult PlaySkinSession::beginPresentationTouch(
    const PresentationTouchEvent &event) {
  if (!publishedLayout_ ||
      std::ranges::any_of(captures_, [&](const TouchCapture &capture) {
        return capture.active && capture.pointerId == event.pointerId;
      })) {
    return {};
  }
  auto capture = std::ranges::find_if(
      captures_, [](const TouchCapture &candidate) {
        return !candidate.active;
      });
  if (capture == captures_.end() ||
      publishedLayout_->hitTestUiControl(event.uiPoint) != event.hit) {
    return {};
  }
  if (const auto eventInvocation = publishedLayout_->eventInvocationFor(
          event.hit, event.uiPoint, event.eventMicros)) {
    if (queuedEventCount_ == queuedEvents_.size()) {
      return {};
    }
    queuedEvents_[queuedEventCount_++] = *eventInvocation;
  } else {
    const auto writerInvocation = publishedLayout_->writerInvocationFor(
        event.hit, event.uiPoint, event.eventMicros);
    if (!writerInvocation || queuedWriterCount_ == queuedWriters_.size()) {
      return {};
    }
    queuedWriters_[queuedWriterCount_++] = *writerInvocation;
  }
  *capture = {.pointerId = event.pointerId,
              .active = true,
              .hit = event.hit};
  return {.consumed = true, .excludeFromGameplay = true};
}

PresentationTouchResult PlaySkinSession::updatePresentationTouch(
    const PresentationTouchEvent &event) {
  if (!std::isfinite(event.uiPoint.x) || !std::isfinite(event.uiPoint.y)) {
    return {};
  }
  const auto capture = std::ranges::find_if(
      captures_, [&](const TouchCapture &candidate) {
        return candidate.active && candidate.pointerId == event.pointerId;
      });
  return capture != captures_.end() && capture->hit == event.hit
             ? PresentationTouchResult{.consumed = true,
                                       .excludeFromGameplay = true}
             : PresentationTouchResult{};
}

PresentationTouchResult PlaySkinSession::endPresentationTouch(
    const PresentationTouchEvent &event, bool cancelled) {
  (void)cancelled;
  const auto capture = std::ranges::find_if(
      captures_, [&](const TouchCapture &candidate) {
        return candidate.active && candidate.pointerId == event.pointerId;
      });
  if (capture == captures_.end()) {
    return {};
  }
  const bool matched = capture->hit == event.hit &&
                       std::isfinite(event.uiPoint.x) &&
                       std::isfinite(event.uiPoint.y);
  *capture = {};
  return matched ? PresentationTouchResult{.consumed = true,
                                           .excludeFromGameplay = true}
                 : PresentationTouchResult{};
}

void PlaySkinSession::cancelPresentationTouches(long long eventMicros) {
  (void)eventMicros;
  captures_.fill({});
}

void PlaySkinSession::onLanePressed(int lane, JudgeResult judge,
                                    long long eventMicros) {
  (void)lane;
  (void)judge;
  (void)eventMicros;
}

void PlaySkinSession::onLaneReleased(int lane, long long eventMicros) {
  (void)lane;
  (void)eventMicros;
}

void PlaySkinSession::onJudge(JudgeResult judge, int combo, int score,
                              PlayfieldJudgeEventClock clock,
                              bool recordTimingSample) {
  (void)judge;
  (void)combo;
  (void)score;
  (void)clock;
  (void)recordTimingSample;
}

} // namespace skin
