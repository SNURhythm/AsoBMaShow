#pragma once

#include "GameplaySkinSettingsController.h"
#include "../skin/beatoraja/SkinAcceptanceRecorder.h"

#include <functional>
#include <optional>
#include <string>

namespace skin {

struct SkinAcceptanceStartRequest {
  std::string opaqueRunId;
  std::string scenarioId;
};

struct GameplaySkinAcceptanceSnapshot {
  SkinAcceptanceCaptureState state = SkinAcceptanceCaptureState::Idle;
  std::optional<SkinAcceptanceExportTicket> exportTicket;
  std::optional<SkinAcceptanceExportResult> lastExport;
  std::string statusMessage;
};

[[nodiscard]] GameplaySkinAcceptanceSnapshot
projectGameplaySkinAcceptanceSnapshot(
    SkinAcceptanceCaptureState recorderState,
    std::optional<SkinAcceptanceExportTicket> exportTicket,
    SkinAcceptanceExportPollResult exportPoll);

using CurrentAcceptanceActivation =
    std::function<std::optional<SkinAcceptanceActivationKey>()>;

class GameplaySkinAcceptanceController final {
public:
  GameplaySkinAcceptanceController(SkinAcceptanceRecorder &,
                                   CurrentAcceptanceActivation);

  GameplaySkinAcceptanceController(const GameplaySkinAcceptanceController &) =
      delete;
  GameplaySkinAcceptanceController &
  operator=(const GameplaySkinAcceptanceController &) = delete;

  [[nodiscard]] const GameplaySkinAcceptanceSnapshot &snapshot() const noexcept;
  [[nodiscard]] ControllerActionResult start(SkinAcceptanceStartRequest);
  [[nodiscard]] ControllerActionResult stopAndExport();
  [[nodiscard]] ControllerActionResult acknowledgeLastExport();
  void poll();
  void close() noexcept;

private:
  SkinAcceptanceRecorder &recorder_;
  CurrentAcceptanceActivation currentActivation_;
  GameplaySkinAcceptanceSnapshot snapshot_;
  bool closed_ = false;
};

} // namespace skin
