#pragma once

#include "MusicSelectTypes.h"
#include "../repositories/ScoreRepositoryModels.h"

#include <cstdint>
#include <functional>
#include <span>

struct MusicSelectRepositoryProjectionInput {
  std::span<const ChartMetaRecord> records;
  std::function<std::optional<ScoreBestSnapshot>(const bms_parser::ChartMeta &,
                                                 int)> scoreFor;
  int selectedLongNoteMode = 0;
  std::uint64_t repositoryRevision = 0;
};

class MusicSelectRepositoryProjection final {
public:
  [[nodiscard]] MusicSelectProjection
  project(MusicSelectRepositoryProjectionInput) const;
};
