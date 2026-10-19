#include "StorageFileTransaction.h"
#include "StorageReportDetail.h"
#include "../AtomicFile.h"
#include "../Uuid.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <system_error>

namespace storage_management {
namespace fs = std::filesystem;
namespace {
using json = nlohmann::json;
enum class Phase { Copying, Prepared, Published, Retired, Committed };
struct Record {
  std::string id;
  AssetOperation operation = AssetOperation::Delete;
  bool reuse = false;
  Phase phase = Phase::Prepared;
  fs::path source, destination, temporary, backup, journal;
  FileIdentity sourceIdentity, outputIdentity;
  std::string sourceHash, outputHash;
};
bool checkpoint(const Checkpoint &check) { return !check || check(); }
std::string utf8(const fs::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
fs::path pathFromUtf8(const std::string &value) {
  return fs::path(std::u8string(value.begin(), value.end()));
}
bool entryExists(const fs::path &path) {
  if (path.empty()) return false;
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  return fs::exists(status) || (ec && ec != std::errc::no_such_file_or_directory);
}
void cleanupAbandonedCandidates(const StoragePlatformPaths &paths,
                                const FileTransactionDependencies &dependencies,
                                const Checkpoint &check, StorageReport &report) {
  if (!entryExists(paths.temporaryRoot)) return;
  std::string error;
  auto failed = [&](const fs::path &path) {
    report.recoveryRequired = report.cleanupPending = true;
    ++report.failed;
    RetainStorageReportDetail(report, {.source = path, .outcome = FileOutcome::Failed,
        .reason = {StorageReason::CleanupFailed, error}});
  };
  if (!IsSafeLocalPath(paths.temporaryRoot, error, paths.pathAccessRoots)) {
    error = "Private candidate cleanup path is unsafe"; failed(paths.temporaryRoot); return;
  }
  std::error_code ec;
  fs::directory_iterator jobs(paths.temporaryRoot, ec), end;
  for (; !ec && jobs != end; jobs.increment(ec)) {
    if (!checkpoint(check)) { report.cancelled = true; return; }
    const auto job = jobs->path();
    if (!uuid::isCanonicalLowerV4(job.filename().string()) || !IsSafeLocalPath(job, error, paths.pathAccessRoots) ||
        !fs::is_directory(jobs->symlink_status(ec))) continue;
    // Only the optimizer's flat, known candidate files establish ownership.
    // Preserve foreign entries, links and future job formats without following them.
    bool recognized = true;
    std::vector<std::pair<fs::path, FileIdentity>> candidates;
    fs::directory_iterator files(job, ec);
    for (; !ec && files != end; files.increment(ec)) {
      const auto path = files->path();
      const auto name = path.filename();
      const auto stem = path.stem().string();
      const auto extension = asobmshow::chart_assets::LowerMediaExtension(path);
      const bool workerCandidate = stem.starts_with("candidate-") && uuid::isCanonicalLowerV4(stem.substr(10)) &&
          (extension == "flac" || extension == "ogg" || extension == "mp4");
      if (!workerCandidate && name != "candidate.flac" && name != "candidate.ogg" && name != "candidate.mp4") {
        recognized = false; break;
      }
      const auto identity = ReadFileIdentity(path, error, paths.pathAccessRoots);
      if (!identity) { recognized = false; break; }
      candidates.emplace_back(path, *identity);
    }
    if (ec) break;
    if (!recognized) continue;
    bool removed = true;
    for (const auto &[path, identity] : candidates) {
      if (!checkpoint(check)) { report.cancelled = true; return; }
      if (ReadFileIdentity(path, error, paths.pathAccessRoots) != identity || !dependencies.remove(path, error)) {
        if (error.empty()) error = "Abandoned candidate changed during cleanup";
        failed(path); removed = false; break;
      }
    }
    if (removed && (!dependencies.remove(job, error) || !dependencies.syncDirectory(paths.temporaryRoot, error)))
      failed(job);
  }
  if (ec) { error = ec.message(); failed(paths.temporaryRoot); }
}
bool sameObject(const FileIdentity &a, const FileIdentity &b) {
  return a.device == b.device && a.fileId == b.fileId;
}
json identityJson(const FileIdentity &identity) {
  return {{"bytes", identity.bytes}, {"modified", identity.modified.time_since_epoch().count()},
          {"device", identity.device}, {"fileId", identity.fileId}};
}
FileIdentity readIdentity(const json &value) {
  for (const auto *key : {"bytes", "device", "fileId"}) {
    if (!value.at(key).is_number_unsigned()) throw std::runtime_error("Invalid journal identity");
  }
  if (!value.at("modified").is_number_integer()) throw std::runtime_error("Invalid journal timestamp");
  return {value.at("bytes").get<std::uint64_t>(),
          fs::file_time_type(fs::file_time_type::duration(value.at("modified").get<std::int64_t>())),
          value.at("device").get<std::uint64_t>(), value.at("fileId").get<std::uint64_t>()};
}
bool writeRecord(const Record &record, std::string &error) {
  const auto encoded = json{{"version", record.reuse ? 2 : 1}, {"id", record.id}, {"reuse", record.reuse},
      {"operation", static_cast<int>(record.operation)}, {"phase", static_cast<int>(record.phase)},
      {"source", utf8(record.source)}, {"destination", utf8(record.destination)},
      {"temporary", utf8(record.temporary)}, {"backup", utf8(record.backup)},
      {"sourceIdentity", identityJson(record.sourceIdentity)}, {"outputIdentity", identityJson(record.outputIdentity)},
      {"sourceHash", record.sourceHash}, {"outputHash", record.outputHash}}.dump();
  const auto operations = atomic_file::privateFileOperations();
  return atomic_file::writeWithoutBackup(record.journal,
      std::as_bytes(std::span(encoded.data(), encoded.size())), error, &operations);
}
bool validHash(const std::string &hash) {
  return hash.size() == 64 && std::ranges::all_of(hash, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool authorized(const fs::path &source, const StoragePlatformPaths &paths, std::string &error) {
  if (!source.is_absolute() || source != source.lexically_normal() ||
      !std::ranges::any_of(paths.allowedRoots, [&](const auto &root) {
        return root.is_absolute() && IsWithinLocalFolder(source, root.lexically_normal());
      }) || !IsSafeLocalPath(source.parent_path(), error, paths.pathAccessRoots)) {
    error = "Transaction is outside an authorized local folder"; return false;
  }
  return true;
}
bool validTargets(const Record &record, const StoragePlatformPaths &paths, std::string &error,
                  bool beforeCreation = false) {
  if (!uuid::isCanonicalLowerV4(record.id) || !authorized(record.source, paths, error)) return false;
  const auto parent = record.source.parent_path();
  if (record.journal != paths.journalRoot / (record.id + ".json") ||
      record.backup != parent / (".asobmashow-storage-" + record.id + ".backup") ||
      !validHash(record.sourceHash) || record.sourceIdentity.fileId == 0) return false;
  const auto extension = asobmshow::chart_assets::LowerMediaExtension(record.source);
  auto expected = record.source;
  switch (record.operation) {
  case AssetOperation::LosslessFlac:
    if (extension != "wav") return false;
    expected.replace_extension("flac"); break;
  case AssetOperation::LossyVorbis:
    if (extension != "wav" && extension != "flac") return false;
    expected.replace_extension("ogg"); break;
  case AssetOperation::CompressH264:
    if (asobmshow::chart_assets::ClassifyMediaPath(record.source) != asobmshow::chart_assets::MediaKind::Video) return false;
    if (extension != "mp4") expected.replace_extension("mp4");
    break;
  case AssetOperation::Delete:
    return !record.reuse && asobmshow::chart_assets::ClassifyMediaPath(record.source) == asobmshow::chart_assets::MediaKind::Video &&
           record.destination == record.source && record.temporary.empty();
  default: return false;
  }
  if (record.reuse) return record.destination == expected && record.destination != record.source &&
      record.temporary.empty() && validHash(record.outputHash) && record.outputIdentity.fileId != 0 &&
      record.outputIdentity.bytes > 0 && record.phase != Phase::Copying && record.phase != Phase::Published;
  return record.destination == expected && validHash(record.outputHash) &&
         record.temporary == parent / (".asobmashow-storage-" + record.id + ".install") &&
         (beforeCreation ? record.phase == Phase::Copying : record.outputIdentity.fileId != 0) &&
         (record.phase == Phase::Copying || (record.outputIdentity.bytes > 0 &&
                                             (record.operation == AssetOperation::LosslessFlac ||
                                              record.outputIdentity.bytes < record.sourceIdentity.bytes)));
}
std::optional<Record> loadRecord(const fs::path &path, const StoragePlatformPaths &paths, std::string &error) {
  std::error_code ec;
  if (!IsSafeLocalPath(path, error, paths.pathAccessRoots) || fs::file_size(path, ec) > 65536 || ec) return {};
  try {
    std::ifstream stream(path, std::ios::binary);
    json value;
    stream >> value;
    if (!value.at("version").is_number_integer() || (value.at("version") != 1 && value.at("version") != 2) ||
        !value.at("operation").is_number_integer() || !value.at("phase").is_number_integer() ||
        value.at("operation") < 0 || value.at("operation") > static_cast<int>(AssetOperation::Delete) ||
        value.at("phase") < 0 || value.at("phase") > static_cast<int>(Phase::Committed)) {
      error = "Unknown or malformed journal version/state"; return {};
    }
    Record record;
    if (value.contains("reuse") && !value.at("reuse").is_boolean()) throw std::runtime_error("Invalid retained destination state");
    record.reuse = value.value("reuse", false);
    if (record.reuse != (value.at("version") == 2)) throw std::runtime_error("Invalid retained destination journal version");
    record.id = value.at("id").get<std::string>();
    record.operation = static_cast<AssetOperation>(value.at("operation").get<int>());
    record.phase = static_cast<Phase>(value.at("phase").get<int>());
    record.source = pathFromUtf8(value.at("source"));
    record.destination = pathFromUtf8(value.at("destination"));
    record.temporary = pathFromUtf8(value.at("temporary"));
    record.backup = pathFromUtf8(value.at("backup"));
    for (auto *target : {&record.source, &record.destination, &record.temporary, &record.backup}) {
      if (target->empty()) continue;
      // The trusted platform mapping may normalize a relocated container name,
      // but must never turn a malformed journal path into an authorized one.
      if (!target->is_absolute() || *target != target->lexically_normal())
        throw std::runtime_error("Invalid journal path");
      if (paths.resolveRecoveryPath) *target = paths.resolveRecoveryPath(*target);
      if (!target->is_absolute() || *target != target->lexically_normal())
        throw std::runtime_error("Invalid relocated journal path");
    }
    record.journal = path;
    record.sourceIdentity = readIdentity(value.at("sourceIdentity"));
    record.outputIdentity = readIdentity(value.at("outputIdentity"));
    record.sourceHash = value.at("sourceHash").get<std::string>();
    record.outputHash = value.at("outputHash").get<std::string>();
    if (record.phase < Phase::Copying || record.phase > Phase::Committed || !validTargets(record, paths, error)) {
      error = "Invalid journal targets or state"; return {};
    }
    return record;
  } catch (const std::exception &exception) {
    error = exception.what(); return {};
  }
}
bool matches(const fs::path &path, const FileIdentity &expected, const std::string &hash,
             const FileTransactionDependencies &ops, bool content, std::string &error,
             Checkpoint check = {}) {
  const auto identity = ops.identity(path, error);
  if (!identity || !sameObject(*identity, expected)) return false;
  if (!content) return true;
  if (identity->bytes != expected.bytes || identity->modified != expected.modified) return false;
  const auto actual = HashStorageFile(path, std::move(check), error);
  return actual && *actual == hash;
}
bool removeOwned(const fs::path &path, const FileIdentity &identity, const std::string &hash,
                 const FileTransactionDependencies &ops, bool content, std::string &error,
                 std::vector<StorageFileChange> *changes = nullptr) {
  if (!entryExists(path)) return true;
  if (!matches(path, identity, hash, ops, content, error)) {
    error = "Recorded path has an unrelated or changed file"; return false;
  }
  if (!ops.remove(path, error)) return false;
  if (changes) changes->push_back({path, std::nullopt});
  return ops.syncDirectory(path.parent_path(), error);
}
bool finishRecord(const Record &record, const FileTransactionDependencies &ops,
                  std::uint64_t &reclaimed, std::uint64_t &added, std::string &error,
                  std::vector<StorageFileChange> *changes = nullptr) {
  const bool deletion = record.operation == AssetOperation::Delete;
  const bool hadBackup = entryExists(record.backup);
  if (record.phase == Phase::Committed) {
    if (!deletion && !matches(record.destination, record.outputIdentity, record.outputHash, ops, true, error)) {
      error = "Installed output changed before recovery"; return false;
    }
    if (!removeOwned(record.backup, record.sourceIdentity, record.sourceHash, ops, true, error, changes)) return false;
    if (hadBackup) AccumulateStorageBytes(reclaimed, added, record.sourceIdentity.bytes,
                                          deletion || record.reuse ? 0 : record.outputIdentity.bytes);
  } else {
    if (hadBackup && !matches(record.backup, record.sourceIdentity, record.sourceHash, ops, true, error)) {
      error = "Recovery backup changed"; return false;
    }
    if (!hadBackup && !entryExists(record.source)) {
      error = "Original and recovery backup are both missing"; return false;
    }
    if (hadBackup && record.destination == record.source && entryExists(record.source) &&
        (deletion || !matches(record.source, record.outputIdentity, record.outputHash, ops, true, error))) {
      error = "Source path occupied by an unrelated file during rollback"; return false;
    }
    if (!deletion && !record.reuse && entryExists(record.destination) &&
        matches(record.destination, record.outputIdentity, record.outputHash, ops, true, error)) {
      if (!hadBackup && record.destination == record.source) {
        error = "Cannot roll back replacement without its backup"; return false;
      }
      if (!ops.remove(record.destination, error)) return false;
      if (changes) changes->push_back({record.destination, std::nullopt});
      if (!ops.syncDirectory(record.destination.parent_path(), error)) return false;
    }
    if (hadBackup) {
      if (!ops.renameNoReplace(record.backup, record.source, error)) return false;
      if (changes) {
        changes->push_back({record.backup, std::nullopt});
        // In-place rollback may have removed this path above. Publish its
        // final restored identity once, while retaining partial failure paths.
        const auto source = std::ranges::find_if(*changes, [&](const auto &change) { return change.path == record.source; });
        if (source != changes->end()) source->after = record.sourceIdentity;
        else changes->push_back({record.source, record.sourceIdentity});
      }
      if (!ops.syncDirectory(record.source.parent_path(), error)) return false;
    }
  }
  if (!record.temporary.empty() && !removeOwned(record.temporary, record.outputIdentity, record.outputHash,
                                               ops, record.phase != Phase::Copying, error, changes)) return false;
  return ops.remove(record.journal, error) && ops.syncDirectory(record.journal.parent_path(), error);
}
void fillDefaults(FileTransactionDependencies &ops) {
  const auto defaults = DefaultFileTransactionDependencies();
  if (!ops.identity) ops.identity = defaults.identity;
  if (!ops.createExclusive) ops.createExclusive = defaults.createExclusive;
  if (!ops.copyAndSync) ops.copyAndSync = defaults.copyAndSync;
  if (!ops.preservePermissions) ops.preservePermissions = defaults.preservePermissions;
  if (!ops.renameNoReplace) ops.renameNoReplace = defaults.renameNoReplace;
  if (!ops.remove) ops.remove = defaults.remove;
  if (!ops.syncDirectory) ops.syncDirectory = defaults.syncDirectory;
}
} // namespace

StorageFileTransaction::StorageFileTransaction(StoragePlatformPaths paths, FileTransactionDependencies dependencies)
    : paths_(std::move(paths)), dependencies_(std::move(dependencies)) {
  if (paths_.journalSyncRoot.empty()) {
    paths_.journalSyncRoot = paths_.journalRoot.parent_path();
    std::error_code error;
    while (!paths_.journalSyncRoot.empty() && !fs::exists(paths_.journalSyncRoot, error) && !error) {
      const auto parent = paths_.journalSyncRoot.parent_path();
      if (parent == paths_.journalSyncRoot) break;
      paths_.journalSyncRoot = parent;
    }
  }
  if (!dependencies_.identity) dependencies_.identity = [roots = paths_.pathAccessRoots](const fs::path &path, std::string &error) {
    return ReadFileIdentity(path, error, roots);
  };
  fillDefaults(dependencies_);
}
bool StorageFileTransaction::PrepareJournalDirectory(std::string &error) {
  std::error_code ec;
  fs::create_directories(paths_.journalRoot, ec);
  if (ec) { error = ec.message(); return false; }
  if (!IsSafeLocalPath(paths_.journalRoot, error, paths_.pathAccessRoots)) return false;
  if (paths_.journalSyncRoot.empty() || !IsWithinFolder(paths_.journalRoot, paths_.journalSyncRoot)) {
    error = "Journal durability root is not an ancestor"; return false;
  }
  // Repeat the complete captured barrier even after a failed attempt created
  // these directories, and before any worker can retire a source. Syncing only
  // journalRoot does not persist its own link in the newly created parent.
  for (auto directory = paths_.journalRoot;; directory = directory.parent_path()) {
    if (!dependencies_.syncDirectory(directory, error)) return false;
    if (directory == paths_.journalSyncRoot) return true;
  }
}

FileResult StorageFileTransaction::Replace(const AssetCandidate &candidate, const EncodeResult &encoded, Checkpoint check) {
  return Run(candidate, &encoded, std::move(check));
}
FileResult StorageFileTransaction::ReuseExisting(const AssetCandidate &candidate, Checkpoint check) {
  return Run(candidate, nullptr, std::move(check), true);
}
FileResult StorageFileTransaction::Delete(const AssetCandidate &candidate, Checkpoint check) {
  return Run(candidate, nullptr, std::move(check));
}

FileResult StorageFileTransaction::Run(const AssetCandidate &candidate, const EncodeResult *encoded, Checkpoint check, bool reuse) {
  const bool deletion = encoded == nullptr && !reuse;
  FileResult result{.source = candidate.source, .destination = candidate.destination,
                    .outcome = FileOutcome::Failed, .bytesBefore = candidate.identity.bytes};
  std::string error;
  if (!checkpoint(check)) { result.outcome = FileOutcome::Cancelled; result.reason.code = StorageReason::Cancelled; return result; }
  const auto fail = [&](StorageReason reason, std::string detail) {
    if (reason == StorageReason::Cancelled) result.outcome = FileOutcome::Cancelled;
    result.reason = {reason, std::move(detail)}; return result;
  };
  if (reuse && (!candidate.existingDestinationIdentity || !validHash(candidate.existingDestinationHash) ||
                candidate.operation == AssetOperation::Delete || candidate.source == candidate.destination))
    return fail(StorageReason::ValidationFailed, "Missing validated existing destination");
  if (!authorized(candidate.source, paths_, error)) return fail(StorageReason::UnsafePath, error);
  const auto sourceIdentity = dependencies_.identity(candidate.source, error);
  if (!sourceIdentity || *sourceIdentity != candidate.identity) return fail(StorageReason::StaleInput, "Source changed since inventory");
  auto sourceHash = HashStorageFile(candidate.source, check, error);
  if (!sourceHash) return fail(checkpoint(check) ? StorageReason::IoError : StorageReason::Cancelled, error);
  if (!candidate.sourceHash.empty() && candidate.sourceHash != *sourceHash) return fail(StorageReason::StaleInput, "Source changed during encoding");
  Record record;
  record.id = uuid::generateV4();
  record.source = candidate.source;
  record.destination = deletion ? candidate.source : candidate.destination;
  record.operation = deletion ? AssetOperation::Delete : candidate.operation;
  record.reuse = reuse;
  record.sourceIdentity = *sourceIdentity;
  record.sourceHash = *sourceHash;
  record.backup = record.source.parent_path() / (".asobmashow-storage-" + record.id + ".backup");
  record.journal = paths_.journalRoot / (record.id + ".json");
  std::error_code ec;
  std::string creationError;
  if (!PrepareJournalDirectory(error)) return fail(StorageReason::IoError, error);
  if (reuse) {
    record.outputIdentity = *candidate.existingDestinationIdentity;
    record.outputHash = candidate.existingDestinationHash;
    if (!matches(record.destination, record.outputIdentity, record.outputHash, dependencies_, true, error, check))
      return fail(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled,
                  "Existing destination changed since validation");
  } else if (!deletion) {
    if (encoded->outcome != FileOutcome::Converted) return fail(StorageReason::ValidationFailed, "No validated encoded output");
    const auto outputIdentity = dependencies_.identity(encoded->outputPath, error);
    if (!outputIdentity || outputIdentity->bytes == 0 ||
        (candidate.operation != AssetOperation::LosslessFlac && outputIdentity->bytes >= sourceIdentity->bytes))
      return fail(StorageReason::NotSmaller, "Encoded output is not smaller");
    auto hash = HashStorageFile(encoded->outputPath, check, error);
    if (!hash) return fail(checkpoint(check) ? StorageReason::IoError : StorageReason::Cancelled, error);
    record.outputHash = *hash;
    record.temporary = record.source.parent_path() / (".asobmashow-storage-" + record.id + ".install");
    record.phase = Phase::Copying;
    // Reject invalid destination/operation combinations before creating a file
    // whose identity is not available yet. Revalidate again after creation.
    if (!validTargets(record, paths_, error, true))
      return fail(StorageReason::UnsafePath, "Unsupported transaction target");
    const auto space = fs::space(record.source.parent_path(), ec);
    if (ec || space.available < outputIdentity->bytes) return fail(StorageReason::InsufficientSpace, "Insufficient installation space");
    const auto created = dependencies_.createExclusive(record.temporary, error);
    if (!created.created) return fail(StorageReason::IoError, error);
    if (!created.identity.fileId) return fail(StorageReason::RecoveryRequired, error);
    record.outputIdentity = created.identity;
    if (!created.ready) creationError = error.empty() ? "Installation creation could not be flushed" : error;
  }
  if (!validTargets(record, paths_, error)) {
    std::string cleanupError;
    if (!record.temporary.empty() &&
        !removeOwned(record.temporary, record.outputIdentity, {}, dependencies_, false, cleanupError)) {
      // The static targets passed before creation, but their parent may now be
      // unavailable. Keep the owned identity so recovery can retry when safe.
      std::string journalError;
      if (!writeRecord(record, journalError)) cleanupError += "; " + journalError;
      return fail(StorageReason::RecoveryRequired, "Transaction target changed; " + cleanupError);
    }
    return fail(StorageReason::UnsafePath, "Unsupported transaction target");
  }
  if (!writeRecord(record, error)) {
    std::string cleanupError;
    const bool cleaned = record.temporary.empty() ||
        removeOwned(record.temporary, record.outputIdentity, {}, dependencies_, false, cleanupError);
    if (!cleaned && !cleanupError.empty()) error += "; " + cleanupError;
    // Atomic writing can fail before publication or after the journal rename.
    // Only a durably cleaned installation with no published journal is settled.
    return fail(cleaned && !entryExists(record.journal) ? StorageReason::IoError : StorageReason::RecoveryRequired, error);
  }
  const auto rollback = [&](StorageReason reason, std::string detail) {
    std::uint64_t ignored = 0, ignoredAdded = 0;
    std::string rollbackError;
    if (!finishRecord(record, dependencies_, ignored, ignoredAdded, rollbackError))
      return fail(StorageReason::RecoveryRequired, detail + "; " + rollbackError);
    if (reason == StorageReason::Cancelled) result.outcome = FileOutcome::Cancelled;
    return fail(reason, std::move(detail));
  };
  // Even a failed initial flush has an owned inode. Journal it before cleanup
  // so a second I/O failure leaves recoverable evidence, never an orphan.
  if (!creationError.empty()) return rollback(StorageReason::IoError, std::move(creationError));
  if (!deletion && !reuse) {
    if (dependencies_.faultPoint) dependencies_.faultPoint("copying");
    if (!dependencies_.copyAndSync(encoded->outputPath, record.temporary, record.outputIdentity, check, error))
      return rollback(checkpoint(check) ? StorageReason::IoError : StorageReason::Cancelled, error);
    if (dependencies_.faultPoint) dependencies_.faultPoint("copied");
    const auto installedIdentity = dependencies_.identity(record.temporary, error);
    const auto hash = HashStorageFile(record.temporary, check, error);
    if (!installedIdentity || !sameObject(*installedIdentity, record.outputIdentity) || !hash || *hash != record.outputHash)
      return rollback(checkpoint(check) ? StorageReason::ValidationFailed : StorageReason::Cancelled,
                      "Installation copy changed or verification cancelled");
    record.outputIdentity = *installedIdentity;
    if (!dependencies_.preservePermissions(record.source, record.sourceIdentity,
                                          record.temporary, record.outputIdentity, error))
      return rollback(StorageReason::IoError, error);
  }
  record.phase = Phase::Prepared;
  if (!writeRecord(record, error)) return rollback(StorageReason::IoError, error);
  if (dependencies_.faultPoint) dependencies_.faultPoint("prepared");
  const auto validateScope = [&](bool retired, bool published) {
    if (!dependencies_.beforeCommit) return true;
    std::vector<StorageFileChange> changes;
    if (!record.temporary.empty()) changes.push_back({record.temporary,
        published ? std::nullopt : std::optional<FileIdentity>(record.outputIdentity)});
    changes.push_back({record.backup, retired ? std::optional<FileIdentity>(record.sourceIdentity) : std::nullopt});
    if (retired && (deletion || record.source != record.destination)) changes.push_back({record.source, std::nullopt});
    if (published) changes.push_back({record.destination, record.outputIdentity});
    if (reuse) changes.push_back({record.destination, record.outputIdentity});
    return dependencies_.beforeCommit(candidate, changes, check, error);
  };
  if (!validateScope(false, false))
    return rollback(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled, error);
  if (!checkpoint(check)) return rollback(StorageReason::Cancelled, "Cancelled before commit");
  if (!matches(record.source, record.sourceIdentity, record.sourceHash, dependencies_, true, error, check))
    return rollback(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled, "Source verification interrupted before commit");
  const auto rename = [&](const fs::path &from, const fs::path &to, Phase phase, std::string_view point) {
    if (!dependencies_.renameNoReplace(from, to, error) || !dependencies_.syncDirectory(to.parent_path(), error)) return false;
    if (dependencies_.faultPoint) dependencies_.faultPoint(std::string(point) + "-before-journal");
    record.phase = phase;
    if (!writeRecord(record, error)) return false;
    if (dependencies_.faultPoint) dependencies_.faultPoint(point);
    return true;
  };
  if (!deletion && !reuse && record.destination != record.source) {
    if (!rename(record.temporary, record.destination, Phase::Published, "published")) return rollback(StorageReason::IoError, error);
  }
  if (!matches(record.source, record.sourceIdentity, record.sourceHash, dependencies_, true, error, check))
    return rollback(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled, "Source verification interrupted before retirement");
  if (reuse && !matches(record.destination, record.outputIdentity, record.outputHash, dependencies_, true, error, check))
    return rollback(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled, "Existing destination changed before retirement");
  if (!rename(record.source, record.backup, Phase::Retired, "retired")) return rollback(StorageReason::IoError, error);
  if (!deletion && !reuse && record.destination == record.source) {
    if (!rename(record.temporary, record.destination, Phase::Published, "published")) return rollback(StorageReason::IoError, error);
  }
  if (!checkpoint(check)) return rollback(StorageReason::Cancelled, "Cancelled before durable commit");
  if (!validateScope(true, !deletion && !reuse))
    return rollback(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled, error);
  if (reuse && !matches(record.destination, record.outputIdentity, record.outputHash, dependencies_, true, error, check))
    return rollback(checkpoint(check) ? StorageReason::StaleInput : StorageReason::Cancelled, "Existing destination changed before commit");
  record.phase = Phase::Committed;
  // A failed durability flush may have already published the committed record.
  // Leave both playable output and backup for restart instead of guessing.
  if (!writeRecord(record, error)) return fail(StorageReason::RecoveryRequired, error);
  if (dependencies_.faultPoint) dependencies_.faultPoint("committed");
  result.outcome = deletion || reuse ? FileOutcome::Deleted : FileOutcome::Converted;
  if (reuse) result.reason.detail = "Removed redundant source; retained validated existing destination";
  result.bytesAfter = deletion || reuse ? 0 : record.outputIdentity.bytes;
  if (deletion || record.source != record.destination)
    result.filesystemChanges.push_back({record.source, std::nullopt});
  if (!deletion && !reuse) result.filesystemChanges.push_back({record.destination, record.outputIdentity});
  if (!finishRecord(record, dependencies_, result.reclaimedBytes, result.addedBytes, error)) {
    const auto cleanupError = error;
    const bool outputIntact = deletion || matches(record.destination, record.outputIdentity,
                                                  record.outputHash, dependencies_, true, error);
    const bool backupIntact = !entryExists(record.backup) || matches(record.backup, record.sourceIdentity,
                                                                    record.sourceHash, dependencies_, true, error);
    result.reason = {outputIntact && backupIntact ? StorageReason::CleanupFailed : StorageReason::RecoveryRequired,
                     cleanupError};
    if (outputIntact && backupIntact && entryExists(record.backup))
      result.filesystemChanges.push_back({record.backup, record.sourceIdentity});
  }
  return result;
}

StorageReport StorageFileTransaction::Recover(Checkpoint check, const StorageRecoveryObserver &observer) {
  StorageReport report;
  std::set<fs::path> changedDirectories;
  std::error_code ec;
  std::string error;
  if (!PrepareJournalDirectory(error)) {
    report.recoveryRequired = true;
    report.failed = 1;
    RetainStorageReportDetail(report, {.source = paths_.journalRoot, .outcome = FileOutcome::Failed,
        .reason = {StorageReason::RecoveryRequired, error}});
    return report;
  }
  fs::directory_iterator it(paths_.journalRoot, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!checkpoint(check)) { report.cancelled = true; break; }
    // Atomic writer temporary files are not durable decisions. Preserve them;
    // recovery follows only the published .json record.
    if (it->path().extension() != ".json") continue;
    const auto record = loadRecord(it->path(), paths_, error);
    std::uint64_t reclaimed = 0, added = 0;
    // Each validated record changes at most its source, output, backup and
    // install paths. Publish exact owned changes before loading another record.
    std::vector<StorageFileChange> changes;
    if (record && observer.before) observer.before(record->source.parent_path());
    const bool finished = record && finishRecord(*record, dependencies_, reclaimed, added, error, &changes);
    if (!changes.empty()) changedDirectories.insert(record->source.parent_path());
    if (!finished) {
      report.recoveryRequired = true;
      ++report.failed;
      RetainStorageReportDetail(report, {.source = it->path(), .outcome = FileOutcome::Failed,
          .reason = {StorageReason::RecoveryRequired, error}});
    } else {
      AccumulateStorageBytes(report.reclaimedBytes, report.addedBytes, reclaimed, added);
      if (observer.after) observer.after(record->source.parent_path(), changes);
    }
  }
  report.changedRoots.assign(changedDirectories.begin(), changedDirectories.end());
  if (ec) report.recoveryRequired = true;
  if (!report.recoveryRequired && !report.cancelled) cleanupAbandonedCandidates(paths_, dependencies_, check, report);
  return report;
}

} // namespace storage_management
