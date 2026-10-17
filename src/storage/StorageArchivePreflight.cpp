#include "StoragePreflight.h"
#include "StorageArchiveDb.h"
#include "StoragePathIndex.h"
#include "../Uuid.h"

namespace storage_management {
namespace fs = std::filesystem;
using namespace archive_detail;
namespace {
struct Scratch {
  fs::path path;
  ~Scratch() { if (!path.empty()) { std::error_code error; fs::remove_all(path, error); } }
};
}
void AppendStorageArchivePreflight(StoragePreflight &result, Checkpoint check) {
  std::map<fs::path, std::vector<std::size_t>> batches;
  for (std::size_t i = 0; i < result.groups.size(); ++i) {
    auto &group = result.groups[i]; if (group.sourceKind != StorageSourceKind::Archive) continue;
    fs::path archive, inner;
    if (!FindStorageArchive(group.root, archive, inner))
      group.reason = {StorageReason::UnsupportedSource, "Archive is not a writable local file"};
    else if (!result.request.pathAccessRoots.empty() && StoragePathIndex(result.request.pathAccessRoots).Ancestors(archive).empty())
      group.reason = {StorageReason::UnsafePath, "Archive is outside authorized storage roots"};
    else { group.root = inner.empty() ? archive : archive / inner; batches[archive].push_back(i); }
  }
  for (const auto &[path, positions] : batches) {
    try {
      active(check);
      auto root = result.request.scanScratchRoot;
      if (root.empty()) root = fs::canonical(fs::temp_directory_path()) / "asobmashow-storage-archive-scan";
      fs::create_directories(root);
      std::string error;
      const auto &privateRoot = result.request.scanAccessRoot;
      const std::vector<fs::path> scratchAccess = privateRoot.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{privateRoot};
      require(IsSafeLocalPath(root, error, scratchAccess), "Unsafe archive scan directory", StorageReason::UnsafePath);
      Scratch scratch;
      const auto privatePath = root / uuid::generateV4();
      require(fs::create_directory(privatePath), "Cannot create archive scan directory", StorageReason::IoError);
      scratch.path = privatePath; fs::permissions(privatePath, fs::perms::owner_all, fs::perm_options::replace);
      StorageArchive archive(path, privatePath / "manifest.sqlite", result.request.pathAccessRoots, check);
      StorageArchiveSummary summary{.path = path, .identity = archive.identity};
      std::vector<GroupStorageMeasurement> measured;
      for (const auto position : positions) {
        auto row = result.groups[position]; row.totalBytes = 0;
        row.sourceIdentity = "archive:" + std::to_string(archive.identity.device) + ":" + std::to_string(archive.identity.fileId) + ":" +
            std::to_string(archive.identity.bytes) + ":" + std::to_string(static_cast<long long>(archive.identity.modified.time_since_epoch().count()));
        measured.push_back(std::move(row));
        auto inner = result.groups[position].root.lexically_relative(path);
        if (inner == ".") inner.clear(); summary.innerRoots.push_back(std::move(inner));
      }
      const StoragePathIndex selected(summary.innerRoots);
      std::uint64_t total = 0, skippedCount = 0;
      std::vector<FileResult> skipped;
      std::optional<std::uint64_t> cursor;
      while (auto entry = archive.Next(cursor)) {
        cursor = entry->offset; active(check); if (entry->directory) continue;
        const auto inner = fs::u8path(entry->name);
        const auto matches = selected.Ancestors(inner); if (matches.empty()) continue;
        total = plus(total, entry->bytes);
        const auto kind = asobmshow::chart_assets::ClassifyMediaPath(inner);
        for (const auto position : matches) {
          auto &row = measured[position]; *row.totalBytes = plus(*row.totalBytes, entry->bytes);
          if (kind == asobmshow::chart_assets::MediaKind::Audio) row.audioBytes = plus(row.audioBytes, entry->bytes);
          if (kind == asobmshow::chart_assets::MediaKind::Video) row.videoBytes = plus(row.videoBytes, entry->bytes);
          if (kind == asobmshow::chart_assets::MediaKind::Image) row.imageBytes = plus(row.imageBytes, entry->bytes);
        }
        StorageDiagnostic reason;
        if (archive.Candidate(*entry, result.request.policy, reason)) {
          ++summary.eligibleCount; summary.eligibleBytes = plus(summary.eligibleBytes, entry->bytes);
        } else if (reason.code != StorageReason::None) {
          ++skippedCount;
          if (skipped.size() < 100) skipped.push_back({.source = path / inner, .reason = reason});
        }
      }
      for (std::size_t i = 0; i < positions.size(); ++i) result.groups[positions[i]] = std::move(measured[i]);
      summary.innerRoots = selected.MinimalRoots();
      result.eligibleCount = plus(result.eligibleCount, summary.eligibleCount);
      result.eligibleBytes = plus(result.eligibleBytes, summary.eligibleBytes);
      result.skippedCount = plus(result.skippedCount, skippedCount);
      for (auto &sample : skipped) if (result.skips.size() < 100) result.skips.push_back(std::move(sample));
      if (result.totalBytes) *result.totalBytes = plus(*result.totalBytes, total);
      result.archives.push_back(std::move(summary));
    } catch (const StorageArchiveError &error) {
      for (const auto position : positions) result.groups[position].reason = {error.code, error.what()};
      if (error.code == StorageReason::Cancelled) { result.cancelled = true; result.complete = false; break; }
    } catch (const std::exception &error) {
      for (const auto position : positions) result.groups[position].reason = {StorageReason::IoError, error.what()};
    }
  }
}
}
