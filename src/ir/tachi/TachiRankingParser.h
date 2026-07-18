#pragma once

#include "../IrDriver.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ir::tachi {

inline constexpr std::size_t kMaximumRankingResponseBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaximumRankingEntries = 20'000;
inline constexpr std::size_t kMaximumPlayerNameCodePoints = 64;

struct RankingTuple {
  int score = 0;
  int clearIndex = 0;
  int badPoints = 0;
  std::optional<std::int64_t> achievedAt;

  bool operator==(const RankingTuple &) const = default;
};

[[nodiscard]] ChartRankingOutcome
parseRankingResponse(std::string_view body, const IrChartQuery &query) noexcept;

} // namespace ir::tachi
