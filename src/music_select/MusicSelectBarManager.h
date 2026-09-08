#pragma once

#include "MusicSelectTypes.h"

#include <cstdint>
#include <string>
#include <span>
#include <unordered_map>
#include <vector>

struct MusicSelectBarManagerConfig {
  std::string modeFilter = "ALL";
  std::string difficultyFilter = "ALL";
  std::string sortId = "TITLE";
};

template <typename Rows> struct MusicSelectBarManagerState {
  Rows rows;
  std::size_t selectedIndex = 0;
  std::vector<MusicSelectBarId> directory;
  Rows directoryBars;
  std::string directoryText;
  int movementDirection = 0;
  std::int64_t movementEndMillis = 0;
  std::string resolvedModeFilter = "ALL";
  std::string resolvedDifficultyFilter = "ALL";
  std::shared_ptr<const std::vector<MusicSelectBar>> rowOwner;
  std::shared_ptr<const std::vector<MusicSelectBar>> directoryOwner;
  std::uint64_t rowsRevision = 0;
  std::shared_ptr<MusicSelectRowProvider> rowProvider;

  [[nodiscard]] std::size_t rowCount() const noexcept {
    return rowProvider ? rowProvider->size() : rows.size();
  }
  [[nodiscard]] const MusicSelectBar &rowAt(std::size_t index) const {
    return rowProvider ? rowProvider->at(index) : rows[index];
  }
  [[nodiscard]] bool rowsEmpty() const noexcept { return rowCount() == 0; }
};

using MusicSelectBarManagerSnapshot =
    MusicSelectBarManagerState<std::vector<MusicSelectBar>>;
using MusicSelectBarManagerReadView =
    MusicSelectBarManagerState<std::span<const MusicSelectBar>>;

struct MusicSelectTableContext {
  std::string name;
  std::string level;
  std::string fullName;
};

[[nodiscard]] int
musicSelectFirstExistingReplay(const MusicSelectBar *bar) noexcept;
[[nodiscard]] int musicSelectNextExistingReplay(const MusicSelectBar *bar,
                                                int selected) noexcept;
[[nodiscard]] std::string
musicSelectSelectedHash(const MusicSelectBar *bar, bool sha256);
[[nodiscard]] MusicSelectTableContext musicSelectTableContextForLaunch(
    const MusicSelectBarManagerReadView &);
[[nodiscard]] MusicSelectTableContext musicSelectTableContextForLaunch(
    const MusicSelectBarManagerSnapshot &);

[[nodiscard]] std::vector<MusicSelectBar> musicSelectProjectionChildren(
    const MusicSelectProjection &, const MusicSelectBarId &);

class MusicSelectBarManager final {
public:
  explicit MusicSelectBarManager(MusicSelectProjection = {},
                                 MusicSelectBarManagerConfig = {});

  [[nodiscard]] bool open(const MusicSelectBarId &);
  [[nodiscard]] bool openSelected();
  [[nodiscard]] bool installChildren(const MusicSelectBarId &,
                                     std::vector<MusicSelectBar>);
  [[nodiscard]] bool installRowProvider(const MusicSelectBarId &,
                                       std::shared_ptr<MusicSelectRowProvider>);
  void installFolderStatus(const MusicSelectBarId &,
                           const skin::MusicSelectBarFrame &);
  [[nodiscard]] bool openTransient(MusicSelectBar directory,
                                   std::vector<MusicSelectBar> children);
  [[nodiscard]] bool close();
  void move(bool increase, int movementDirection,
            std::int64_t movementEndMillis);
  [[nodiscard]] bool select(const MusicSelectBarId &);
  [[nodiscard]] std::vector<MusicSelectBar>
  childrenOf(const MusicSelectBarId &) const;
  void setSelectedPosition(float);
  void configure(MusicSelectBarManagerConfig);
  void refresh(MusicSelectProjection);
  [[nodiscard]] MusicSelectBarManagerSnapshot snapshot() const;
  [[nodiscard]] MusicSelectBarManagerReadView readView() const;
  [[nodiscard]] skin::MusicSelectSongListFrame songListFrame() const;

private:
  struct RowProviderState {
    std::shared_ptr<MusicSelectRowProvider> provider;
    std::optional<MusicSelectBarManagerConfig> configuration;
  };

  [[nodiscard]] std::size_t rowCount() const noexcept;
  [[nodiscard]] const MusicSelectBar *selected() const;
  void rebuildRows(std::optional<MusicSelectBarId> preferred = std::nullopt);
  void rebuildProjectionIndex();
  [[nodiscard]] const MusicSelectBar *find(const MusicSelectBarId &) const;

  MusicSelectProjection projection_;
  std::vector<MusicSelectBarId> directory_;
  std::vector<MusicSelectBarId> sourceBars_;
  std::shared_ptr<std::vector<MusicSelectBar>> rows_ =
      std::make_shared<std::vector<MusicSelectBar>>();
  std::shared_ptr<std::vector<MusicSelectBar>> directoryBars_ =
      std::make_shared<std::vector<MusicSelectBar>>();
  std::string directoryText_;
  std::unordered_map<std::string, std::size_t> projectionIndex_;
  std::unordered_map<std::string, std::size_t> rowIndex_;
  std::unordered_map<std::string, RowProviderState> rowProviders_;
  std::shared_ptr<MusicSelectRowProvider> rowProvider_;
  std::uint64_t rowsRevision_ = 0;
  std::size_t selectedIndex_ = 0;
  int movementDirection_ = 0;
  std::int64_t movementEndMillis_ = 0;
  MusicSelectBarManagerConfig config_;
};
