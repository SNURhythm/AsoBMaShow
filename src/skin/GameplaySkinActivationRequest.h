#pragma once

#include "SkinProfileSettings.h"
#include "package/SkinActivationCommitStore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace skin {

// The only package-to-gameplay handoff. The owning activation and its revision
// lease move into one chart-lifetime request; gameplay never prepares or
// commits package state itself.
struct GameplaySkinActivationRequest {
  std::uint64_t sessionSerial = 0;
  SkinProfileId profileId;
  ValidatedSkinActivation activation;
  ViewportSettings viewport;
};

// Built-in gameplay is a deliberate selection only when the chart trait has
// no selected skin. A selected skin that cannot be activated must remain
// distinguishable so gameplay does not silently replace it with built-in UI.
enum class GameplaySkinAcquisitionDisposition : std::uint8_t {
  BuiltIn,
  Ready,
  Failed,
};

struct GameplaySkinAcquisitionFailure {
  std::optional<SkinEntryId> entry;
  std::string revisionDigest;
  std::string configurationDigest;
  SkinDiagnostic diagnostic;
};

struct GameplaySkinAcquisition {
  GameplaySkinAcquisitionDisposition disposition =
      GameplaySkinAcquisitionDisposition::BuiltIn;
  std::optional<GameplaySkinActivationRequest> request;
  std::optional<GameplaySkinAcquisitionFailure> failure;

  // Transitional optional-like access keeps existing chart-boundary callers
  // source-compatible while making the no-selection / selected-failure state
  // explicit to new callers.
  [[nodiscard]] bool has_value() const noexcept { return request.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept {
    return has_value();
  }
  [[nodiscard]] GameplaySkinActivationRequest &operator*() noexcept {
    return *request;
  }
  [[nodiscard]] const GameplaySkinActivationRequest &operator*() const
      noexcept {
    return *request;
  }
  [[nodiscard]] GameplaySkinActivationRequest *operator->() noexcept {
    return request.operator->();
  }
  [[nodiscard]] const GameplaySkinActivationRequest *operator->() const
      noexcept {
    return request.operator->();
  }
};

using AcquireGameplaySkinForNextChart =
    std::function<GameplaySkinAcquisition(int keyMode)>;

} // namespace skin
