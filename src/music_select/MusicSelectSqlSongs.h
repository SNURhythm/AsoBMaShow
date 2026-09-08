#pragma once

#include "MusicSelectBarManager.h"
#include "../repositories/ChartMetaPageCache.h"

#include <functional>
#include <string_view>

class MusicSelectSqlSongs final : public MusicSelectRowProvider {
public:
  using PageLoader = std::function<std::vector<ChartMetaRecord>(
      std::size_t offset, std::size_t limit)>;
  using IndexLookup =
      std::function<std::optional<std::size_t>(std::string_view identity)>;
  using Projector = std::function<MusicSelectBar(const ChartMetaRecord &)>;

  struct ResolvedQuery {
    std::size_t count = 0;
    std::pair<std::string, std::string> resolvedFilters = {"ALL", "ALL"};
    PageLoader loadPage;
    IndexLookup findIndex;
  };

  using QueryResolver =
      std::function<ResolvedQuery(const MusicSelectBarManagerConfig &)>;

  MusicSelectSqlSongs(std::string context, QueryResolver, Projector,
                      MusicSelectBarManagerConfig = {});
  MusicSelectSqlSongs(const MusicSelectSqlSongs &) = delete;
  MusicSelectSqlSongs &operator=(const MusicSelectSqlSongs &) = delete;
  MusicSelectSqlSongs(MusicSelectSqlSongs &&) = delete;
  MusicSelectSqlSongs &operator=(MusicSelectSqlSongs &&) = delete;
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
  struct PreparedClone {};
  MusicSelectSqlSongs(const MusicSelectSqlSongs &, PreparedClone);
  void resetPages();
  std::vector<MusicSelectBar> loadPage(std::size_t, std::size_t) const;
  MusicSelectBar unavailableBar(std::size_t) const;

  std::string context_;
  QueryResolver resolve_;
  Projector project_;
  MusicSelectBarManagerConfig config_;
  ResolvedQuery query_;
  BoundedPageCache<MusicSelectBar> pages_;
  mutable std::string diagnostic_;
};
