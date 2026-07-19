#include "IrScoreHistoryProjection.h"
#include "../ScoreRankUtils.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>

namespace ir {
namespace {

std::optional<std::string> formatUnixMillis(std::int64_t unixMillis) {
  if (unixMillis <= 0) {
    return std::nullopt;
  }
  const std::time_t seconds =
      static_cast<std::time_t>(unixMillis / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &seconds) != 0) {
    return std::nullopt;
  }
#else
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return std::nullopt;
  }
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << (unixMillis % 1'000);
  return output.str();
}

void storeImportedRank(ScoreRankMap &ranks, const std::string &hash,
                       int lampRank) {
  if (hash.empty()) {
    return;
  }
  auto &byMode = ranks[hash];
  for (auto &rank : byMode.ranks) {
    rank = std::max(rank, lampRank);
  }
}

void storeImportedBest(ScoreBestMap &scores, const std::string &hash,
                       const ScoreBestSnapshot &candidate) {
  if (hash.empty()) {
    return;
  }
  auto &byMode = scores[hash];
  for (auto &current : byMode.snapshots) {
    if (scoreBestSnapshotIsBetter(candidate, current)) {
      current = candidate;
    }
  }
}

ScoreBestSnapshot snapshotFor(const IrRemoteScore &remote) {
  ScoreBestSnapshot snapshot{
      .score = remote.score,
      .maxScore = std::max(0, remote.noteCount) * 2,
      .maxCombo = remote.maxCombo,
      .comboBreak = std::nullopt,
      .badPoints = remote.badPoints,
      .finalGauge = remote.finalGauge,
      .clearType = remote.lampRank,
      .createdAt = remote.timeAchievedUnixMillis
                       ? formatUnixMillis(*remote.timeAchievedUnixMillis)
                       : std::nullopt,
      .bestOrderTime = formatUnixMillis(
          remote.timeAchievedUnixMillis.value_or(remote.timeAddedUnixMillis)),
      .source = ScoreBestSource::ImportedIr,
  };
  return snapshot;
}

std::optional<int> scoreRankOrder(std::string_view rank) {
  if (rank == "MAX") {
    return 9;
  }
  if (rank == "MAX -") {
    return 8;
  }
  if (rank == "AAA") {
    return 7;
  }
  if (rank == "AA") {
    return 6;
  }
  if (rank == "A") {
    return 5;
  }
  if (rank == "B") {
    return 4;
  }
  if (rank == "C") {
    return 3;
  }
  if (rank == "D") {
    return 2;
  }
  if (rank == "E") {
    return 1;
  }
  if (rank == "F") {
    return 0;
  }
  return std::nullopt;
}

bool matchesClearFilter(const ChartMetaRecord &record,
                        const ChartMetaQuery &query,
                        const ScoreClearRankCache &clearRanks) {
  if (!query.clearMarkFilter) {
    return true;
  }
  const int rank =
      clearRanks.bestRankFor(record.meta, query.selectedLongNoteMode);
  if (query.clearMarkOrAbove) {
    return rank >= query.clearMarkRank;
  }
  if (query.clearMarkOrBelow) {
    return rank <= query.clearMarkRank;
  }
  return rank == query.clearMarkRank;
}

bool matchesScoreFilter(const ChartMetaRecord &record,
                        const ChartMetaQuery &query,
                        const ScoreBestCache &bestScores) {
  if (!query.scoreRank.has_value()) {
    return true;
  }
  const auto best = bestScores.bestFor(record.meta, query.selectedLongNoteMode);
  if (!best.has_value()) {
    return false;
  }
  const std::string rank =
      score_rank::labelForScore(best->score, best->maxScore);
  if (!query.scoreRankOrAbove && !query.scoreRankOrBelow) {
    return rank == *query.scoreRank;
  }
  const auto actualOrder = scoreRankOrder(rank);
  const auto requestedOrder = scoreRankOrder(*query.scoreRank);
  if (!actualOrder.has_value() || !requestedOrder.has_value()) {
    return false;
  }
  return query.scoreRankOrAbove ? *actualOrder >= *requestedOrder
                                : *actualOrder <= *requestedOrder;
}

int compareRequired(int left, int right,
                    ChartRecordSortDirection direction) {
  if (left == right) {
    return 0;
  }
  const bool leftFirst = direction == ChartRecordSortDirection::Ascending
                             ? left < right
                             : left > right;
  return leftFirst ? -1 : 1;
}

int compareOptional(const std::optional<int> &left,
                    const std::optional<int> &right,
                    ChartRecordSortDirection direction) {
  if (!left.has_value() && !right.has_value()) {
    return 0;
  }
  if (!left.has_value()) {
    return 1;
  }
  if (!right.has_value()) {
    return -1;
  }
  return compareRequired(*left, *right, direction);
}

struct ProjectedOrderValues {
  int clearRank = kNoClearTypeRank;
  std::optional<int> score;
  std::optional<int> maxCombo;
};

ProjectedOrderValues projectedOrderValues(
    const ChartMetaRecord &record, const ChartMetaQuery &query,
    const ScoreClearRankCache &clearRanks,
    const ScoreBestCache &bestScores) {
  ProjectedOrderValues values;
  values.clearRank =
      clearRanks.bestRankFor(record.meta, query.selectedLongNoteMode);
  const auto best = bestScores.bestFor(record.meta, query.selectedLongNoteMode);
  if (best.has_value()) {
    values.score = best->score;
    values.maxCombo = best->maxCombo;
  }
  return values;
}

} // namespace

