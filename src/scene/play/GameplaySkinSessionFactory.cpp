#include "GameplaySkinSessionFactory.h"

#include "../../skin/beatoraja/BgfxSkinTextureDevice.h"
#include "../../skin/beatoraja/PlaySkinSession.h"
#include "../../rendering/common.h"

#include <optional>
#include <string>
#include <utility>

namespace {

void appendSessionDiagnostic(skin::SkinDiagnosticHistory *history,
                             const skin::SkinEntryId &entry,
                             std::string revisionDigest,
                             std::string configurationDigest,
                             skin::SkinDiagnostic diagnostic) noexcept {
  if (history == nullptr) {
    return;
  }
  try {
    const std::optional<std::uint32_t> luaLine =
        diagnostic.source && diagnostic.source->line != 0
            ? std::optional<std::uint32_t>(diagnostic.source->line)
            : std::nullopt;
    history->append({.entry = entry,
                     .revisionDigest = std::move(revisionDigest),
                     .configurationDigest = std::move(configurationDigest),
                     .phase = skin::SkinDiagnosticPhase::Session,
                     .diagnostic = std::move(diagnostic),
                     .luaLine = luaLine,
                     .frameSerial = std::nullopt});
  } catch (...) {
  }
}

skin::SkinDiagnostic unavailableDiagnostic(std::string code,
                                           std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = skin::DiagnosticSeverity::Error};
}

GameplaySkinSessionResult failedResult(skin::SkinDiagnosticHistory *history,
                                       skin::SkinEntryId entry,
                                       std::string revisionDigest,
                                       std::string configurationDigest,
                                       skin::SkinDiagnostic diagnostic,
                                       bool diagnosticAlreadyRecorded = false) {
  if (!diagnosticAlreadyRecorded) {
    appendSessionDiagnostic(history, entry, revisionDigest, configurationDigest,
                            diagnostic);
  }
  return {.disposition = GameplaySkinSessionDisposition::Failed,
          .session = {},
          .failure = PresentationFailure{
              .entry = std::move(entry),
              .revisionDigest = std::move(revisionDigest),
              .configurationDigest = std::move(configurationDigest),
              .diagnostic = std::move(diagnostic)}};
}

} // namespace

