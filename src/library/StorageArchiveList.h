#pragma once
#include "GroupedChartListModel.h"

// Archive identity comes from library paths, without opening archive contents.
using StorageGroupArchives = std::map<std::string, std::filesystem::path>;
struct StorageArchiveRow {
  music_playlist::MusicTrack track;
  std::filesystem::path archivePath;
  bool archiveHeader = false;
  std::size_t songCount = 0, selectedCount = 0, visibleCount = 0, visibleSelectedCount = 0;
  unsigned IndentLevel() const;
};
std::vector<StorageArchiveRow> MakeStorageArchiveRows(
    const std::vector<music_playlist::MusicTrack> &visible,
    const StorageGroupArchives &, const StorageGroupSelection &);
void ToggleStorageArchiveSelection(const std::filesystem::path &,
    const std::vector<music_playlist::MusicTrack> &visible,
    const StorageGroupArchives &, StorageGroupSelection &);
