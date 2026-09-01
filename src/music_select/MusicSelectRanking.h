#pragma once

#include "MusicSelectPropertyProjection.h"
#include "../ir/IrRankingModels.h"

[[nodiscard]] MusicSelectRankingSnapshot
projectMusicSelectRanking(const ir::IrRankingSnapshot &, int offset);
