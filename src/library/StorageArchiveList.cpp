#include "StorageArchiveList.h"
#include <algorithm>

unsigned StorageArchiveRow::IndentLevel() const {
  return archiveHeader ? 0 : (!archivePath.empty() ? 1 : 0) + (track.expandedChart && !songLeader ? 1 : 0);
}
std::vector<StorageArchiveRow> MakeStorageArchiveRows(
    const std::vector<music_playlist::MusicTrack> &visible,
    const StorageGroupArchives &archives, const StorageGroupSelection &selection,
    const std::set<std::filesystem::path> &expandedArchives) {
  struct Section {
    StorageArchiveRow header;
    std::vector<const music_playlist::MusicTrack *> tracks;
    std::set<std::string> visibleGroups;
  };
  std::vector<Section> sections;
  std::map<std::filesystem::path, std::size_t> positions;
  // The first matching song places the section; songs keep their sorted order.
  for (const auto &track : visible) {
    const auto archive = archives.find(track.groupId);
    if (archive == archives.end()) {
      sections.push_back({.tracks = {&track}});
      continue;
    }
    const auto [position, added] = positions.emplace(archive->second, sections.size());
    if (added) sections.push_back({.header = {.archivePath = archive->second, .archiveHeader = true,
        .expanded = expandedArchives.contains(archive->second)}});
    auto &section = sections[position->second];
    section.tracks.push_back(&track);
    if (section.visibleGroups.insert(track.groupId).second) {
      ++section.header.visibleCount;
      if (selection.Contains(track.groupId)) ++section.header.visibleSelectedCount;
    }
  }
  for (const auto &[id, archive] : archives) {
    const auto position = positions.find(archive);
    if (position == positions.end()) continue;
    auto &header = sections[position->second].header;
    ++header.songCount;
    if (selection.Contains(id)) ++header.selectedCount;
  }
  std::vector<StorageArchiveRow> result;
  std::size_t rowCount = 0;
  for (const auto &section : sections)
    rowCount += section.header.archiveHeader ? 1 + (section.header.expanded ? section.tracks.size() : 0) : section.tracks.size();
  result.reserve(rowCount);
  std::set<std::string> shownSongs;
  for (const auto &section : sections) {
    if (section.header.archiveHeader) {
      result.push_back(section.header);
      if (!section.header.expanded) continue;
    }
    for (const auto *track : section.tracks)
      result.push_back({.track = *track, .archivePath = section.header.archivePath,
          .songLeader = shownSongs.insert(track->groupId).second});
  }
  return result;
}
void ToggleStorageArchiveSelection(const std::filesystem::path &archive,
    const std::vector<music_playlist::MusicTrack> &visible,
    const StorageGroupArchives &archives, StorageGroupSelection &selection) {
  std::map<std::string, const music_playlist::MusicTrack *> groups;
  for (const auto &track : visible) {
    const auto found = archives.find(track.groupId);
    if (found != archives.end() && found->second == archive) groups.emplace(track.groupId, &track);
  }
  const bool allSelected = std::all_of(groups.begin(), groups.end(),
      [&](const auto &group) { return selection.Contains(group.first); });
  for (const auto &[id, track] : groups)
    if (selection.Contains(id) == allSelected) selection.Toggle(*track);
}
