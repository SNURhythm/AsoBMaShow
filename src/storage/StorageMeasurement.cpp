#include "StorageMeasurement.h"
#include "StorageArchiveMeasurement.h"
#include "StoragePathIndex.h"
#include "../ArchiveFile.h"

#include <limits>
#include <mutex>
#include <unordered_map>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
void add(std::uint64_t &value, std::uint64_t bytes) {
  if (bytes > std::numeric_limits<std::uint64_t>::max() - value) throw std::overflow_error("Storage size overflow");
  value += bytes;
}
void add(StorageScanMetrics &to, const StorageScanMetrics &from) {
  to.entries += from.entries; to.metadataReads += from.metadataReads; to.rootIndexSteps += from.rootIndexSteps;
  to.archiveIndexVisits += from.archiveIndexVisits; to.archiveEntries += from.archiveEntries;
  to.peakArchiveAggregationRecords = std::max(to.peakArchiveAggregationRecords, from.peakArchiveAggregationRecords);
}
fs::path normalizedRoot(const FolderSelection &group) {
  auto path = group.chartPathFallback ? group.path.parent_path() : group.path;
  if (!path.is_absolute()) return {};
  for (const auto &part : path) if (part == "..") return {};
  path = path.lexically_normal();
  while (path != path.root_path() && path.filename().empty()) path = path.parent_path();
  return path;
}
struct ArchiveWork {
  fs::path path;
  std::vector<FolderSelection> groups;
  std::vector<std::size_t> positions;
};
}
StorageMeasurements MeasureStorageGroups(const InventoryRequest &request, Checkpoint check,
    MeasurementCallback publish, StorageScanMetrics *metrics, StorageScanDependencies deps) {
  StorageMeasurements result;
  result.totalBytes = 0;
  std::vector<fs::path> localRoots;
  std::map<fs::path, std::vector<std::size_t>, StoragePathLess> localGroups;
  std::map<fs::path, bool> archiveBoundaries;
  std::unordered_map<std::string, std::size_t> archivePositions;
  std::vector<ArchiveWork> archives;
  const StoragePathIndex accessRoots(request.pathAccessRoots);
  for (const auto &group : request.groups) {
    const auto position = result.groups.size();
    GroupStorageMeasurement measurement{.groupId = group.groupId, .root = group.path, .sourceKind = group.sourceKind};
    if (group.sourceKind == StorageSourceKind::Archive) {
      fs::path archive, inner;
      const auto path = group.chartPathFallback ? group.path.parent_path() : group.path;
      if (SplitStorageArchivePath(path, archive, inner, archiveBoundaries)) {
        const auto key = archive_file::cacheKeyForPath(archive);
        auto [found, inserted] = archivePositions.try_emplace(key, archives.size());
        if (inserted) archives.push_back({.path = archive});
        auto &bundle = archives[found->second];
        auto selected = group;
        selected.path = inner.empty() ? bundle.path : bundle.path / inner;
        selected.chartPathFallback = false;
        bundle.groups.push_back(std::move(selected)); bundle.positions.push_back(position);
      } else measurement.reason = {StorageReason::UnsupportedSource, "Archive source is unavailable"};
    } else if (group.sourceKind == StorageSourceKind::Local) {
      const auto root = normalizedRoot(group);
      if (root.empty()) measurement.reason = {StorageReason::UnsafePath, "Expected a normalized absolute folder"};
      else if (!request.pathAccessRoots.empty() && accessRoots.Ancestors(root).empty())
        measurement.reason = {StorageReason::UnsafePath, "Folder is outside authorized storage roots"};
      else { measurement.root = root; localRoots.push_back(root); localGroups[root].push_back(position); }
    } else measurement.reason = {StorageReason::UnsupportedSource, "Storage source is unsupported"};
    result.groups.push_back(std::move(measurement));
  }
  std::vector<StorageScanJob> jobs;
  for (const auto &root : StoragePathIndex(localRoots).MinimalRoots()) {
    fs::path access;
    const auto ancestors = accessRoots.Ancestors(root);
    if (!ancestors.empty()) access = request.pathAccessRoots[ancestors.back()];
    jobs.push_back({.path = root, .accessRoot = access});
  }
  for (std::size_t i = 0; i < archives.size(); ++i)
    jobs.push_back({.kind = StorageScanJobKind::Archive, .path = archives[i].path, .context = i + 1});
  std::mutex archiveMutex;
  std::unordered_map<std::uint64_t, std::vector<GroupStorageMeasurement>> archiveResults;
  bool cancelled = false;
  const auto checkpoint = [&] {
    if (check && !check()) return false;
    return !deps.limits.stopToken.stop_requested();
  };
  const bool scanned = RunStorageScan(jobs, deps.limits, [&](const auto &job, auto active, auto sink) {
    if (job.kind == StorageScanJobKind::Archive) {
      StorageScanBatch batch;
      auto groups = MeasureArchiveGroups(job.path, archives[job.context - 1].groups, active, &batch.metrics);
      { std::lock_guard lock(archiveMutex); archiveResults.emplace(job.context, std::move(groups)); }
      sink(std::move(batch));
      return;
    }
    StorageScanBatch batch;
    std::string error;
    const bool complete = deps.enumerate(job.path, job.accessRoot, StorageVisitMode::SizeOnly, active,
        [&](const StorageDirectoryEntry &entry) {
          if (entry.kind == StorageEntryKind::Directory) {
            batch.children.push_back({.path = job.path / entry.name});
            if (batch.children.size() == 128) {
              StorageScanBatch children;
              children.children = std::move(batch.children);
              if (!sink(std::move(children))) return false;
            }
          } else if (entry.kind == StorageEntryKind::Regular) {
            add(batch.totals.bytes, entry.bytes); ++batch.totals.files;
            using asobmshow::chart_assets::MediaKind;
            const auto kind = asobmshow::chart_assets::ClassifyMediaPath(entry.name);
            if (kind == MediaKind::Audio) add(batch.totals.audio, entry.bytes);
            if (kind == MediaKind::Video) add(batch.totals.video, entry.bytes);
            if (kind == MediaKind::Image) add(batch.totals.image, entry.bytes);
          }
          return true;
        }, batch.stamp, &batch.metrics, error);
    if (!complete) batch.reason = {StorageReason::IncompleteInventory, std::move(error)};
    sink(std::move(batch));
  }, [&](StorageScanBatch batch) {
    if (batch.directoryComplete && metrics) add(*metrics, batch.metrics);
    if (batch.job.kind == StorageScanJobKind::Archive && batch.directoryComplete) {
      const auto &bundle = archives[batch.job.context - 1];
      std::vector<GroupStorageMeasurement> completed;
      { std::lock_guard lock(archiveMutex); completed = std::move(archiveResults.at(batch.job.context)); archiveResults.erase(batch.job.context); }
      std::vector<fs::path> roots;
      std::map<fs::path, std::optional<std::uint64_t>> totals;
      for (std::size_t i = 0; i < completed.size(); ++i) {
        roots.push_back(completed[i].root); totals[completed[i].root] = completed[i].totalBytes;
        result.groups[bundle.positions[i]] = completed[i];
      }
      for (const auto &root : StoragePathIndex(roots).MinimalRoots()) {
        if (result.totalBytes && totals.at(root)) add(*result.totalBytes, *totals.at(root));
        else result.totalBytes.reset();
      }
      if (publish) publish(completed);
    } else if (batch.job.kind == StorageScanJobKind::Directory && batch.subtreeComplete) {
      result.directories[batch.job.path] = {batch.stamp, batch.totals, batch.reason};
      if (!batch.job.parentId) {
        if (result.totalBytes && batch.reason.code == StorageReason::None) add(*result.totalBytes, batch.totals.bytes);
        else result.totalBytes.reset();
      }
      const auto found = localGroups.find(batch.job.path);
      if (found == localGroups.end()) return true;
      std::vector<GroupStorageMeasurement> completed;
      for (const auto position : found->second) {
        auto &measurement = result.groups[position];
        measurement.reason = batch.reason;
        if (batch.reason.code == StorageReason::None) {
          measurement.totalBytes = batch.totals.bytes;
          measurement.audioBytes = batch.totals.audio; measurement.videoBytes = batch.totals.video;
          measurement.imageBytes = batch.totals.image; measurement.sourceIdentity = StorageDirectorySourceIdentity(batch.stamp);
        }
        completed.push_back(measurement);
      }
      if (publish) publish(completed);
    }
    return true;
  }, checkpoint);
  if (!scanned) cancelled = !checkpoint();
  result.cancelled = cancelled;
  result.complete = scanned;
  for (auto &measurement : result.groups) if (!measurement.totalBytes) {
    result.complete = false;
    if (measurement.reason.code == StorageReason::None)
      measurement.reason = {cancelled ? StorageReason::Cancelled : StorageReason::IncompleteInventory,
                            cancelled ? "Cancelled" : "Folder measurement did not complete"};
  }
  if (!result.complete || (!localRoots.empty() && !archives.empty())) result.totalBytes.reset();
  return result;
}
}
