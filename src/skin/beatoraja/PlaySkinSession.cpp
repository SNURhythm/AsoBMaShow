#include "PlaySkinSession.h"

#include "GameplaySkinBuiltinCatalog.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinTableDecoder.h"
#include "SkinModelValidator.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
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
        viewportSettings(viewportSettingsValue),
        safeUiBounds(safeUiBoundsValue), viewport(viewportValue),
        gaugeRandom(std::make_unique<DeterministicGaugeRandomSource>(
            identitySeed(identity))),
        bridge(std::make_unique<PlaySkinStateBridge>(PlaySkinStateBridgeContext{
            .chartModel = *chartModel,
            .model = model,
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
  ViewportSettings viewportSettings;
  UiLogicalRect safeUiBounds;
  PlaySkinViewport viewport;
  Skin2DRenderer renderer;
  std::unique_ptr<ISkinGaugeRandomSource> gaugeRandom;
  std::unique_ptr<PlaySkinStateBridge> bridge;
};

#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
PlaySkinSession::PlaySkinSession(PlaySkinSessionFrameContext context) noexcept
    : owned_(), context_(context) {}
#endif

PlaySkinSession::PlaySkinSession(
    std::unique_ptr<OwnedActivation> owned) noexcept
    : owned_(std::move(owned)),
      context_{.sessionSerial = owned_->identity.sessionSerial,
               .model = owned_->model,
               .configuration = owned_->configuration,
               .resources = *owned_->resources,
               .viewport = owned_->viewport,
               .runtime = *owned_->runtime,
               .bridge = *owned_->bridge,
               .renderer = owned_->renderer,
               .gaugeRandomSource = owned_->gaugeRandom.get()} {}

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
  if (cancelled(context.stop, result)) {
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

    auto configuredValue = runtime.runtime->loadConfigured(configuration);
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
                                                context.textureDevice);
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
  static const PlaySkinSessionIdentity empty;
  return owned_ ? owned_->identity : empty;
}

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
