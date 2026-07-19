#pragma once

#include "IrRemoteScoreModels.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ScoreRepositoryModels.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ir {

void projectIrRemoteScores(std::span<const IrRemoteScore> remote,
                           ScoreClearRankCache &clearRanks,
                           ScoreBestCache &bestScores);

[[nodiscard]] bool
chartMetaQueryUsesProjectedScores(const ChartMetaQuery &query) noexcept;

[[nodiscard]] ChartMetaQuery
chartMetaQueryWithoutProjectedScoreCriteria(const ChartMetaQuery &query);

class ProjectedChartMetadataCache final {
public:
  using Loader = std::function<void(const ChartMetaQuery &,
                                    std::vector<ChartMetaRecord> &)>;

  [[nodiscard]] const std::vector<ChartMetaRecord> &
  recordsFor(const ChartMetaQuery &query, std::uint64_t libraryRevision,
             const Loader &loader);
  void clear() noexcept;

private:
  std::optional<ChartMetaQuery> baseQuery_;
  std::uint64_t libraryRevision_ = 0;
  std::vector<ChartMetaRecord> records_;
};

[[nodiscard]] std::vector<std::size_t> projectedScoreQueryIndices(
    const ChartMetaQuery &query, const ScoreClearRankCache &clearRanks,
    const ScoreBestCache &bestScores,
    std::span<const ChartMetaRecord> records);

void applyProjectedScoreQuery(const ChartMetaQuery &query,
                              const ScoreClearRankCache &clearRanks,
                              const ScoreBestCache &bestScores,
                              std::vector<ChartMetaRecord> &records);

[[nodiscard]] int findProjectedChartPathIndex(
    std::span<const ChartMetaRecord> records,
    const std::filesystem::path &path);

} // namespace ir
