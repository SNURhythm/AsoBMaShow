#pragma once

#include "MusicSelectTypes.h"
#include "../repositories/ScoreRepositoryModels.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

struct MusicSelectDifficultyLevelSource {
  DifficultyLevelInfo info;
  std::vector<ChartMetaRecord> records;
};

struct MusicSelectCourseSource {
  DifficultyCourseInfo info;
  std::vector<ChartMetaRecord> stages;
};

struct MusicSelectDifficultyTableSource {
  DifficultyTableInfo info;
  std::vector<MusicSelectDifficultyLevelSource> levels;
  std::vector<MusicSelectCourseSource> courses;
};

struct MusicSelectRepositoryMetadata {
  std::vector<ChartEntry> entries;
  std::vector<MusicSelectDifficultyTableSource> tables;
};

struct MusicSelectRepositoryProjectionInput {
  std::span<const ChartMetaRecord> records;
  std::function<std::optional<ScoreBestSnapshot>(const bms_parser::ChartMeta &,
                                                 int)> scoreFor;
  std::function<int(std::string_view, int, int)> courseRankFor;
  const MusicSelectRepositoryMetadata *metadata = nullptr;
  std::string_view modeFilter = "ALL";
  int selectedLongNoteMode = 0;
  std::uint64_t repositoryRevision = 0;
};

class MusicSelectRepositoryProjection final {
public:
  [[nodiscard]] static MusicSelectRepositoryMetadata
  loadMetadata(ChartRepository::Session &, int selectedLongNoteMode);

  [[nodiscard]] MusicSelectProjection
  project(MusicSelectRepositoryProjectionInput) const;
};