void projectIrRemoteScores(std::span<const IrRemoteScore> remote,
                           ScoreClearRankCache &clearRanks,
                           ScoreBestCache &bestScores) {
  for (const auto &score : remote) {
    storeImportedRank(clearRanks.importedIrRankBySha256, score.chartSha256,
                      score.lampRank);
    const ScoreBestSnapshot snapshot = snapshotFor(score);
    storeImportedBest(bestScores.importedIrScoreBySha256, score.chartSha256,
                      snapshot);
    if (!score.chartMd5.empty()) {
      storeImportedRank(clearRanks.importedIrRankByMd5[score.chartMd5],
                        score.chartSha256, score.lampRank);
      storeImportedBest(bestScores.importedIrScoreByMd5[score.chartMd5],
                        score.chartSha256, snapshot);
    }
  }
}

bool chartMetaQueryUsesProjectedScores(const ChartMetaQuery &query) noexcept {
  if (query.solidArchivesOnly) {
    return false;
  }
  return query.clearMarkFilter || query.scoreRank.has_value() ||
         query.sortCriterion == ChartRecordSortCriterion::ClearMark ||
         query.sortCriterion == ChartRecordSortCriterion::Score;
}

ChartMetaQuery
chartMetaQueryWithoutProjectedScoreCriteria(const ChartMetaQuery &query) {
  ChartMetaQuery base = query;
  base.clearMarkFilter = false;
  base.clearMarkRank = -1;
  base.clearMarkOrAbove = false;
  base.clearMarkOrBelow = false;
  base.scoreRank.reset();
  base.scoreRankOrAbove = false;
  base.scoreRankOrBelow = false;
  if (base.sortCriterion == ChartRecordSortCriterion::ClearMark ||
      base.sortCriterion == ChartRecordSortCriterion::Score) {
    base.sortCriterion = ChartRecordSortCriterion::Default;
    base.sortDirection = ChartRecordSortDirection::Descending;
  }
  base.selectedLongNoteMode = 1;
  base.limit = 0;
  base.offset = 0;
  return base;
}

const std::vector<ChartMetaRecord> &ProjectedChartMetadataCache::recordsFor(
    const ChartMetaQuery &query, std::uint64_t libraryRevision,
    const Loader &loader) {
  const ChartMetaQuery baseQuery =
      chartMetaQueryWithoutProjectedScoreCriteria(query);
  if (baseQuery_ == baseQuery && libraryRevision_ == libraryRevision) {
    return records_;
  }

  std::vector<ChartMetaRecord> loadedRecords;
  loader(baseQuery, loadedRecords);
  baseQuery_ = baseQuery;
  libraryRevision_ = libraryRevision;
  records_ = std::move(loadedRecords);
  return records_;
}

void ProjectedChartMetadataCache::clear() noexcept {
  baseQuery_.reset();
  libraryRevision_ = 0;
  records_.clear();
}

std::vector<std::size_t> projectedScoreQueryIndices(
    const ChartMetaQuery &query, const ScoreClearRankCache &clearRanks,
    const ScoreBestCache &bestScores,
    std::span<const ChartMetaRecord> records) {
  std::vector<std::size_t> indices(records.size());
  std::iota(indices.begin(), indices.end(), std::size_t{0});
  std::erase_if(indices, [&](std::size_t index) {
    const auto &record = records[index];
    return !matchesClearFilter(record, query, clearRanks) ||
           !matchesScoreFilter(record, query, bestScores);
  });

  if (query.sortCriterion != ChartRecordSortCriterion::ClearMark &&
      query.sortCriterion != ChartRecordSortCriterion::Score) {
    return indices;
  }
  std::stable_sort(indices.begin(), indices.end(),
                   [&](std::size_t leftIndex, std::size_t rightIndex) {
    const auto leftValues = projectedOrderValues(
        records[leftIndex], query, clearRanks, bestScores);
    const auto rightValues = projectedOrderValues(
        records[rightIndex], query, clearRanks, bestScores);
    int comparison = 0;
    if (query.sortCriterion == ChartRecordSortCriterion::ClearMark) {
      comparison = compareRequired(leftValues.clearRank,
                                   rightValues.clearRank,
                                   query.sortDirection);
      if (comparison == 0) {
        comparison = compareOptional(
            leftValues.score, rightValues.score,
            ChartRecordSortDirection::Descending);
      }
    } else {
      comparison = compareOptional(leftValues.score, rightValues.score,
                                   query.sortDirection);
      if (comparison == 0) {
        comparison = compareRequired(
            leftValues.clearRank, rightValues.clearRank,
            ChartRecordSortDirection::Descending);
      }
    }
    if (comparison == 0) {
      comparison = compareOptional(leftValues.maxCombo,
                                   rightValues.maxCombo,
                                   ChartRecordSortDirection::Descending);
    }
    return comparison < 0;
  });
  return indices;
}

void applyProjectedScoreQuery(const ChartMetaQuery &query,
                              const ScoreClearRankCache &clearRanks,
                              const ScoreBestCache &bestScores,
                              std::vector<ChartMetaRecord> &records) {
  const auto indices =
      projectedScoreQueryIndices(query, clearRanks, bestScores, records);
  std::vector<ChartMetaRecord> projected;
  projected.reserve(indices.size());
  for (const std::size_t index : indices) {
    projected.push_back(std::move(records[index]));
  }
  records = std::move(projected);
}

int findProjectedChartPathIndex(std::span<const ChartMetaRecord> records,
                                const std::filesystem::path &path) {
  if (path.empty()) {
    return -1;
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (records[index].meta.BmsPath == path) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

} // namespace ir
