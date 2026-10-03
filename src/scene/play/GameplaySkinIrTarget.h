#pragma once

#include "../../ir/IrRankingModels.h"
#include "../../skin/beatoraja/BeatorajaTargetPropertyNames.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// InternetRankingTargetProperty's selected ScoreData, independent of the
// built-in HUD's user-selected pacemaker. Project once per ranking revision.
struct PlayfieldIrTargetState {
  std::string targetId;
  std::string playerName = "NO DATA";
  int score = 0;

  bool operator==(const PlayfieldIrTargetState &) const = default;
};

[[nodiscard]] inline bool isGameplaySkinIrTarget(std::string_view id) {
  using skin::beatoraja_target_property_detail::positiveSuffix;
  const auto rate = positiveSuffix(id, "IR_RANKRATE_");
  return positiveSuffix(id, "IR_NEXT_").has_value() ||
         positiveSuffix(id, "IR_RANK_").has_value() ||
         (rate && *rate < 100);
}

[[nodiscard]] inline PlayfieldIrTargetState projectGameplaySkinIrTarget(
    std::string_view id, const ir::IrRankingSnapshot &snapshot,
    int localBestScore) {
  PlayfieldIrTargetState result{.targetId = std::string(id)};
  // Partial pages cannot resolve NEXT or a percentile reliably. Do not
  // present their target as final, including after failed pagination.
  if (!isGameplaySkinIrTarget(id) ||
      snapshot.state != ir::IrRankingSnapshotState::Succeeded ||
      !snapshot.ranking || snapshot.loadingNextPage ||
      snapshot.paginationBlocked || snapshot.ranking->nextPageToken ||
      snapshot.ranking->totalPlayers < 0 ||
      snapshot.ranking->entries.size() !=
          static_cast<std::size_t>(snapshot.ranking->totalPlayers) ||
      snapshot.ranking->entries.empty()) {
    return result;
  }
  std::vector<const ir::IrChartRankingEntry *> entries;
  entries.reserve(snapshot.ranking->entries.size());
  for (const auto &entry : snapshot.ranking->entries) entries.push_back(&entry);
  std::stable_sort(entries.begin(), entries.end(), [](auto left, auto right) {
    return left->score > right->score;
  });
  using skin::beatoraja_target_property_detail::positiveSuffix;
  std::size_t index = 0;
  if (const auto next = positiveSuffix(id, "IR_NEXT_")) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i]->score <= localBestScore) {
        index = i > static_cast<std::size_t>(*next) ? i - *next : 0;
        break;
      }
    }
  } else if (const auto rank = positiveSuffix(id, "IR_RANK_")) {
    index = std::min(entries.size(), static_cast<std::size_t>(*rank)) - 1;
  } else if (const auto rate = positiveSuffix(id, "IR_RANKRATE_")) {
    index = entries.size() * static_cast<std::size_t>(*rate) / 100;
  }
  const auto &entry = *entries[index];
  result.playerName = entry.currentUser || entry.playerName.empty()
                          ? "YOU" : entry.playerName;
  result.score = entry.score;
  return result;
}

[[nodiscard]] inline std::optional<int>
gameplaySkinIrCurrentUserRank(const ir::IrRankingSnapshot &snapshot) {
  if (snapshot.state != ir::IrRankingSnapshotState::Succeeded ||
      !snapshot.ranking) return std::nullopt;
  for (const auto &entry : snapshot.ranking->entries) {
    if (entry.currentUser && entry.rank > 0) return entry.rank;
  }
  for (const auto &entry : snapshot.ranking->nearbyEntries) {
    if (entry.currentUser && entry.rank > 0) return entry.rank;
  }
  return std::nullopt;
}

[[nodiscard]] inline bool gameplaySkinRankingMatches(
    const ir::IrRankingSnapshot &snapshot, const ir::IrRankingRequest &request) {
  return snapshot.request &&
         snapshot.request->profileId == request.profileId &&
         snapshot.request->providerId == request.providerId &&
         snapshot.request->serverOrigin == request.serverOrigin &&
         snapshot.request->chart == request.chart &&
         (!snapshot.ranking ||
          (snapshot.ranking->providerId == request.providerId &&
           snapshot.ranking->chart == request.chart));
}
