#pragma once

#include "MusicSelectPropertyProjection.h"
#include "../ir/IrRankingModels.h"

#include <cstdint>
#include <string>

[[nodiscard]] std::string
musicSelectRankingCacheKey(const ir::IrRankingRequest &,
                           std::uint64_t accountEvidenceRevision);

[[nodiscard]] MusicSelectRankingSnapshot
projectMusicSelectRanking(const ir::IrRankingSnapshot &, int offset);
