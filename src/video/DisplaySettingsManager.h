#pragma once

#include "FrameCapRuntime.h"
#include "../settings/AudioVideoSettings.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace display {
struct Resolution {
  int width = 0;
  int height = 0;
  int refreshRateHz = 0;
  bool operator==(const Resolution &) const = default;
};

struct DisplayInfo {
  int index = 0;
  std::string name;
  std::vector<Resolution> resolutions;
  bool operator==(const DisplayInfo &) const = default;
};

struct Capabilities {
  bool canChangeMode = false;
  bool canSelectDisplay = false;
  bool canSelectResolution = false;
  bool canChangeVsync = false;
  bool canSetFrameCap = true;
  std::vector<DisplayInfo> displays;
  bool operator==(const Capabilities &) const = default;
};

struct RuntimeState {
  player_settings::VideoSettings settings;
  int windowX = 0;
  int windowY = 0;
  std::uint32_t sdlWindowFlags = 0;
  std::uint32_t bgfxResetFlags = 0;
  int exclusiveRefreshRateHz = 0;
  std::uint32_t exclusivePixelFormat = 0;
  bool windowMaximized = false;
  bool operator==(const RuntimeState &) const = default;
};

enum class RestoreStatus { Restored, RetryableFailure, Failed };

class IDisplayBackend {
public:
  virtual ~IDisplayBackend() = default;
  virtual Capabilities capabilities() const = 0;
  virtual void observeRuntimeState() const {}
  virtual RuntimeState capture() const = 0;
  virtual bool apply(const player_settings::VideoSettings &,
                     std::string &errorMessage) = 0;
  virtual RestoreStatus restore(const RuntimeState &,
                                std::string &errorMessage) = 0;
};

enum class RollbackReason { Timeout, FocusLost, Cancelled, ApplyFailed };

enum class ApplyStatus {
  Applied,
  PreviewPending,
  Unsupported,
  FailedRolledBack,
  RollbackPending,
  FailedUnrecoverable,
};

struct ApplyResult {
  ApplyStatus status = ApplyStatus::Unsupported;
  player_settings::VideoSettings effective;
  std::string message;
};

class DisplaySettingsManager {
public:
  static constexpr std::chrono::seconds kConfirmationTimeout{15};

  DisplaySettingsManager(IDisplayBackend &, IFrameCapRuntime &,
                         player_settings::VideoSettings configuredIntent);
  ~DisplaySettingsManager();

  Capabilities capabilities() const;
  const player_settings::VideoSettings &configuredIntent() const;
  const player_settings::VideoSettings &lastWorkingSettings() const;
  ApplyResult applySafeStartupIntent();
  ApplyResult beginPreview(const player_settings::VideoSettings &,
                           std::chrono::steady_clock::time_point now);
  ApplyResult confirmPreview();
  ApplyResult cancelPreview(RollbackReason);
  ApplyResult shutdown();
  std::optional<ApplyResult> tick(std::chrono::steady_clock::time_point now);
  std::optional<ApplyResult> onFocusLost();
  bool hasPendingPreview() const;

private:
  struct PendingPreview {
    RuntimeState previous;
    player_settings::VideoSettings candidate;
    std::chrono::steady_clock::time_point deadline;
    std::optional<RollbackReason> rollbackReason;
  };

  std::optional<std::string>
  unsupportedReason(const player_settings::VideoSettings &,
                    const player_settings::VideoSettings &effective) const;
  static bool displayFieldsEqual(const player_settings::VideoSettings &,
                                 const player_settings::VideoSettings &);
  bool applyFrameCap(std::uint32_t, std::string &errorMessage);
  ApplyResult rollback(const RuntimeState &, RollbackReason,
                       std::string applyError = {});
  player_settings::VideoSettings captureEffectiveSettings() const;

  IDisplayBackend &backend;
  IFrameCapRuntime &frameCapRuntime;
  Capabilities backendCapabilities;
  player_settings::VideoSettings persistedIntent;
  player_settings::VideoSettings lastWorkingIntent;
  std::optional<PendingPreview> pendingPreview;
};
} // namespace display
