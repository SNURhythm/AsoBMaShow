#include "StorageMeasurementState.h"
#include "StoragePathIndex.h"
#include <limits>
namespace storage_management {
namespace {
std::filesystem::path root(const FolderSelection &group) {
  return (group.chartPathFallback ? group.path.parent_path() : group.path).lexically_normal();
}
bool adjust(std::uint64_t &value, std::uint64_t before, std::uint64_t after) {
  if (before > value || after > std::numeric_limits<std::uint64_t>::max() - (value - before)) return false;
  value = value - before + after; return true;
}
}
std::set<std::string> ReconcileStorageMeasurements(StorageMeasurementRows &rows,
    std::map<std::string, StorageGroupSource> &previous, const std::vector<StorageGroupSource> &current, bool invalidateAll,
    const std::vector<std::filesystem::path> &changedRoots) {
  std::set<std::string> refresh;
  std::map<std::string, StorageGroupSource> next;
  std::vector<std::filesystem::path> changedPaths = changedRoots, paths;
  for (const auto &source : current) {
    const auto &group = source.selection;
    const auto old = previous.find(group.groupId);
    if (invalidateAll || old == previous.end() || old->second.revision != source.revision ||
        root(old->second.selection) != root(group) || old->second.selection.sourceKind != group.sourceKind) {
      changedPaths.push_back(root(group));
      if (old != previous.end()) changedPaths.push_back(root(old->second.selection));
      refresh.insert(group.groupId);
    }
    next[group.groupId] = source; paths.push_back(root(group));
  }
  for (const auto &[id, source] : previous) if (!next.contains(id)) changedPaths.push_back(root(source.selection));
  const StoragePathIndex groupIndex(paths, StoragePathComparison::CaseSensitive),
      changedIndex(changedPaths, StoragePathComparison::CaseSensitive);
  for (const auto &path : changedPaths)
    for (const auto index : groupIndex.Ancestors(path)) {
      const auto &group = current[index].selection;
      rows[group.groupId] = {.groupId = group.groupId, .root = paths[index],
          .reason = {StorageReason::StaleInput, "Cached total needs refresh"}, .sourceKind = group.sourceKind};
    }
  for (std::size_t i = 0; i < current.size(); ++i)
    if (!changedIndex.Ancestors(paths[i]).empty()) refresh.insert(current[i].selection.groupId);
  for (const auto &id : refresh) rows.erase(id);
  std::erase_if(rows, [&](const auto &row) { return !next.contains(row.first); });
  previous = std::move(next); return refresh;
}
std::set<std::string> ApplyStorageReportMeasurements(StorageMeasurementRows &rows,
    const std::vector<FolderSelection> &groups, const StorageReport &report) {
  std::vector<std::filesystem::path> paths;
  for (const auto &group : groups) paths.push_back(root(group));
  const StoragePathIndex groupIndex(paths, StoragePathComparison::CaseSensitive),
      changedIndex(report.changedRoots, StoragePathComparison::CaseSensitive);
  std::set<std::string> affected;
  for (const auto &changed : report.changedRoots)
    for (const auto index : groupIndex.Ancestors(changed)) affected.insert(groups[index].groupId);
  for (std::size_t i = 0; i < groups.size(); ++i)
    if (!changedIndex.Ancestors(paths[i]).empty()) affected.insert(groups[i].groupId);
  for (const auto &delta : report.groupDeltas) {
    const auto found = rows.find(delta.groupId);
    bool applied = false;
    if (!delta.needsRefresh && found != rows.end() && delta.baselineBytes &&
        found->second.totalBytes == delta.baselineBytes && !delta.sourceIdentity.empty() &&
        found->second.sourceIdentity == delta.sourceIdentity && found->second.generation == delta.baselineGeneration) {
      auto measurement = found->second;
      if (adjust(*measurement.totalBytes, delta.bytesBefore, delta.bytesAfter) &&
          adjust(measurement.audioBytes, delta.audioBefore, delta.audioAfter) &&
          adjust(measurement.videoBytes, delta.videoBefore, delta.videoAfter) &&
          adjust(measurement.imageBytes, delta.imageBefore, delta.imageAfter)) {
        measurement.sourceIdentity = delta.nextSourceIdentity;
        found->second = std::move(measurement); applied = true;
      }
    }
    if (applied) affected.erase(delta.groupId);
    else affected.insert(delta.groupId);
  }
  std::set<std::string> refresh;
  for (std::size_t i = 0; i < groups.size(); ++i) if (affected.contains(groups[i].groupId)) {
    const auto &group = groups[i];
    rows[group.groupId] = {.groupId = group.groupId, .root = paths[i],
        .reason = {StorageReason::StaleInput, "Cached total needs refresh"}, .sourceKind = group.sourceKind};
    // A strict ancestor would recursively re-enumerate unrelated siblings.
    // Keep its estimate unavailable until explicit Refresh; auto-refresh only
    // rows contained in the physical changed subtree union.
    if (!changedIndex.Ancestors(paths[i]).empty()) refresh.insert(group.groupId);
  }
  return refresh;
}
}
