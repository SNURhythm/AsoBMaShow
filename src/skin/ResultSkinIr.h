#pragma once

#include "../ir/IrRankingModels.h"
#include "../ir/IrSubmissionService.h"

#include <algorithm>
#include <array>
#include <optional>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

struct ResultIrRankingEntryData {
  int rank = 0;
  std::string playerName;
  int score = 0;
  int clearType = kClearTypeFailedRank;
  bool currentUser = false;
};

namespace result_skin_ir {

struct RankingData {
  std::vector<ResultIrRankingEntryData> entries;
  std::optional<int> currentUserRank;
  std::optional<int> totalPlayers;
  std::optional<std::array<int, 11>> clearCounts;
};

inline int clearIndex(int rank) {
  if (rank >= kClearTypeFullComboRank) return 8;
  if (rank >= kClearTypeExHardClearRank) return 7;
  if (rank >= kClearTypeHardClearRank) return 6;
  if (rank >= kClearTypeNormalClearRank) return 5;
  if (rank >= kClearTypeEasyClearRank) return 4;
  if (rank >= kClearTypeLightAssistedEasyClearRank) return 3;
  if (rank >= kClearTypeAssistedEasyClearRank) return 2;
  return 1;
}

inline RankingData projectRanking(const ir::IrRankingSnapshot &source) {
  RankingData result;
  if (source.state != ir::IrRankingSnapshotState::Succeeded || !source.ranking) {
    return result;
  }
  const auto &ranking = *source.ranking;
  result.totalPlayers = ranking.totalPlayers;
  const bool complete = !source.loadingNextPage && !source.paginationBlocked &&
                        !ranking.nextPageToken && ranking.totalPlayers >= 0 &&
                        ranking.entries.size() ==
                            static_cast<std::size_t>(ranking.totalPlayers);
  if (complete) result.clearCounts.emplace();
  result.entries.reserve(ranking.entries.size());
  for (const auto &entry : ranking.entries) {
    result.entries.push_back({.rank = entry.rank,
                              .playerName = entry.playerName,
                              .score = entry.score,
                              .clearType = entry.clearType,
                              .currentUser = entry.currentUser});
    if (entry.currentUser) result.currentUserRank = entry.rank;
    if (complete) {
      ++(*result.clearCounts)[static_cast<std::size_t>(clearIndex(entry.clearType))];
    }
  }
  return result;
}

inline int aggregateCount(int id, const std::array<int, 11> &counts) {
  constexpr std::array<int, 11> countIds{202, 210, 204, 206, 212, 214,
                                         216, 208, 218, 222, 224};
  constexpr std::array<int, 11> fractionIds{230, 234, 231, 232, 235, 236,
                                            237, 233, 238, 239, 240};
  for (std::size_t index = 0; index < counts.size(); ++index) {
    if (id == countIds[index] || id == countIds[index] + 1 ||
        id == fractionIds[index]) return counts[index];
  }
  const bool fullCombo = id == 228 || id == 229 || id == 242;
  return std::accumulate(counts.begin() + (fullCombo ? 8 : 2), counts.end(), 0);
}

inline int integerAggregate(int id,
                            const std::optional<std::array<int, 11>> &counts,
                            std::optional<int> total) {
  if (!counts) return std::numeric_limits<int>::min();
  const int count = aggregateCount(id, *counts);
  if ((id >= 202 && id <= 218 && id % 2 == 0) ||
      id == 222 || id == 224 || id == 226 || id == 228) return count;
  if (!total || *total <= 0) return std::numeric_limits<int>::min();
  const auto scale = static_cast<std::int64_t>(count) * (id >= 230 ? 1000 : 100);
  const int rate = static_cast<int>(scale / *total);
  return id >= 230 ? rate % 10 : rate;
}

inline double floatAggregate(int id,
                             const std::optional<std::array<int, 11>> &counts,
                             std::optional<int> total) {
  if (!counts || !total || *total <= 0) return std::numeric_limits<float>::min();
  return static_cast<double>(aggregateCount(id, *counts)) / *total;
}

// The source result timers describe score transmission, independently of the
// ranking download. Timestamps are supplied by the scene's monotonic clock.
struct SubmissionTimers {
  std::array<std::optional<std::int64_t>, 3> startedMicros{};
  bool succeeded = false;

  bool observe(const ir::IrAttemptStatusSnapshot &snapshot,
               std::int64_t nowMicros) {
    if (!snapshot.found) return false;
    const bool active = snapshot.activeRequest != ir::IrActiveRequestKind::None ||
                        snapshot.state == ir::IrOutboxState::Uploading ||
                        snapshot.state == ir::IrOutboxState::AwaitingRemoteResult;
    const bool complete = snapshot.state == ir::IrOutboxState::Succeeded;
    const bool failed = snapshot.state == ir::IrOutboxState::FailedPermanent ||
                        snapshot.state == ir::IrOutboxState::BlockedConfiguration ||
                        (!active && snapshot.consecutiveFailureCount > 0);
    if ((active || snapshot.requestAttemptCount > 0 || complete) &&
        !startedMicros[0]) startedMicros[0] = nowMicros;
    if (complete && !startedMicros[1]) startedMicros[1] = nowMicros;
    if (failed && !startedMicros[2]) startedMicros[2] = nowMicros;
    const bool refreshRanking = complete && !succeeded;
    succeeded = complete;
    return refreshRanking;
  }
};

} // namespace result_skin_ir
