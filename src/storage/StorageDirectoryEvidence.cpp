#include "StorageDirectoryEvidence.h"
#include "../BmsChartFile.h"
#include <algorithm>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
std::vector<fs::path> boundaries(const StorageDirectorySummary &summary) {
  return summary.accessRoot.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{summary.accessRoot};
}
bool active(Checkpoint check, std::string &error) {
  if (!check || check()) return true;
  error = "Cancelled"; return false;
}
}
StorageDirectoryEvidence::StorageDirectoryEvidence(StorageDirectorySummary &summary, const StoragePreflight &preflight,
    StorageDirectoryLimits limits, StorageEvidenceMetrics *metrics)
    : summary_(summary), preflight_(preflight), limits_(std::move(limits)), metrics_(metrics) {}
bool StorageDirectoryEvidence::Load(Checkpoint check, std::string &error) {
  directory_ = LoadStorageDirectory(summary_, preflight_.request, check, error, limits_);
  if (!directory_) return false;
  candidates_ = std::make_unique<StorageDirectoryInventory>(summary_.directory, limits_);
  std::string candidateError;
  if (!directory_->Visit(check, [&](const auto &entry) {
    StorageDiagnostic reason;
    const auto asset = StorageCandidateForEntry(entry, *directory_, preflight_.request.policy, {}, reason);
    if (reason.code == StorageReason::IoError) { candidateError = reason.detail; return false; }
    return !asset || candidates_->Add(entry, candidateError);
  }, error)) { if (!candidateError.empty()) error = candidateError; return false; }
  auto path = summary_.directory;
  for (;;) {
    const auto found = preflight_.directories.find(path);
    if (found != preflight_.directories.end()) {
      const auto &summary = found->second;
      auto ancestor = path == summary_.directory ? nullptr : LoadStorageDirectory(summary, preflight_.request, check, error, limits_);
      if (path != summary_.directory && !ancestor) return false;
      auto &entries = ancestor ? *ancestor : *directory_;
      auto charts = std::make_unique<StorageDirectoryInventory>(path, limits_);
      std::string storeError;
      if (!entries.Visit(check, [&](const auto &entry) {
        return entry.kind == StorageEntryKind::Directory || !asobmshow::bms_chart_file::isBmsChartPath(entry.name) ||
            charts->Add(entry, storeError);
      }, error)) { if (!storeError.empty()) error = storeError; return false; }
      // Only the stamp and path are needed once ancestor membership is verified.
      StorageDirectorySummary scope;
      scope.directory = path; scope.accessRoot = summary.accessRoot; scope.stamp = summary.stamp;
      scopes_.push_back({std::move(scope), std::move(charts)});
    }
    if (path == path.root_path()) break;
    path = path.parent_path();
  }
  return true;
}
bool StorageDirectoryEvidence::CheckScopes(bool changingLocal, Checkpoint check, std::string &error) const {
  for (const auto &scope : scopes_) {
    if (!active(check, error)) return false;
    StorageDirectoryStamp stamp;
    if (metrics_) ++metrics_->directoryChecks;
    if (!ReadStorageDirectoryStamp(scope.summary.directory, scope.summary.accessRoot, stamp, error)) return false;
    auto expected = scope.summary.directory == summary_.directory ? summary_.stamp : scope.summary.stamp;
    if (changingLocal && scope.summary.directory == summary_.directory) expected.modified = stamp.modified;
    if (stamp != expected) { error = "Directory changed since preflight"; return false; }
    const auto roots = boundaries(scope.summary);
    std::string chartError;
    if (!scope.charts->Visit(check, [&](const auto &entry) {
      if (!entry.identity) { chartError = "Chart identity cannot be verified"; return false; }
      if (metrics_) ++metrics_->chartChecks;
      const auto current = ReadFileIdentity(scope.summary.directory / entry.name, chartError, roots);
      if (!current || *current != *entry.identity) { chartError = "Chart changed since preflight"; return false; }
      return true;
    }, error)) { if (!chartError.empty()) error = chartError; return false; }
  }
  return true;
}
bool StorageDirectoryEvidence::Candidate(const AssetCandidate &asset, Checkpoint check, std::string &error) const {
  if (!CheckScopes(false, check, error)) return false;
  const auto identity = ReadFileIdentity(asset.source, error, boundaries(summary_));
  if (!identity || *identity != asset.identity) { error = "Source changed since preflight"; return false; }
  return true;
}
bool StorageDirectoryEvidence::CheckEntries(const std::vector<StorageFileChange> &owned, Checkpoint check,
    std::string &error, StorageDirectoryStamp &stamp) const {
  if (metrics_) ++metrics_->membershipEnumerations;
  if (!ReadStorageDirectoryStamp(summary_.directory, summary_.accessRoot, stamp, error)) return false;
  std::size_t expectedCount = directory_->Size(), actualCount = 0;
  for (const auto &change : owned) {
    const auto old = directory_->Find(change.path.filename(), error);
    if (!error.empty()) return false;
    if (!old && change.after) ++expectedCount;
    if (old && !change.after) --expectedCount;
  }
  std::error_code ec;
  fs::directory_iterator it(summary_.directory, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!active(check, error)) return false;
    if (metrics_) ++metrics_->membershipEntries;
    const auto name = it->path().filename();
    const auto change = std::ranges::find_if(owned, [&](const auto &item) { return item.path.filename() == name; });
    const bool expected = change != owned.end() ? change->after.has_value() : directory_->Find(name, error).has_value();
    if (!expected || !error.empty()) { error = "Unexpected directory entry changed during transaction"; return false; }
    ++actualCount;
  }
  StorageDirectoryStamp after;
  if (ec || actualCount != expectedCount ||
      !ReadStorageDirectoryStamp(summary_.directory, summary_.accessRoot, after, error) || after != stamp) {
    error = "Directory membership changed during transaction"; return false;
  }
  return true;
}
bool StorageDirectoryEvidence::BeforeCommit(const AssetCandidate &asset, const std::vector<StorageFileChange> &owned,
    Checkpoint check, std::string &error) const {
  error.clear();
  if (asset.source.parent_path() != summary_.directory || !CheckScopes(true, check, error)) return false;
  bool sourceChanged = false;
  for (const auto &change : owned) {
    if (change.path.parent_path() != summary_.directory) { error = "Unexpected transaction path"; return false; }
    sourceChanged |= change.path == asset.source;
    if (change.after) {
      if (ReadFileIdentity(change.path, error, boundaries(summary_)) != change.after) {
        error = "Transaction file identity changed"; return false;
      }
    } else {
      std::error_code ec; const auto status = fs::symlink_status(change.path, ec);
      if (fs::exists(status) || (ec && ec != std::errc::no_such_file_or_directory)) {
        error = "Foreign file appeared at transaction path"; return false;
      }
    }
  }
  if (!sourceChanged && ReadFileIdentity(asset.source, error, boundaries(summary_)) != std::optional(asset.identity)) {
    error = "Source changed before commit"; return false;
  }
  StorageDirectoryStamp stamp;
  return CheckEntries(owned, check, error, stamp);
}
bool StorageDirectoryEvidence::Advance(const AssetCandidate &asset, const FileResult &result, Checkpoint check, std::string &error) {
  if (!BeforeCommit(asset, result.filesystemChanges, check, error)) return false;
  for (const auto &change : result.filesystemChanges) {
    if (change.after) {
      std::error_code ec; const auto permissions = fs::status(change.path, ec).permissions();
      if (ec) { error = ec.message(); return false; }
      if (!directory_->Set({.name = change.path.filename(), .kind = StorageEntryKind::Regular,
          .bytes = change.after->bytes, .permissions = permissions, .identity = change.after}, error)) return false;
    } else if (!directory_->Remove(change.path.filename(), error)) return false;
  }
  return ReadStorageDirectoryStamp(summary_.directory, summary_.accessRoot, summary_.stamp, error);
}
bool StorageDirectoryEvidence::Finish(Checkpoint check, std::string &error) {
  // Compute from the owned baseline, never adopt a fresh filesystem snapshot.
  summary_.fingerprint = directory_->Fingerprint(std::move(check), error);
  return !summary_.fingerprint.empty();
}
}
