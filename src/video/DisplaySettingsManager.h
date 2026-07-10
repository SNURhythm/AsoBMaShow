#pragma once

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
  bool operator==(const RuntimeState &) const = default;
};

class IDisplayBackend {
public:
  virtual ~IDisplayBackend() = default;
  virtual Capabilities capabilities() const = 0;
  virtual RuntimeState capture() const = 0;
  virtual bool apply(const player_settings::VideoSettings &,
                     std::string &errorMessage) = 0;
  virtual bool restore(const RuntimeState &, std::string &errorMessage) = 0;
};

enum class RollbackReason { Timeout, FocusLost, Cancelled, ApplyFailed };

enum class ApplyStatus {
  Applied,
  PreviewPending,
  Unsupported,
  FailedRolledBack,
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

  DisplaySettingsManager(IDisplayBackend &,
                         player_settings::VideoSettings initialSettings);
  ~DisplaySettingsManager();

  Capabilities capabilities() const;
  ApplyResult beginPreview(const player_settings::VideoSettings &,
                           std::chrono::steady_clock::time_point now);
  bool confirmPreview();
  ApplyResult cancelPreview(RollbackReason);
  std::optional<ApplyResult> tick(std::chrono::steady_clock::time_point now);
  void onFocusLost();
  bool hasPendingPreview() const;

private:
  struct PendingPreview {
    RuntimeState previous;
    player_settings::VideoSettings candidate;
    std::chrono::steady_clock::time_point deadline;
  };

  std::optional<std::string>
  unsupportedReason(const player_settings::VideoSettings &) const;
  static bool displayFieldsEqual(const player_settings::VideoSettings &,
                                 const player_settings::VideoSettings &);

  IDisplayBackend &backend;
  Capabilities backendCapabilities;
  player_settings::VideoSettings confirmedSettings;
  std::optional<PendingPreview> pendingPreview;
};
} // namespace display
