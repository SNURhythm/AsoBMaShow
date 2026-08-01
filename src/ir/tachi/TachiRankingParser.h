#pragma once

#include "../IrDriver.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ir::tachi {

inline constexpr std::size_t kMaximumRankingResponseBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaximumRankingEntries = 20'000;
inline constexpr std::size_t kMaximumPlayerNameCodePoints = 64;

struct TachiRankingIdentityOutcome {
  ChartRankingStatus status = ChartRankingStatus::MalformedResponse;
  std::optional<std::int64_t> userId;
  std::string diagnostic;
};

struct TachiChartResolveOutcome {
  ChartRankingStatus status = ChartRankingStatus::MalformedResponse;
  std::optional<std::string> chartId;
  std::string diagnostic;
};

struct TachiRankingPage {
  std::vector<IrChartRankingEntry> entries;
  std::vector<std::int64_t> userIds;
  int outOf = 0;
};

struct TachiRankingPageOutcome {
  ChartRankingStatus status = ChartRankingStatus::MalformedResponse;
  std::optional<TachiRankingPage> page;
  std::string diagnostic;
};

[[nodiscard]] TachiRankingIdentityOutcome
parseRankingIdentityResponse(std::string_view body) noexcept;

[[nodiscard]] TachiChartResolveOutcome
parseChartResolveResponse(std::string_view body,
                          const IrChartQuery &query) noexcept;

[[nodiscard]] TachiRankingPageOutcome
parseRankingPageResponse(std::string_view body, const IrChartQuery &query,
                         std::string_view expectedChartId,
                         std::optional<std::int64_t>
                             authenticatedUserId) noexcept;

} // namespace ir::tachi
