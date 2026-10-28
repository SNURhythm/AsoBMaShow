#pragma once

#include "MusicSelectRepositoryProjection.h"
#include "../kombo/KomboModel.h"

struct MusicSelectAssignmentChart {
  ChartMetaRecord record;
  std::vector<kombo::Level> levels;
};

[[nodiscard]] bool musicSelectIsAssignmentDirectory(const MusicSelectBar &);
[[nodiscard]] MusicSelectBar musicSelectAssignmentsRoot(std::string title);
[[nodiscard]] std::vector<MusicSelectBar> musicSelectAssignmentChildren(
    const kombo::Day &, const MusicSelectBar &,
    std::span<const MusicSelectAssignmentChart>, MusicSelectRepositoryProjectionInput);
[[nodiscard]] std::vector<MusicSelectAssignmentChart> loadMusicSelectAssignmentCharts(
    ChartRepository::Session &, const MusicSelectRepositoryMetadata &,
    const kombo::Day &, int selectedLongNoteMode);

// Only a shortage of distinct, playable candidates permits changing today's round.
[[nodiscard]] bool musicSelectAssignmentNeedsAdjustment(
    const kombo::Day &, const kombo::Progress &,
    std::span<const MusicSelectAssignmentChart>, MusicSelectRepositoryProjectionInput);