GameplaySkinSessionResult
createGameplaySkinSession(GameplaySkinSessionServices services,
                          GameplaySkinSessionInput input) {
  if (!services.acquire) {
    return {};
  }

  skin::GameplaySkinAcquisition acquisition;
  try {
    acquisition = services.acquire(input.keyMode);
  } catch (...) {
    return failedResult(
        services.diagnosticHistory, {}, {}, {},
        unavailableDiagnostic("skin.lifecycle.acquisition_exception",
                              "Gameplay skin lifecycle acquisition threw."));
  }

  if (acquisition.disposition ==
          skin::GameplaySkinAcquisitionDisposition::BuiltIn &&
      !acquisition.request && !acquisition.failure) {
    return {};
  }
  if (acquisition.disposition ==
      skin::GameplaySkinAcquisitionDisposition::Failed) {
    const auto failure =
        acquisition.failure.value_or(skin::GameplaySkinAcquisitionFailure{
            .diagnostic = unavailableDiagnostic(
                "skin.lifecycle.acquisition_invalid",
                "The selected gameplay skin returned no activation.")});
    return failedResult(services.diagnosticHistory,
                        failure.entry.value_or(skin::SkinEntryId{}),
                        failure.revisionDigest, failure.configurationDigest,
                        failure.diagnostic);
  }
  if (acquisition.disposition !=
          skin::GameplaySkinAcquisitionDisposition::Ready ||
      !acquisition.request) {
    return failedResult(
        services.diagnosticHistory, {}, {}, {},
        unavailableDiagnostic(
            "skin.lifecycle.acquisition_invalid",
            "The selected gameplay skin returned no activation."));
  }

  skin::GameplaySkinActivationRequest request = std::move(*acquisition.request);
  const skin::SkinEntryId entry = request.activation.entry;
  const std::string revisionDigest =
      request.activation.revision.revision().lowercaseSha256;
  const std::string configurationDigest =
      request.activation.configurationDigest;
  if (services.storageRoots == nullptr ||
      services.resourcePreparation == nullptr ||
      !services.liveResourceCounters ||
      services.configurationWrites == nullptr ||
      services.diagnosticHistory == nullptr) {
    return failedResult(
        services.diagnosticHistory, entry, revisionDigest, configurationDigest,
        unavailableDiagnostic("skin.session.services_unavailable",
                              "Gameplay skin services are unavailable. Return "
                              "to Settings, then try "
                              "the selected skin again."));
  }
  if (input.chartModel == nullptr) {
    return failedResult(
        services.diagnosticHistory, entry, revisionDigest, configurationDigest,
        unavailableDiagnostic("skin.session.chart_model_missing",
                              "Gameplay skin session requires a chart model."));
  }

  try {
    auto captureLegacyInputGeneration =
        std::move(services.captureLegacyInputGeneration);
    if (!captureLegacyInputGeneration) {
      captureLegacyInputGeneration = [] {
        skin::LuaSkinLegacyInputGeneration generation;
        generation.drawableWidth = rendering::render_width;
        generation.drawableHeight = rendering::render_height;
        return generation;
      };
    }
    skin::PlaySkinSessionContext context{
        .sessionSerial = request.sessionSerial,
        .profileId = std::move(request.profileId),
        .safetyPolicy = skin::SkinSafetyPolicy(request.safetyLevel),
        .chartModel = *input.chartModel,
        .initialState = input.initialState,
        .initialProjection = input.initialProjection,
        .viewport = request.viewport,
        .safeUiBounds = input.safeUiBounds,
        .storageRoots = *services.storageRoots,
        .resourcePreparation = *services.resourcePreparation,
        .builtinImageReader = std::move(services.builtinImageReader),
        .builtinImageBatchReader = std::move(services.builtinImageBatchReader),
        .builtinImageCache = services.builtinImageCache,
        .builtinImageCacheKey = services.builtinImageCacheKey,
        .textureDevice = std::make_shared<skin::BgfxSkinTextureDevice>(),
        .httpTransport = services.createHttpTransport
                             ? services.createHttpTransport(services.stop)
                             : nullptr,
        .audioBackend = std::move(services.audioBackend),
        .captureLegacyInputGeneration =
            std::move(captureLegacyInputGeneration),
        .liveResourceCounters = std::move(services.liveResourceCounters),
        .configurationWrites = *services.configurationWrites,
        .applyAudioVolume = std::move(services.applyAudioVolume),
        .applyPracticeItemScroll = std::move(services.applyPracticeItemScroll),
        .applyPracticeMenuItem = std::move(services.applyPracticeMenuItem),
        .applyPracticeVisibleItems =
            std::move(services.applyPracticeVisibleItems),
        .pinnedRuntimeSelection = std::move(input.pinnedRuntimeSelection),
        .stop = services.stop};
#if defined(ASOBMASHOW_GAMEPLAY_SKIN_SESSION_FACTORY_TESTING)
    auto created = services.createSessionForTesting
                       ? services.createSessionForTesting(
                             std::move(request.activation), std::move(context))
                       : skin::PlaySkinSession::create(
                             std::move(request.activation), std::move(context));
#else
    auto created = skin::PlaySkinSession::create(std::move(request.activation),
                                                 std::move(context));
#endif
    std::optional<skin::SkinDiagnostic> firstError;
    for (auto &diagnostic : created.diagnostics) {
      if (!firstError &&
          diagnostic.severity == skin::DiagnosticSeverity::Error) {
        firstError = diagnostic;
      }
      appendSessionDiagnostic(services.diagnosticHistory, entry, revisionDigest,
                              configurationDigest, std::move(diagnostic));
    }
    if (services.stop.stop_requested()) {
      return failedResult(
          services.diagnosticHistory, entry, revisionDigest,
          configurationDigest,
          unavailableDiagnostic("skin.session.preparation_cancelled",
                                "Gameplay skin preparation was cancelled."));
    }
    if (!created.session) {
      const skin::SkinDiagnostic diagnostic = firstError.value_or(
          unavailableDiagnostic("skin.session.construction_failed",
                                "The selected gameplay skin could not start a "
                                "chart-lifetime session."));
      return failedResult(services.diagnosticHistory, entry, revisionDigest,
                          configurationDigest, diagnostic,
                          firstError.has_value());
    }
    const auto runtimeSelection =
        created.session->runtimeConfigurationSelection();
    return {.disposition = GameplaySkinSessionDisposition::Ready,
            .session = std::move(created.session),
            .failure = std::nullopt,
            .runtimeSelection = std::move(runtimeSelection)};
  } catch (...) {
    return failedResult(
        services.diagnosticHistory, entry, revisionDigest, configurationDigest,
        unavailableDiagnostic("skin.session.construction_exception",
                              "The selected gameplay skin threw while "
                              "starting. Built-in gameplay was "
                              "not used as a replacement."));
  }
}
