#pragma once

enum class MusicSelectToolbarMode { Expanded, Collapsed, Hidden };

struct MusicSelectToolbarState {
  MusicSelectToolbarMode mode = MusicSelectToolbarMode::Expanded;
  float x = 0.0F;
  float y = 0.0F;
  bool hasPosition = false;

  bool operator==(const MusicSelectToolbarState &) const = default;
};

struct ApplicationUiState {
  static constexpr int kSchemaVersion = 1;
  MusicSelectToolbarState musicSelectToolbar;

  bool operator==(const ApplicationUiState &) const = default;
};
