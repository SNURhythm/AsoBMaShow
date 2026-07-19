#pragma once

#include "../IrHttpClient.h"
#include "../IrRemoteScoreModels.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace ir::tachi {

inline constexpr std::size_t kMaximumTachiUserScoreResponseBytes =
    kMaximumIrHttpResponseBytes;

struct ParsedUserGameScores {
  std::vector<IrRemoteScore> scores;
};

[[nodiscard]] IrUserScoreSnapshotOutcome
parseUserGameScores(std::string_view expectedGame, long httpStatus,
                    std::string_view responseBody);

} // namespace ir::tachi
