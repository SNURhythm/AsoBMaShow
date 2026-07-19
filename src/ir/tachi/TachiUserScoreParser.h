#pragma once

#include "../IrRemoteScoreModels.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace ir::tachi {

inline constexpr std::size_t kMaximumTachiUserScoreResponseBytes =
    64U * 1024U * 1024U;

struct ParsedUserGameScores {
  std::vector<IrRemoteScore> scores;
};

[[nodiscard]] IrUserScoreSnapshotOutcome
parseUserGameScores(std::string_view expectedGame, long httpStatus,
                    std::string_view responseBody);

} // namespace ir::tachi
