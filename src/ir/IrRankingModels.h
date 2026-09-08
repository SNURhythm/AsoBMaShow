#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/GameplayScoreState.h"

#include <cstdint>
#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

enum class ChartRankingStatus;

struct IrChartQuery {
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int totalNotes = 0;

  bool operator==(const IrChartQuery &) const = default;
};

struct IrChartQueryBuildOutcome {
  std::optional<IrChartQuery> value;
  std::string diagnostic;
};

struct IrChartRankingEntry {
  int rank = 0;
  std::string providerEntryId;
  std::string playerName;
  int score = 0;
  int maxScore = 0;
  std::optional<int> pGreat;
  std::optional<int> great;
  std::optional<int> good;
  std::optional<int> bad;
  std::optional<int> poor;
  std::optional<int> earlyPGreat;
  std::optional<int> latePGreat;
  std::optional<int> earlyGreat;
  std::optional<int> lateGreat;
  std::optional<int> earlyGood;
  std::optional<int> lateGood;
  std::optional<int> earlyBad;
  std::optional<int> lateBad;
  std::optional<int> earlyPoor;
  std::optional<int> latePoor;
  int clearType = kClearTypeFailedRank;
  std::optional<int> badPoints;
  std::optional<int> maxCombo;
  std::optional<std::int64_t> achievedAtUnixMillis;
  bool currentUser = false;

  bool operator==(const IrChartRankingEntry &) const = default;
};

struct IrChartRanking {
  std::string providerId;
  IrChartQuery chart;
  int totalPlayers = 0;
  std::vector<IrChartRankingEntry> entries;
  std::optional<std::string> nextPageToken;
  std::int64_t fetchedAtUnixMillis = 0;

  bool operator==(const IrChartRanking &) const = default;
};

struct IrLocalComparison {
  std::string label;
  int score = 0;
  int maxScore = 0;
  int clearType = kClearTypeFailedRank;
  std::optional<int> badPoints;
  std::optional<int> maxCombo;

  bool operator==(const IrLocalComparison &) const = default;
};

struct IrRankingRequest {
  std::uint64_t generation = 0;
  std::string profileId;
  std::string providerId;
  std::string serverOrigin;
  IrChartQuery chart;
  std::optional<IrLocalComparison> localComparison;

  bool operator==(const IrRankingRequest &) const = default;
};

struct IrRankingCacheKey {
  std::string profileId;
  std::string providerId;
  std::string serverOrigin;
  int keyMode = 0;
  std::string chartSha256;
  int totalNotes = 0;

  auto operator<=>(const IrRankingCacheKey &) const = default;
};

struct IrRankingCacheKeyBuildOutcome {
  std::optional<IrRankingCacheKey> value;
  std::string diagnostic;
};

enum class IrRankingSnapshotState {
  Closed,
  Loading,
  Succeeded,
  ChartNotFound,
  AuthenticationRequired,
  TransientFailure,
  Unsupported,
  MalformedResponse,
  OversizedResponse,
  Cancelled,
};

struct IrRankingSnapshot {
  std::uint64_t revision = 0;
  std::uint64_t generation = 0;
  IrRankingSnapshotState state = IrRankingSnapshotState::Closed;
  std::optional<IrRankingRequest> request;
  std::shared_ptr<const IrChartRanking> ranking;
  std::string diagnostic;
  bool fromCache = false;
  bool loadingNextPage = false;
  bool paginationBlocked = false;
};

struct IrRankingInvalidation {
  std::optional<std::string> profileId;
  std::optional<std::string> providerId;
  std::optional<std::string> serverOrigin;
  std::optional<std::string> chartSha256;
  bool clearVisible = true;
};

[[nodiscard]] IrChartQueryBuildOutcome
makeIrChartQuery(const bms_parser::ChartMeta &meta) noexcept;

[[nodiscard]] std::optional<int>
calculateIrBadPoints(int bad, int poor, int kPoor) noexcept;

[[nodiscard]] IrRankingCacheKeyBuildOutcome
makeIrRankingCacheKey(const IrRankingRequest &request) noexcept;

[[nodiscard]] IrRankingSnapshotState
snapshotStateFor(ChartRankingStatus status) noexcept;

[[nodiscard]] std::string
describeIrRankingCacheKey(const IrRankingCacheKey &key);

[[nodiscard]] std::string describeIrChartRanking(const IrChartRanking &ranking);

} // namespace ir
