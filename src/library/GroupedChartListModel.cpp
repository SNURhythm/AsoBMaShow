#include "GroupedChartListModel.h"
#include "../path.h"
#include <algorithm>
#include <cctype>

namespace {
std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
bool matches(const music_playlist::MusicTrack &track, const std::string &query) {
  if (query.empty()) return true;
  const auto &meta = track.representativeChart;
  return lower(track.title + " " + track.subtitle + " " + track.artist + " " +
      track.subArtist + " " + track.genre + " " + track.trackId + " " +
      track.chartId + " " + meta.Title + " " + meta.SubTitle + " " +
      meta.Artist + " " + meta.SubArtist + " " + meta.Genre + " " + meta.MD5 +
      " " + meta.SHA256 + " " + fspath_to_utf8(meta.BmsPath) + " " +
      fspath_to_utf8(meta.Folder)).find(query) != std::string::npos;
}
}
void GroupedChartListModel::SetGroups(std::vector<Track> groups) {
  groups_ = std::move(groups);
  dirty_ = true;
}
void GroupedChartListModel::SetChildren(std::string groupId, std::vector<Track> children) {
  children_[std::move(groupId)] = std::move(children);
  dirty_ = true;
}
void GroupedChartListModel::SetExpanded(std::string groupId, bool expanded) {
  if (expanded) expanded_.insert(std::move(groupId));
  else expanded_.erase(groupId);
  dirty_ = true;
}
void GroupedChartListModel::SetQuery(std::string query) {
  const auto begin = std::find_if_not(query.begin(), query.end(),
      [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(query.rbegin(), query.rend(),
      [](unsigned char c) { return std::isspace(c); }).base();
  query_ = begin < end ? lower(std::string(begin, end)) : "";
  dirty_ = true;
}
void GroupedChartListModel::Rebuild() const {
  dirty_ = false;
  rows_.clear();
  for (const auto &group : groups_) {
    const auto children = children_.find(group.groupId);
    if (expanded_.contains(group.groupId) && children != children_.end()) {
      for (const auto &child : children->second)
        if (matches(child, query_)) rows_.push_back(child);
    } else if (matches(group, query_)) rows_.push_back(group);
  }
}
void StorageGroupSelection::SetGroups(std::vector<Track> groups) {
  groups_ = std::move(groups);
  available_.clear();
  for (const auto &group : groups_) available_.insert(group.groupId);
  std::erase_if(selected_, [this](const auto &id) { return !available_.contains(id); });
}
void StorageGroupSelection::Toggle(const Track &row) {
  if (row.groupId.empty() || !available_.contains(row.groupId)) return;
  if (!selected_.erase(row.groupId)) selected_.insert(row.groupId);
}
std::size_t StorageGroupSelection::HiddenCount(const std::vector<Track> &rows) const {
  auto hidden = selected_;
  for (const auto &row : rows) hidden.erase(row.groupId);
  return hidden.size();
}
std::vector<StorageGroupSelection::Track> StorageGroupSelection::SelectedGroups() const {
  std::vector<Track> result;
  for (const auto &group : groups_)
    if (selected_.contains(group.groupId)) result.push_back(group);
  return result;
}
