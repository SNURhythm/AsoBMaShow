#pragma once

#include "MusicSelectBarManager.h"
#include "MusicSelectSongIndex.h"
#include "../repositories/ChartMetaPageCache.h"

#include <functional>
#include <span>

class MusicSelectPagedSongs final : public MusicSelectRowProvider {
public:
  using RecordLoader = std::function<ChartMetaPathBatchReadOutcome(
      std::span<const std::filesystem::path>)>;
  using Projector = std::function<MusicSelectBar(const ChartMetaRecord &)>;

  MusicSelectPagedSongs(MusicSelectSongIndex, RecordLoader, Projector,
                        MusicSelectBarManagerConfig = {});
  MusicSelectPagedSongs(const MusicSelectPagedSongs &) = delete;
  MusicSelectPagedSongs &operator=(const MusicSelectPagedSongs &) = delete;
  MusicSelectPagedSongs(MusicSelectPagedSongs &&) = delete;
  MusicSelectPagedSongs &operator=(MusicSelectPagedSongs &&) = delete;
  [[nodiscard]] std::size_t size() const noexcept override;
  [[nodiscard]] std::shared_ptr<MusicSelectRowProvider> clone() const override;
  [[nodiscard]] const MusicSelectBar &at(std::size_t) const override;
  [[nodiscard]] std::optional<std::size_t>
  indexOf(const MusicSelectBarId &) const override;
  std::pair<std::string, std::string> configure(
      const std::string &, const std::string &, const std::string &) override;
  [[nodiscard]] const std::string &diagnostic() const noexcept override;
  void retryFailedPages();

private:
  void resetPages();
  std::vector<MusicSelectBar> loadPage(std::size_t, std::size_t) const;
  MusicSelectBar unavailableBar(std::size_t) const;

  MusicSelectSongIndex index_;
  RecordLoader loadRecords_;
  Projector project_;
  MusicSelectBarManagerConfig config_;
  std::pair<std::string, std::string> resolved_;
  BoundedPageCache<MusicSelectBar> pages_;
  mutable std::string diagnostic_;
};
