#include "StorageArchiveMeasurement.h"
#include "StoragePathIndex.h"
#include "../ArchiveFile.h"
#include "../ArchiveSourceIdentity.h"
#include "../archive/ArchiveSourceAccess.h"

#include <limits>
#include <stdexcept>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
void add(std::uint64_t &value, std::uint64_t bytes) {
  if (bytes > std::numeric_limits<std::uint64_t>::max() - value) throw std::overflow_error("Archive size overflow");
  value += bytes;
}
}
bool SplitStorageArchivePath(const fs::path &path, fs::path &archive, fs::path &inner,
                             std::map<fs::path, bool> &knownBoundaries) {
  archive.clear(); inner.clear();
  for (const auto &part : path) if (part == "..") return false;
  fs::path candidate;
  bool found = false;
  for (const auto &part : path.lexically_normal()) {
    if (found) { if (!part.empty()) inner /= part; continue; }
    candidate /= part;
    if (!archive_file::hasSupportedArchiveExtension(candidate)) continue;
    auto [entry, inserted] = knownBoundaries.try_emplace(candidate, false);
    if (inserted) {
      std::error_code error;
      entry->second = archive_source::validReference(candidate) || fs::is_regular_file(candidate, error);
    }
    if (entry->second) { archive = candidate; found = true; }
  }
  return found;
}
std::vector<GroupStorageMeasurement> MeasureArchiveGroups(const fs::path &archive,
    std::span<const FolderSelection> groups, Checkpoint check, StorageScanMetrics *metrics) {
  std::vector<GroupStorageMeasurement> result;
  std::vector<fs::path> innerRoots;
  std::vector<std::size_t> validGroups;
  const auto identity = archive_source_identity::KeyForPath(archive);
  for (const auto &group : groups) {
    auto root = (group.chartPathFallback ? group.path.parent_path() : group.path).lexically_normal();
    while (root != root.root_path() && root.filename().empty()) root = root.parent_path();
    GroupStorageMeasurement measurement{.groupId = group.groupId, .root = root,
        .sourceKind = StorageSourceKind::Archive, .sourceIdentity = identity};
    auto relative = root.lexically_relative(archive);
    bool valid = !relative.empty();
    for (const auto &part : relative) if (part == "..") valid = false;
    if (relative == ".") relative.clear();
    if (valid) {
      validGroups.push_back(result.size());
      innerRoots.push_back(relative);
      measurement.totalBytes = 0;
    } else measurement.reason = {StorageReason::UnsafePath, "Archive folder is outside its source"};
    result.push_back(std::move(measurement));
  }
  if (validGroups.empty()) return result;
  const StoragePathIndex roots(innerRoots);
  std::map<fs::path, StorageScanTotals> directories;
  bool solid = false, cancelled = false;
  std::string error;
  bool completed = false;
  try {
    if (metrics) ++metrics->archiveIndexVisits;
    completed = archive_file::visitIndexEntries(archive, [&](const auto &entry) {
      if (check && !check()) { cancelled = true; return false; }
      if (metrics) ++metrics->archiveEntries;
      if (entry.solid) { solid = true; return false; }
      if (entry.directory) { directories.try_emplace(entry.path); return true; }
      auto &totals = directories[entry.path.parent_path()];
      add(totals.bytes, entry.size); ++totals.files;
      const auto kind = asobmshow::chart_assets::ClassifyMediaPath(entry.path);
      using asobmshow::chart_assets::MediaKind;
      if (kind == MediaKind::Audio) add(totals.audio, entry.size);
      if (kind == MediaKind::Video) add(totals.video, entry.size);
      if (kind == MediaKind::Image) add(totals.image, entry.size);
      return true;
    }, &error, [&] {
      if (check && !check()) cancelled = true;
      return !cancelled;
    });
    if (completed) {
      std::vector<bool> seen(validGroups.size());
      for (std::size_t i = 0; i < innerRoots.size(); ++i) seen[i] = innerRoots[i].empty();
      for (const auto &[directory, totals] : directories) {
        for (const auto index : roots.Ancestors(directory, metrics ? &metrics->rootIndexSteps : nullptr)) {
          seen[index] = true;
          auto &measurement = result[validGroups[index]];
          add(*measurement.totalBytes, totals.bytes); add(measurement.audioBytes, totals.audio);
          add(measurement.videoBytes, totals.video); add(measurement.imageBytes, totals.image);
        }
      }
      for (std::size_t i = 0; i < seen.size(); ++i) if (!seen[i]) {
        auto &measurement = result[validGroups[i]];
        measurement.totalBytes.reset();
        measurement.reason = {StorageReason::StaleInput, "Folder is absent from the archive index"};
      }
    }
  } catch (const std::exception &failure) { completed = false; error = failure.what(); }
  if (!completed) for (auto &measurement : result) {
    measurement.totalBytes.reset();
    measurement.audioBytes = measurement.videoBytes = measurement.imageBytes = 0;
    measurement.reason = {solid ? StorageReason::UnsupportedSource :
        cancelled ? StorageReason::Cancelled : StorageReason::IncompleteInventory,
        solid ? "Solid archives are not chart sources" : cancelled ? "Cancelled" : error};
  }
  return result;
}
}
