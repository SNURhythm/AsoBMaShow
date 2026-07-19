#include "IrScoreHistoryProjection.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
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

} // namespace ir
