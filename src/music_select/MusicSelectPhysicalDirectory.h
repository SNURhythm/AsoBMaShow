#pragma once

#include "MusicSelectBarManager.h"
#include "MusicSelectDirectoryLoader.h"
#include "MusicSelectRepositoryProjection.h"

[[nodiscard]] MusicSelectDirectoryLoader::Content loadMusicSelectPhysicalDirectory(
    ChartRepository &, const MusicSelectRepositoryMetadata &,
    const MusicSelectBar &, std::shared_ptr<const ScoreBestCache>,
    std::shared_ptr<const ScoreClearRankCache>, const std::filesystem::path &replayRoot,
    const MusicSelectBarManagerConfig &, int selectedLongNoteMode,
    std::stop_token stop = {});

[[nodiscard]] MusicSelectDirectoryLoader::Content
loadMusicSelectPhysicalDirectoryAutoplay(
    ChartRepository &, const MusicSelectBar &, int selectedLongNoteMode,
    std::stop_token stop = {});
