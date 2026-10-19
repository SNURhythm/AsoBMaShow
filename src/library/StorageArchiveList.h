#pragma once
#include "GroupedChartListModel.h"
#include <functional>
#include <optional>

// Archive identity comes from library paths, without opening archive contents.
using StorageGroupArchives = std::map<std::string, std::filesystem::path>;
using StorageArchiveSizeLookup = std::function<std::optional<std::uint64_t>(
    const std::filesystem::path &archive, const std::string &groupId)>;
struct StorageArchiveRow {
  music_playlist::MusicTrack track;
  std::filesystem::path archivePath;
  bool archiveHeader = false;
  bool expanded = false;
  bool songLeader = true;
  std::size_t songCount = 0, selectedCount = 0, visibleCount = 0, visibleSelectedCount = 0;
  unsigned IndentLevel() const;
};
std::vector<StorageArchiveRow> MakeStorageArchiveRows(
    const std::vector<music_playlist::MusicTrack> &visible,
    const StorageGroupArchives &, const StorageGroupSelection &,
    const std::set<std::filesystem::path> &expandedArchives = {},
    const StorageArchiveSizeLookup &sizeOrder = {});
void ToggleStorageArchiveSelection(const std::filesystem::path &,
    const std::vector<music_playlist::MusicTrack> &visible,
    const StorageGroupArchives &, StorageGroupSelection &);
