#include "StoragePreflight.h"
#include "StoragePathIndex.h"
#include "../BmsChartFile.h"
#include <algorithm>
#include <limits>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
using asobmshow::chart_assets::MediaKind;
void add(std::uint64_t &to, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - to) throw std::overflow_error("Storage size overflow");
  to += value;
}
bool writable(fs::perms value) {
  return (value & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) != fs::perms::none;
}
void skip(StorageDirectorySummary &summary, const fs::path &path, StorageDiagnostic reason) {
  ++summary.skippedCount; ++summary.reasons[reason.code];
  if (summary.samples.size() < 100) summary.samples.push_back({.source = path, .reason = std::move(reason)});
}
}
std::optional<AssetCandidate> StorageCandidateForEntry(const StorageDirectoryEntry &entry,
    const StorageDirectoryInventory &directory, const StoragePolicy &policy,
    const std::vector<std::string> &groups, StorageDiagnostic &reason) {
  reason = {};
  if (entry.kind == StorageEntryKind::Directory) return {};
  if (entry.kind != StorageEntryKind::Regular || !entry.identity) {
    reason = {StorageReason::UnsafePath, "Symlink, hard link or unsupported directory entry"}; return {};
  }
  if (!IsValid(policy)) return {};
  const auto path = directory.Directory() / entry.name;
  const auto kind = asobmshow::chart_assets::ClassifyMediaPath(path);
  const auto extension = asobmshow::chart_assets::LowerMediaExtension(path);
  AssetCandidate candidate{.source = path, .destination = path, .kind = kind, .groups = groups, .identity = *entry.identity};
  if (kind == MediaKind::Audio && extension == "wav" && policy.audio == AudioAction::LosslessFlac) {
    candidate.operation = AssetOperation::LosslessFlac; candidate.destination.replace_extension("flac");
  } else if (kind == MediaKind::Audio && (extension == "wav" || extension == "flac") && policy.audio == AudioAction::LossyVorbis) {
    candidate.operation = AssetOperation::LossyVorbis; candidate.destination.replace_extension("ogg");
  } else if (kind == MediaKind::Video && policy.bga != BgaAction::Keep) {
    candidate.operation = policy.bga == BgaAction::Delete ? AssetOperation::Delete : AssetOperation::CompressH264;
    if (extension != "mp4") candidate.destination.replace_extension("mp4");
  } else return {};
  if (candidate.operation != AssetOperation::Delete) {
    std::string error;
    const auto destination = candidate.destination != path ? directory.Find(candidate.destination.filename(), error) : std::nullopt;
    if (!error.empty()) { reason = {StorageReason::IoError, error}; return {}; }
    if (destination) {
      if (destination->kind != StorageEntryKind::Regular || !destination->identity) {
        reason = {StorageReason::UnsafePath, "Existing destination is not a safe regular file"}; return {};
      }
      // Review captures metadata only. Apply must validate the complete retained
      // media before this existing destination can authorize source retirement.
      candidate.reuseDestination = true;
      candidate.existingDestinationIdentity = destination->identity;
    } else {
      const auto count = directory.StemCount(entry.name, kind == MediaKind::Audio, error);
      if (!error.empty()) { reason = {StorageReason::IoError, error}; return {}; }
      if (count > 1) { reason = {StorageReason::Collision, "Same-stem media alternatives are ambiguous"}; return {}; }
    }
  }
  return candidate;
}
StoragePreflight BuildStoragePreflight(const InventoryRequest &request, Checkpoint check,
    StorageScanMetrics *metrics, StorageScanDependencies deps) {
  StoragePreflight result;
  result.request = request; result.totalBytes = 0;
  std::vector<fs::path> roots;
  std::map<fs::path, std::vector<std::size_t>, StoragePathLess> positions;
  const StoragePathIndex access(request.pathAccessRoots);
  for (const auto &group : request.groups) {
    GroupStorageMeasurement measured{.groupId = group.groupId, .root = group.path, .sourceKind = group.sourceKind};
    auto root = group.chartPathFallback ? group.path.parent_path() : group.path;
    if (group.sourceKind == StorageSourceKind::Archive) {
      measured.root = root;
      result.groups.push_back(std::move(measured));
      continue;
    }
    if (group.sourceKind != StorageSourceKind::Local || !group.writableLocal)
      measured.reason = {StorageReason::UnsupportedSource, "Compression requires a writable local folder"};
    else if (!root.is_absolute() || std::ranges::any_of(root, [](const auto &part) { return part == ".."; }))
      measured.reason = {StorageReason::UnsafePath, "Expected a normalized absolute folder"};
    else {
      root = root.lexically_normal();
      while (root != root.root_path() && root.filename().empty()) root = root.parent_path();
      if (!request.pathAccessRoots.empty() && access.Ancestors(root).empty())
        measured.reason = {StorageReason::UnsafePath, "Folder is outside authorized storage roots"};
      else {
        measured.root = root; roots.push_back(root); positions[root].push_back(result.groups.size());
      }
    }
    result.groups.push_back(std::move(measured));
  }
  std::vector<StorageScanJob> jobs;
  for (const auto &root : StoragePathIndex(roots).MinimalRoots()) {
    fs::path boundary;
    const auto ancestors = access.Ancestors(root);
    if (!ancestors.empty()) boundary = request.pathAccessRoots[ancestors.back()];
    jobs.push_back({.path = root, .accessRoot = boundary});
  }
  const auto checkpoint = [&] { return !deps.limits.stopToken.stop_requested() && (!check || check()); };
  const bool complete = RunStorageScan(jobs, deps.limits, [&](const auto &job, auto active, auto sink) {
    StorageScanBatch batch;
    auto summary = std::make_shared<StorageDirectorySummary>();
    summary->directory = job.path; summary->accessRoot = job.accessRoot;
    StorageDirectoryInventory directory(job.path, {.scratchRoot = deps.limits.scratchRoot});
    StorageDirectoryInventory charts(job.path, {.scratchRoot = deps.limits.scratchRoot});
    std::string error, storeError;
    bool scanned = deps.enumerate(job.path, job.accessRoot, StorageVisitMode::Preflight, active,
        [&](const StorageDirectoryEntry &entry) {
          if (!directory.Add(entry, storeError)) return false;
          if ((entry.kind == StorageEntryKind::Directory || asobmshow::bms_chart_file::isBmsChartPath(entry.name)) &&
              !charts.Add(entry, storeError)) return false;
          if (entry.kind == StorageEntryKind::Directory) {
            summary->childDirectories.push_back(entry.name);
            batch.children.push_back({.path = job.path / entry.name});
            if (batch.children.size() == 128) {
              StorageScanBatch children; children.children = std::move(batch.children);
              if (!sink(std::move(children))) return false;
            }
          } else if (entry.kind == StorageEntryKind::Regular) {
            add(batch.totals.bytes, entry.bytes); ++batch.totals.files;
            const auto kind = asobmshow::chart_assets::ClassifyMediaPath(entry.name);
            if (kind == MediaKind::Audio) add(batch.totals.audio, entry.bytes);
            if (kind == MediaKind::Video) add(batch.totals.video, entry.bytes);
            if (kind == MediaKind::Image) add(batch.totals.image, entry.bytes);
          }
          return true;
        }, batch.stamp, &batch.metrics, error);
    if (!storeError.empty()) error = storeError;
    summary->stamp = batch.stamp; summary->totals = batch.totals;
    if (scanned) {
      summary->fingerprint = directory.Fingerprint(active, error);
      scanned = !summary->fingerprint.empty();
      if (scanned) {
        summary->chartFingerprint = charts.Fingerprint(active, error);
        scanned = !summary->chartFingerprint.empty();
      }
    }
    if (scanned) scanned = directory.Visit(active, [&](const auto &entry) {
      StorageDiagnostic reason;
      const auto candidate = StorageCandidateForEntry(entry, directory, request.policy, {}, reason);
      if (reason.code == StorageReason::IoError) { storeError = reason.detail; return false; }
      if (candidate && !writable(batch.stamp.permissions))
        reason = {StorageReason::UnsupportedSource, "Parent folder is not writable"};
      if (reason.code != StorageReason::None) skip(*summary, job.path / entry.name, std::move(reason));
      else if (candidate) { ++summary->eligibleCount; add(summary->eligibleBytes, entry.bytes); }
      return true;
    }, error);
    if (!scanned) {
      if (!storeError.empty()) error = storeError;
      summary->reason = batch.reason = {StorageReason::IncompleteInventory, std::move(error)};
      summary->eligibleCount = summary->eligibleBytes = 0;
    }
    batch.summary = std::move(summary); sink(std::move(batch));
  }, [&](StorageScanBatch batch) {
    if (batch.directoryComplete) {
      if (metrics) {
        add(metrics->entries, batch.metrics.entries); add(metrics->metadataReads, batch.metrics.metadataReads);
        add(metrics->rootIndexSteps, batch.metrics.rootIndexSteps);
      }
      if (batch.summary) {
        auto summary = std::move(*batch.summary);
        add(result.eligibleCount, summary.eligibleCount); add(result.eligibleBytes, summary.eligibleBytes);
        add(result.skippedCount, summary.skippedCount);
        for (auto &sample : summary.samples) if (result.skips.size() < 100) result.skips.push_back(std::move(sample));
        summary.samples.clear(); summary.samples.shrink_to_fit();
        result.directories.emplace(batch.job.path, std::move(summary));
      }
    }
    if (batch.subtreeComplete) {
      if (!batch.job.parentId) {
        if (result.totalBytes && batch.reason.code == StorageReason::None) add(*result.totalBytes, batch.totals.bytes);
        else result.totalBytes.reset();
      }
      if (const auto found = positions.find(batch.job.path); found != positions.end()) for (const auto position : found->second) {
        auto &measured = result.groups[position]; measured.reason = batch.reason;
        if (batch.reason.code == StorageReason::None) {
          measured.sourceIdentity = StorageDirectorySourceIdentity(batch.stamp);
          measured.totalBytes = batch.totals.bytes; measured.audioBytes = batch.totals.audio;
          measured.videoBytes = batch.totals.video; measured.imageBytes = batch.totals.image;
        }
      }
    }
    return true;
  }, checkpoint);
  result.cancelled = !checkpoint(); result.complete = complete && !result.cancelled;
  AppendStorageArchivePreflight(result, checkpoint, metrics);
  for (auto &group : result.groups) if (!group.totalBytes) {
    result.totalBytes.reset();
    if (group.reason.code == StorageReason::None) {
      result.complete = false;
      group.reason = {result.cancelled ? StorageReason::Cancelled : StorageReason::IncompleteInventory, "Preflight did not complete"};
    }
  }
  if (!result.complete) result.totalBytes.reset();
  if (result.cancelled) {
    result.eligibleCount = result.eligibleBytes = 0;
    for (auto &group : result.groups) group.totalBytes.reset();
  }
  return result;
}
std::vector<fs::path> StoragePreflight::EligibleRoots() const {
  if (!complete || cancelled) return {};
  std::vector<fs::path> roots;
  // Keep the whole selected folder reserved, including parent chart files that
  // may reference eligible media in a descendant. Omit rejected selections.
  for (const auto &group : groups) {
    const auto found = directories.find(group.root);
    if (found != directories.end() && found->second.reason.code == StorageReason::None)
      roots.push_back(group.root);
  }
  const StoragePathIndex selected(roots);
  std::vector<fs::path> eligible;
  for (const auto &archive : archives) if (archive.eligibleCount) eligible.push_back(archive.path);
  for (const auto &[path, summary] : directories) if (summary.eligibleCount && summary.reason.code == StorageReason::None) {
    const auto ancestors = selected.Ancestors(path);
    if (!ancestors.empty()) eligible.push_back(roots[ancestors.front()]);
  }
  return StoragePathIndex(std::move(eligible)).MinimalRoots();
}
std::unique_ptr<StorageDirectoryInventory> LoadStorageDirectory(const StorageDirectorySummary &summary,
    const InventoryRequest &, Checkpoint check, std::string &error, StorageDirectoryLimits limits) {
  error.clear();
  if (summary.reason.code != StorageReason::None || summary.fingerprint.empty()) {
    error = "Directory preflight is incomplete"; return {};
  }
  auto directory = std::make_unique<StorageDirectoryInventory>(summary.directory, std::move(limits));
  StorageDirectoryStamp stamp;
  std::string storeError;
  if (!VisitStorageDirectory(summary.directory, summary.accessRoot, StorageVisitMode::Preflight, check,
      [&](const auto &entry) { return directory->Add(entry, storeError); }, stamp, nullptr, error)) {
    if (!storeError.empty()) error = storeError;
    return {};
  }
  if (stamp != summary.stamp) { error = "Directory changed since preflight"; return {}; }
  const auto fingerprint = directory->Fingerprint(std::move(check), error);
  if (fingerprint.empty() || fingerprint != summary.fingerprint) {
    if (error.empty()) error = "Directory contents changed since preflight";
    return {};
  }
  return directory;
}
}
