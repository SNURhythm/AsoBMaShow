#pragma once

#include "MusicSelectRepositoryProjection.h"
#include "../streak/StreakModel.h"

struct MusicSelectAssignmentChart {
  ChartMetaRecord record;
  std::vector<streak::Level> levels;
};

[[nodiscard]] bool musicSelectIsAssignmentDirectory(const MusicSelectBar &);
[[nodiscard]] MusicSelectBar musicSelectAssignmentsRoot(std::string title);
[[nodiscard]] std::vector<MusicSelectBar> musicSelectAssignmentChildren(
    const streak::Day &, const MusicSelectBar &,
    std::span<const MusicSelectAssignmentChart>, MusicSelectRepositoryProjectionInput);
[[nodiscard]] std::vector<MusicSelectAssignmentChart> loadMusicSelectAssignmentCharts(
    ChartRepository::Session &, const MusicSelectRepositoryMetadata &,
    const streak::Day &, int selectedLongNoteMode);
