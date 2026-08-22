#include "GameplaySkinSessionFactory.h"

#include "../../skin/beatoraja/BgfxSkinTextureDevice.h"
#include "../../skin/beatoraja/PlaySkinSession.h"
#include "../../rendering/common.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace skin {
namespace {

std::string_view gdxKeyName(SDL_Scancode scancode) noexcept {
  switch (scancode) {
  case SDL_SCANCODE_LALT:
    return "L-Alt";
  case SDL_SCANCODE_RALT:
    return "R-Alt";
  case SDL_SCANCODE_LSHIFT:
    return "L-Shift";
  case SDL_SCANCODE_RSHIFT:
    return "R-Shift";
  case SDL_SCANCODE_LCTRL:
    return "L-Ctrl";
  case SDL_SCANCODE_RCTRL:
    return "R-Ctrl";
  case SDL_SCANCODE_BACKSPACE:
    return "Delete";
  case SDL_SCANCODE_DELETE:
    return "Forward Delete";
  case SDL_SCANCODE_RETURN:
  case SDL_SCANCODE_RETURN2:
  case SDL_SCANCODE_KP_ENTER:
    return "Enter";
  case SDL_SCANCODE_PAGEUP:
    return "Page Up";
  case SDL_SCANCODE_PAGEDOWN:
    return "Page Down";
  case SDL_SCANCODE_KP_PLUS:
    return "Plus";
  case SDL_SCANCODE_APPLICATION:
    return "Menu";
  case SDL_SCANCODE_AC_BACK:
    return "Back";
  case SDL_SCANCODE_AUDIOPLAY:
    return "Play/Pause";
  case SDL_SCANCODE_AUDIOSTOP:
    return "Stop Media";
  case SDL_SCANCODE_AUDIONEXT:
    return "Next Media";
  case SDL_SCANCODE_AUDIOPREV:
    return "Prev Media";
  case SDL_SCANCODE_AUDIOREWIND:
    return "Rewind";
  case SDL_SCANCODE_AUDIOFASTFORWARD:
    return "Fast Forward";
  case SDL_SCANCODE_AUDIOMUTE:
    return "Mute";
  case SDL_SCANCODE_KP_0:
    return "Numpad 0";
  case SDL_SCANCODE_KP_1:
    return "Numpad 1";
  case SDL_SCANCODE_KP_2:
    return "Numpad 2";
  case SDL_SCANCODE_KP_3:
    return "Numpad 3";
  case SDL_SCANCODE_KP_4:
    return "Numpad 4";
  case SDL_SCANCODE_KP_5:
    return "Numpad 5";
  case SDL_SCANCODE_KP_6:
    return "Numpad 6";
  case SDL_SCANCODE_KP_7:
    return "Numpad 7";
  case SDL_SCANCODE_KP_8:
    return "Numpad 8";
  case SDL_SCANCODE_KP_9:
    return "Numpad 9";
  default:
    if (const char *name = SDL_GetScancodeName(scancode)) {
      return name;
    }
    return {};
  }
}

} // namespace

static LuaSkinLegacyInputSnapshot
captureLuaSkinLegacyInputSnapshot() noexcept {
  LuaSkinLegacyInputSnapshot result;
  try {
    result.drawableWidth = rendering::render_width;
    result.drawableHeight = rendering::render_height;

    int keyCount = 0;
    if (const Uint8 *keyboard = SDL_GetKeyboardState(&keyCount)) {
      for (int index = 0; index < keyCount; ++index) {
        if (keyboard[index] == 0) {
          continue;
        }
        result.anyKeyPressed = true;
        const int code = LuaSkinLegacyInputHost::keyCode(
            gdxKeyName(static_cast<SDL_Scancode>(index)));
        if (code >= 0) {
          result.pressedKeys.push_back(code);
        }
      }
    }

    const int joystickCount = std::max(0, SDL_NumJoysticks());
    result.controllers.reserve(static_cast<std::size_t>(joystickCount));
    for (int index = 0; index < joystickCount; ++index) {
      if (SDL_IsGameController(index) != SDL_TRUE) {
        continue;
      }
      std::unique_ptr<SDL_GameController, decltype(&SDL_GameControllerClose)>
          ownedController(nullptr, SDL_GameControllerClose);
      SDL_GameController *controller = nullptr;
      const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
      if (instance >= 0) {
        controller = SDL_GameControllerFromInstanceID(instance);
      }
      if (controller == nullptr) {
        ownedController.reset(SDL_GameControllerOpen(index));
        controller = ownedController.get();
      }
      if (controller == nullptr) {
        continue;
      }
      LuaSkinLegacyControllerSnapshot captured;
      if (const char *name = SDL_GameControllerName(controller)) {
        captured.name = name;
      }
      for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
        if (SDL_GameControllerGetButton(
                controller, static_cast<SDL_GameControllerButton>(button)) !=
            0) {
          captured.pressedButtons.push_back(button);
        }
      }
      result.controllers.push_back(std::move(captured));
    }
  } catch (...) {
    return {};
  }
  return result;
}

} // namespace skin

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
        .textureDevice = std::make_shared<skin::BgfxSkinTextureDevice>(),
        .httpTransport = services.createHttpTransport
                             ? services.createHttpTransport(services.stop)
                             : nullptr,
        .audioBackend = std::move(services.audioBackend),
        .captureLegacyInputSnapshot =
            services.captureLegacyInputSnapshot
                ? std::move(services.captureLegacyInputSnapshot)
                : skin::captureLuaSkinLegacyInputSnapshot,
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
