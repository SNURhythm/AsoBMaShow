#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/GameplayScoreState.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ir {

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
  std::string playerName;
  int score = 0;
  int maxScore = 0;
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
  std::vector<IrChartRankingEntry> entries;
  std::int64_t fetchedAtUnixMillis = 0;

  bool operator==(const IrChartRanking &) const = default;
};

[[nodiscard]] IrChartQueryBuildOutcome
makeIrChartQuery(const bms_parser::ChartMeta &meta) noexcept;

} // namespace ir
