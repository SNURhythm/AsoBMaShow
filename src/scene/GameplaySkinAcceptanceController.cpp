#include "GameplaySkinAcceptanceController.h"

#include <cstddef>
#include <exception>
#include <string_view>
#include <utility>

namespace skin {
namespace {

ControllerActionResult rejected(std::string message) {
  return {.message = std::move(message)};
}

ControllerActionResult accepted(std::string message,
                                bool asynchronous = false) {
  return {.accepted = true,
          .asynchronous = asynchronous,
          .message = std::move(message)};
}

bool isLowerHex(std::string_view value, std::size_t requiredLength) noexcept {
  if (value.size() != requiredLength) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool validActivation(const SkinAcceptanceActivationKey &key) noexcept {
  return !key.profileId.opaque.empty() &&
         !key.entry.package.directoryName.empty() &&
         !key.entry.package.collisionKey.empty() &&
         !key.entry.packageRelativePath.empty() &&
         !key.entry.collisionKey.empty() &&
         isLowerHex(key.revisionDigest, 64) &&
         isLowerHex(key.configurationDigest, 64);
}

const char *statusMessage(SkinAcceptanceCaptureState state) noexcept {
  switch (state) {
  case SkinAcceptanceCaptureState::Idle:
    return "Acceptance recording is idle.";
  case SkinAcceptanceCaptureState::Armed:
    return "Acceptance recording is armed.";
  case SkinAcceptanceCaptureState::WarmingUp:
    return "Acceptance recording is warming up.";
  case SkinAcceptanceCaptureState::Recording:
    return "Acceptance recording is in progress.";
  case SkinAcceptanceCaptureState::Exporting:
    return "Acceptance export is pending.";
  case SkinAcceptanceCaptureState::Exported:
    return "Acceptance export is ready for acknowledgement.";
  case SkinAcceptanceCaptureState::Failed:
    return "Acceptance recording failed; its export result is retained.";
  }
  return "Acceptance recording state is unavailable.";
}

} // namespace

GameplaySkinAcceptanceSnapshot projectGameplaySkinAcceptanceSnapshot(
    SkinAcceptanceCaptureState recorderState,
    std::optional<SkinAcceptanceExportTicket> exportTicket,
    SkinAcceptanceExportPollResult exportPoll) {
  GameplaySkinAcceptanceSnapshot snapshot{
      .state = recorderState,
      .exportTicket = exportTicket,
  };
  if (!exportTicket) {
    snapshot.statusMessage = statusMessage(recorderState);
    return snapshot;
  }

  switch (exportPoll.state) {
  case SkinAcceptanceExportPollState::Unknown:
    snapshot.statusMessage = "Acceptance export ticket is no longer available.";
    break;
  case SkinAcceptanceExportPollState::Pending:
    snapshot.state = SkinAcceptanceCaptureState::Exporting;
    snapshot.statusMessage = "Acceptance export is pending.";
    break;
  case SkinAcceptanceExportPollState::Ready:
    if (!exportPoll.result) {
      snapshot.state = SkinAcceptanceCaptureState::Failed;
      snapshot.statusMessage =
          "Acceptance export completed without a terminal result.";
      break;
    }
    snapshot.lastExport = std::move(exportPoll.result);
    snapshot.state = snapshot.lastExport->exported
                         ? SkinAcceptanceCaptureState::Exported
                         : SkinAcceptanceCaptureState::Failed;
    snapshot.statusMessage =
        snapshot.lastExport->exported
            ? "Acceptance export is ready."
            : "Acceptance export completed with failure evidence.";
    break;
  }
  return snapshot;
}

GameplaySkinAcceptanceController::GameplaySkinAcceptanceController(
    SkinAcceptanceRecorder &recorder,
    CurrentAcceptanceActivation currentActivation)
    : recorder_(recorder), currentActivation_(std::move(currentActivation)) {
  poll();
}

const GameplaySkinAcceptanceSnapshot &
GameplaySkinAcceptanceController::snapshot() const noexcept {
  return snapshot_;
}

ControllerActionResult
GameplaySkinAcceptanceController::start(SkinAcceptanceStartRequest request) {
  poll();
  if (closed_) {
    return rejected("Gameplay skin acceptance settings are closed.");
  }
  if (snapshot_.exportTicket ||
      snapshot_.state != SkinAcceptanceCaptureState::Idle) {
    return rejected("A prior gameplay skin acceptance run must finish and be "
                    "acknowledged.");
  }

  std::optional<SkinAcceptanceActivationKey> activation;
  try {
    if (currentActivation_) {
      activation = currentActivation_();
    }
  } catch (...) {
    return rejected("The current gameplay skin activation is unavailable.");
  }
  if (!activation || !validActivation(*activation)) {
    return rejected("Select a valid gameplay skin activation before starting.");
  }
  if (!recorder_.arm(std::move(request.opaqueRunId),
                     std::move(request.scenarioId), std::move(*activation))) {
    poll();
    return rejected("Gameplay skin acceptance recording could not be armed.");
  }
  poll();
  return accepted("Gameplay skin acceptance recording is armed.");
}

ControllerActionResult GameplaySkinAcceptanceController::stopAndExport() {
  poll();
  if (closed_) {
    return rejected("Gameplay skin acceptance settings are closed.");
  }
  if (snapshot_.exportTicket ||
      snapshot_.state == SkinAcceptanceCaptureState::Idle) {
    return rejected("No gameplay skin acceptance run can be exported.");
  }

  try {
    const auto ticket = recorder_.beginStopAndExport();
    poll();
    if (!ticket || !snapshot_.exportTicket) {
      return rejected("Gameplay skin acceptance export could not be started.");
    }
  } catch (...) {
    poll();
    return rejected("Gameplay skin acceptance export could not be started.");
  }
  return accepted("Gameplay skin acceptance export started.", true);
}

ControllerActionResult
GameplaySkinAcceptanceController::acknowledgeLastExport() {
  poll();
  if (closed_) {
    return rejected("Gameplay skin acceptance settings are closed.");
  }
  if (!snapshot_.exportTicket) {
    return rejected("No gameplay skin acceptance export is available.");
  }
  if (!snapshot_.lastExport) {
    return rejected("Gameplay skin acceptance export is not ready yet.");
  }
  if (!recorder_.acknowledgeExport(*snapshot_.exportTicket)) {
    poll();
    return rejected(
        "Gameplay skin acceptance export could not be acknowledged.");
  }
  poll();
  return accepted("Gameplay skin acceptance export acknowledged.");
}

void GameplaySkinAcceptanceController::poll() {
  recorder_.pollAsyncDependencies();
  const auto exportTicket = recorder_.currentExportTicket();
  if (!exportTicket) {
    snapshot_ = projectGameplaySkinAcceptanceSnapshot(recorder_.state(),
                                                      std::nullopt, {});
    return;
  }

  auto exportPoll = recorder_.pollExport(*exportTicket);
  const auto resampledState = recorder_.state();
  snapshot_ = projectGameplaySkinAcceptanceSnapshot(
      resampledState, exportTicket, std::move(exportPoll));
}

void GameplaySkinAcceptanceController::close() noexcept { closed_ = true; }

} // namespace skin
