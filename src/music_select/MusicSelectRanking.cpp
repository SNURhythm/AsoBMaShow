#include "MusicSelectRanking.h"

#include <algorithm>

namespace {
MusicSelectRankingState stateFor(ir::IrRankingSnapshotState state) {
  switch (state) {
  case ir::IrRankingSnapshotState::Closed:
  case ir::IrRankingSnapshotState::Cancelled:
    return MusicSelectRankingState::None;
  case ir::IrRankingSnapshotState::Loading:
    return MusicSelectRankingState::Access;
  case ir::IrRankingSnapshotState::Succeeded:
    return MusicSelectRankingState::Finish;
  case ir::IrRankingSnapshotState::ChartNotFound:
  case ir::IrRankingSnapshotState::AuthenticationRequired:
  case ir::IrRankingSnapshotState::TransientFailure:
  case ir::IrRankingSnapshotState::Unsupported:
  case ir::IrRankingSnapshotState::MalformedResponse:
  case ir::IrRankingSnapshotState::OversizedResponse:
    return MusicSelectRankingState::Fail;
  }
  return MusicSelectRankingState::None;
}

int beatorajaClearType(int rank) {
  if (rank >= kClearTypeFullComboRank) return 8;
  if (rank >= kClearTypeExHardClearRank) return 7;
  if (rank >= kClearTypeHardClearRank) return 6;
  if (rank >= kClearTypeNormalClearRank) return 5;
  if (rank >= kClearTypeEasyClearRank) return 4;
  if (rank >= kClearTypeLightAssistedEasyClearRank) return 3;
  if (rank >= kClearTypeAssistedEasyClearRank) return 2;
  return 1;
}
} // namespace

std::string
musicSelectRankingCacheKey(const ir::IrRankingRequest &request,
                           std::uint64_t accountEvidenceRevision) {
  return request.profileId + "\n" + request.providerId + "\n" +
         request.serverOrigin + "\n" +
         std::to_string(request.chart.keyMode) + "\n" +
         request.chart.chartSha256 + "\n" +
         std::to_string(request.chart.totalNotes) + "\n" +
         std::to_string(accountEvidenceRevision);
}

MusicSelectRankingSnapshot
projectMusicSelectRanking(const ir::IrRankingSnapshot &source, int offset) {
  MusicSelectRankingSnapshot result;
  result.state = stateFor(source.state);
  if (source.state == ir::IrRankingSnapshotState::Succeeded) {
    if (source.paginationBlocked) {
      result.state = MusicSelectRankingState::Fail;
    } else if (source.loadingNextPage) {
      result.state = MusicSelectRankingState::Access;
    }
  }
  result.offset = offset;
  if (result.state != MusicSelectRankingState::Finish || !source.ranking) {
    return result;
  }

  auto entries = source.ranking->entries;
  std::stable_sort(entries.begin(), entries.end(), [](const auto &left,
                                                       const auto &right) {
    return left.score > right.score;
  });
  result.totalPlayers = static_cast<int>(entries.size());
  result.entries.reserve(entries.size());
  int previousScore = 0;
  int previousRank = 0;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto &entry = entries[index];
    const int rank = index > 0 && entry.score == previousScore
                         ? previousRank
                         : static_cast<int>(index) + 1;
    const int clearType = beatorajaClearType(entry.clearType);
    ++result.clearCounts[static_cast<std::size_t>(clearType)];
    if (entry.currentUser) result.rank = rank;
    result.entries.push_back({.name = entry.playerName,
                              .score = entry.score,
                              .rank = rank,
                              .playerType = entry.currentUser ? 1 : 0,
                              .clearType = clearType});
    previousScore = entry.score;
    previousRank = rank;
  }
  return result;
}
