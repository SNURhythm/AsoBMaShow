#pragma once

#include "IrRemoteScoreModels.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ScoreRepositoryModels.h"

#include <filesystem>
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

void applyProjectedScoreQuery(const ChartMetaQuery &query,
                              const ScoreClearRankCache &clearRanks,
                              const ScoreBestCache &bestScores,
                              std::vector<ChartMetaRecord> &records);

[[nodiscard]] int findProjectedChartPathIndex(
    std::span<const ChartMetaRecord> records,
    const std::filesystem::path &path);

} // namespace ir
