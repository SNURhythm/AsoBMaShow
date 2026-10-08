#pragma once

namespace player_settings {

struct ReplayPreferences {
  int exportFps = 120;
  bool exportFullResolution = true;
  bool renderTouchPoints = true;
  bool renderGhosts = true;
  bool autoKeySound = false;

  bool operator==(const ReplayPreferences &) const = default;

  void sanitize() noexcept {
    if (exportFps != 60 && exportFps != 120) exportFps = 120;
  }
};

} // namespace player_settings
