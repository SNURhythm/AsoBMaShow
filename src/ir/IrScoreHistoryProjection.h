#pragma once

#include "IrRemoteScoreModels.h"
#include "../repositories/ScoreRepositoryModels.h"

#include <span>

namespace ir {

void projectIrRemoteScores(std::span<const IrRemoteScore> remote,
                           ScoreClearRankCache &clearRanks,
                           ScoreBestCache &bestScores);

} // namespace ir
