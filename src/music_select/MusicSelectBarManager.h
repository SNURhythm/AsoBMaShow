#pragma once

#include "MusicSelectTypes.h"

#include <cstdint>
#include <string>
#include <vector>

struct MusicSelectBarManagerConfig {
  std::string modeFilter = "ALL";
  std::string difficultyFilter = "ALL";
  std::string sortId = "TITLE";
};

struct MusicSelectBarManagerSnapshot {
  std::vector<MusicSelectBar> rows;
  std::size_t selectedIndex = 0;
  std::vector<MusicSelectBarId> directory;
  std::string directoryText;
  int movementDirection = 0;
  std::int64_t movementEndMillis = 0;
  std::string resolvedModeFilter = "ALL";
  std::string resolvedDifficultyFilter = "ALL";
};

[[nodiscard]] int
musicSelectFirstExistingReplay(const MusicSelectBar *bar) noexcept;
[[nodiscard]] int musicSelectNextExistingReplay(const MusicSelectBar *bar,
                                                int selected) noexcept;
[[nodiscard]] std::string
musicSelectSelectedHash(const MusicSelectBar *bar, bool sha256);

class MusicSelectBarManager final {
public:
  explicit MusicSelectBarManager(MusicSelectProjection = {},
                                 MusicSelectBarManagerConfig = {});

  [[nodiscard]] bool openSelected();
  [[nodiscard]] bool openTransient(MusicSelectBar directory,
                                   std::vector<MusicSelectBar> children);
  [[nodiscard]] bool close();
  void move(bool increase, int movementDirection,
            std::int64_t movementEndMillis);
  void setSelectedPosition(float);
  void configure(MusicSelectBarManagerConfig);
  void refresh(MusicSelectProjection);
  [[nodiscard]] MusicSelectBarManagerSnapshot snapshot() const;

private:
  [[nodiscard]] const MusicSelectBar *selected() const;
  void rebuildRows(std::optional<MusicSelectBarId> preferred = std::nullopt);

  MusicSelectProjection projection_;
  std::vector<MusicSelectBarId> directory_;
  std::vector<MusicSelectBarId> sourceBars_;
  std::vector<MusicSelectBar> rows_;
  std::size_t selectedIndex_ = 0;
  int movementDirection_ = 0;
  std::int64_t movementEndMillis_ = 0;
  MusicSelectBarManagerConfig config_;
};
