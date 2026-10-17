#pragma once
#include "../audio/MusicPlaylist.h"
#include <map>
#include <set>

// Expanded children replace the group, matching the music browser's behavior.
class GroupedChartListModel {
public:
  using Track = music_playlist::MusicTrack;
  void SetGroups(std::vector<Track> groups);
  void SetChildren(std::string groupId, std::vector<Track> children);
  void SetExpanded(std::string groupId, bool expanded);
  void SetQuery(std::string query);
  const std::vector<Track> &Rows() const { if (dirty_) Rebuild(); return rows_; }
private:
  void Rebuild() const;
  std::vector<Track> groups_;
  mutable std::vector<Track> rows_;
  mutable bool dirty_ = true;
  std::map<std::string, std::vector<Track>> children_;
  std::set<std::string> expanded_;
  std::string query_;
};

// Selection always resolves to a group representative, including child actions.
class StorageGroupSelection {
public:
  using Track = music_playlist::MusicTrack;
  void SetGroups(std::vector<Track> groups);
  void Toggle(const Track &row);
  void Clear() { selected_.clear(); }
  bool Contains(const std::string &groupId) const { return selected_.contains(groupId); }
  std::size_t Count() const { return selected_.size(); }
  std::size_t HiddenCount(const std::vector<Track> &rows) const;
  std::vector<Track> SelectedGroups() const;
private:
  std::vector<Track> groups_;
  std::set<std::string> selected_;
};
